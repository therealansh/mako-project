#ifndef _LIB_COLUMN_DELTA_H_
#define _LIB_COLUMN_DELTA_H_

#include <string>
#include <vector>
#include <tuple>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <functional>

namespace mako {

// Feature flag for column-delta MVCC optimization
// When enabled, updates store only changed columns instead of full rows
#ifndef MAKO_ENABLE_COLUMN_DELTAS
#define MAKO_ENABLE_COLUMN_DELTAS 1
#endif

// Runtime flag to enable/disable column-delta (for A/B comparison testing)
// This allows running baseline vs modified in the same binary
inline std::atomic<bool>& columnDeltasEnabled() {
    static std::atomic<bool> enabled{true};
    return enabled;
}

inline void setColumnDeltasEnabled(bool enabled) {
    columnDeltasEnabled().store(enabled);
}

inline bool isColumnDeltasEnabled() {
    return columnDeltasEnabled().load();
}

// Value kind byte for distinguishing value types in MVCC chain
// Layout: [kind byte][payload][timestamp+term][Node]
enum ValueKind : uint8_t {
    LEGACY_BASE = 0,    // Legacy full row (no kind byte prefix, for backward compat)
    COL_BASE    = 1,    // Full row with column-delta format support
    COL_DELTA   = 2     // Column delta (only changed columns stored)
};

// Size of the kind byte prefix (1 byte)
const int KIND_BYTE_SIZE = sizeof(uint8_t);

// Maximum number of columns that can be stored in a delta
const int MAX_DELTA_COLUMNS = 32;

// Threshold: if more than this many columns change, use full row instead of delta
const int DELTA_COLUMN_THRESHOLD = 8;

// Count the number of set bits in a changed_fields bitmask
// Used to determine how many columns changed in an update
inline int countChangedFields(uint32_t changed_fields) {
    int count = 0;
    while (changed_fields) {
        count += changed_fields & 1;
        changed_fields >>= 1;
    }
    return count;
}

// Metrics for column-delta evaluation
struct ColumnDeltaMetrics {
    std::atomic<uint64_t> total_updates{0};
    std::atomic<uint64_t> delta_updates{0};
    std::atomic<uint64_t> full_row_updates{0};
    std::atomic<uint64_t> bytes_full_row{0};
    std::atomic<uint64_t> bytes_delta{0};
    std::atomic<uint64_t> bytes_saved{0};
    
    // Per-table metrics for detailed analysis
    std::atomic<uint64_t> customer_updates{0};
    std::atomic<uint64_t> warehouse_updates{0};
    std::atomic<uint64_t> district_updates{0};
    std::atomic<uint64_t> ycsb_updates{0};
    
    void reset() {
        total_updates = 0;
        delta_updates = 0;
        full_row_updates = 0;
        bytes_full_row = 0;
        bytes_delta = 0;
        bytes_saved = 0;
        customer_updates = 0;
        warehouse_updates = 0;
        district_updates = 0;
        ycsb_updates = 0;
    }
    
    void print() const {
        printf("\n=== Column-Delta MVCC Metrics ===\n");
        printf("Total updates: %lu\n", total_updates.load());
        printf("Delta updates: %lu (%.2f%%)\n", delta_updates.load(), 
               total_updates.load() > 0 ? 100.0 * delta_updates.load() / total_updates.load() : 0.0);
        printf("Full row updates: %lu (%.2f%%)\n", full_row_updates.load(),
               total_updates.load() > 0 ? 100.0 * full_row_updates.load() / total_updates.load() : 0.0);
        printf("Bytes (full row): %lu\n", bytes_full_row.load());
        printf("Bytes (delta): %lu\n", bytes_delta.load());
        printf("Bytes saved: %lu (%.2f%%)\n", bytes_saved.load(),
               bytes_full_row.load() > 0 ? 100.0 * bytes_saved.load() / bytes_full_row.load() : 0.0);
        printf("--- Per-table breakdown ---\n");
        printf("Customer updates: %lu\n", customer_updates.load());
        printf("Warehouse updates: %lu\n", warehouse_updates.load());
        printf("District updates: %lu\n", district_updates.load());
        printf("YCSB updates: %lu\n", ycsb_updates.load());
        printf("=================================\n\n");
    }
    
    // Get metrics as JSON string for evaluation scripts
    std::string toJson() const {
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "{"
            "\"total_updates\": %lu, "
            "\"delta_updates\": %lu, "
            "\"full_row_updates\": %lu, "
            "\"bytes_full_row\": %lu, "
            "\"bytes_delta\": %lu, "
            "\"bytes_saved\": %lu, "
            "\"delta_ratio\": %.4f, "
            "\"savings_ratio\": %.4f"
            "}",
            total_updates.load(),
            delta_updates.load(),
            full_row_updates.load(),
            bytes_full_row.load(),
            bytes_delta.load(),
            bytes_saved.load(),
            total_updates.load() > 0 ? (double)delta_updates.load() / total_updates.load() : 0.0,
            bytes_full_row.load() > 0 ? (double)bytes_saved.load() / bytes_full_row.load() : 0.0
        );
        return std::string(buf);
    }
};

inline ColumnDeltaMetrics& getColumnDeltaMetrics() {
    static ColumnDeltaMetrics metrics;
    return metrics;
}

// ============================================================================
// Column Delta Serialization/Deserialization
// ============================================================================

// Column delta format:
// [COL_DELTA kind byte][num_columns (1 byte)][column entries...]
// Each column entry: [column_id (1 byte)][value_len (2 bytes)][value data]

class ColumnDelta {
public:
    // Maximum size for a single column value
    static constexpr size_t MAX_COLUMN_VALUE_SIZE = 256;
    
    // Header size per column entry: column_id (1) + value_len (2)
    static constexpr size_t ENTRY_HEADER_SIZE = sizeof(uint8_t) + sizeof(uint16_t);
    
    // Build a delta from a list of (column_id, value_ptr, value_len) tuples
    // Returns the serialized delta string (including COL_DELTA kind byte)
    static std::string buildDelta(
        const std::vector<std::tuple<uint8_t, const char*, uint16_t>>& columns) {
        
        if (columns.empty() || columns.size() > MAX_DELTA_COLUMNS) {
            return "";
        }
        
        // Calculate total size needed
        size_t total_size = KIND_BYTE_SIZE + 1; // kind byte + num_columns
        for (const auto& col : columns) {
            total_size += ENTRY_HEADER_SIZE + std::get<2>(col);
        }
        
        std::string result;
        result.resize(total_size);
        char* ptr = &result[0];
        
        // Write kind byte
        *ptr++ = static_cast<char>(COL_DELTA);
        
        // Write number of columns
        *ptr++ = static_cast<char>(columns.size());
        
        // Write each column entry
        for (const auto& col : columns) {
            uint8_t col_id = std::get<0>(col);
            const char* value_ptr = std::get<1>(col);
            uint16_t value_len = std::get<2>(col);
            
            // Write column ID
            *ptr++ = static_cast<char>(col_id);
            
            // Write value length (little-endian)
            memcpy(ptr, &value_len, sizeof(uint16_t));
            ptr += sizeof(uint16_t);
            
            // Write value data
            memcpy(ptr, value_ptr, value_len);
            ptr += value_len;
        }
        
        return result;
    }
    
    // Parse a delta and return column entries as (column_id, value_offset, value_len)
    // The value_offset is relative to the start of the delta data (after kind byte)
    static bool parseDelta(
        const char* data,
        size_t data_len,
        std::vector<std::tuple<uint8_t, size_t, uint16_t>>& columns) {
        
        columns.clear();
        
        if (data_len < KIND_BYTE_SIZE + 1) {
            return false;
        }
        
        const char* ptr = data;
        
        // Check kind byte
        if (static_cast<uint8_t>(*ptr++) != COL_DELTA) {
            return false;
        }
        
        // Read number of columns
        uint8_t num_columns = static_cast<uint8_t>(*ptr++);
        if (num_columns == 0 || num_columns > MAX_DELTA_COLUMNS) {
            return false;
        }
        
        columns.reserve(num_columns);
        
        // Parse each column entry
        for (uint8_t i = 0; i < num_columns; i++) {
            if (ptr + ENTRY_HEADER_SIZE > data + data_len) {
                columns.clear();
                return false;
            }
            
            // Read column ID
            uint8_t col_id = static_cast<uint8_t>(*ptr++);
            
            // Read value length
            uint16_t value_len;
            memcpy(&value_len, ptr, sizeof(uint16_t));
            ptr += sizeof(uint16_t);
            
            if (ptr + value_len > data + data_len) {
                columns.clear();
                return false;
            }
            
            // Store offset relative to data start
            size_t value_offset = ptr - data;
            columns.emplace_back(col_id, value_offset, value_len);
            
            ptr += value_len;
        }
        
        return true;
    }
};

// ============================================================================
// Generic Field Comparator using value_descriptor
// ============================================================================

// Type trait to get the parent struct type from a value type
// This allows us to access the value_descriptor
// Usage: parent_struct<customer::value>::type == customer
template<typename T>
struct parent_struct {
    // Default: no parent struct known
    // Specializations are generated by DO_STRUCT macro or manually defined
};

// Generic FieldComparator that works with any DO_STRUCT-generated type
// Uses value_descriptor to get field offsets and sizes at compile time
template<typename ValueType, typename ParentStruct = typename parent_struct<ValueType>::type>
struct GenericFieldComparator {
    using Descriptor = typename ParentStruct::value_descriptor;
    
    // Compare two values and return a bitmask of changed fields
    static uint32_t compare(const ValueType& old_val, const ValueType& new_val) {
        uint32_t changed = 0;
        const size_t nfields = Descriptor::nfields();
        
        for (size_t i = 0; i < nfields && i < 32; ++i) {
            size_t offset = Descriptor::cstruct_offsetof(i);
            size_t size = Descriptor::cstruct_sizeof(i);
            
            const char* old_ptr = reinterpret_cast<const char*>(&old_val) + offset;
            const char* new_ptr = reinterpret_cast<const char*>(&new_val) + offset;
            
            if (std::memcmp(old_ptr, new_ptr, size) != 0) {
                changed |= (1u << i);
            }
        }
        
        return changed;
    }
    
    // Get the offset of a field by its ID
    static size_t getFieldOffset(uint8_t field_id) {
        return Descriptor::cstruct_offsetof(field_id);
    }
    
    // Get the size of a field by its ID
    static size_t getFieldSize(uint8_t field_id) {
        return Descriptor::cstruct_sizeof(field_id);
    }
    
    // Get the number of fields
    static size_t getNumFields() {
        return Descriptor::nfields();
    }
};

// Default FieldComparator template - uses GenericFieldComparator when parent_struct is defined
// Falls back to comparing all fields as changed when parent_struct is not defined
template<typename T, typename = void>
struct FieldComparator {
    static uint32_t compare(const T& old_val, const T& new_val) {
        // Default: assume all fields changed (conservative fallback)
        return 0xFFFFFFFF;
    }
    
    static size_t getFieldOffset(uint8_t field_id) {
        return 0;
    }
    
    static size_t getFieldSize(uint8_t field_id) {
        return 0;
    }
    
    static size_t getNumFields() {
        return 0;
    }
};

// Specialization for types that have parent_struct defined
template<typename T>
struct FieldComparator<T, std::void_t<typename parent_struct<T>::type>> 
    : GenericFieldComparator<T> {};

// ============================================================================
// Delta Building from Struct Values
// ============================================================================

// Build a column delta from old and new struct values
// Uses FieldComparator to get field metadata
template<typename ValueType>
std::string buildDeltaFromStructs(const ValueType& old_val, const ValueType& new_val, uint32_t changed_fields) {
    using Comparator = FieldComparator<ValueType>;
    
    std::vector<std::tuple<uint8_t, const char*, uint16_t>> columns;
    
    for (uint8_t i = 0; i < 32 && i < Comparator::getNumFields(); ++i) {
        if (changed_fields & (1u << i)) {
            size_t offset = Comparator::getFieldOffset(i);
            size_t size = Comparator::getFieldSize(i);
            
            if (size > 0 && size <= ColumnDelta::MAX_COLUMN_VALUE_SIZE) {
                const char* value_ptr = reinterpret_cast<const char*>(&new_val) + offset;
                columns.emplace_back(i, value_ptr, static_cast<uint16_t>(size));
            }
        }
    }
    
    if (columns.empty()) {
        return "";
    }
    
    return ColumnDelta::buildDelta(columns);
}

// Apply a delta to a base value to reconstruct the full row
// Returns true if successful
template<typename ValueType>
bool applyDeltaToStruct(const char* delta_data, size_t delta_len, ValueType& value) {
    using Comparator = FieldComparator<ValueType>;
    
    std::vector<std::tuple<uint8_t, size_t, uint16_t>> columns;
    if (!ColumnDelta::parseDelta(delta_data, delta_len, columns)) {
        return false;
    }
    
    for (const auto& col : columns) {
        uint8_t col_id = std::get<0>(col);
        size_t value_offset = std::get<1>(col);
        uint16_t value_len = std::get<2>(col);
        
        size_t field_offset = Comparator::getFieldOffset(col_id);
        size_t field_size = Comparator::getFieldSize(col_id);
        
        if (field_size == 0 || value_len != field_size) {
            return false; // Size mismatch
        }
        
        char* dest = reinterpret_cast<char*>(&value) + field_offset;
        const char* src = delta_data + value_offset;
        std::memcpy(dest, src, value_len);
    }
    
    return true;
}

// ============================================================================
// Delta Context for MVCC Integration
// ============================================================================

// Thread-local context for passing delta information from benchmark to MVCC layer
// This allows the benchmark code to provide decoded old/new values and changed_fields
// which mvInstall can use to create actual COL_DELTA nodes
struct DeltaContext {
    bool active{false};                    // Whether delta context is set
    const void* old_value{nullptr};        // Pointer to decoded old value struct
    const void* new_value{nullptr};        // Pointer to decoded new value struct
    uint32_t changed_fields{0};            // Bitmask of changed fields
    size_t value_size{0};                  // Size of the value struct
    
    // Function pointer for building delta from the stored values
    // This is set by the benchmark layer to the appropriate buildDeltaFromStructs instantiation
    std::string (*build_delta_fn)(const void*, const void*, uint32_t){nullptr};
    
    // Function pointer for applying delta to a struct
    bool (*apply_delta_fn)(const char*, size_t, void*){nullptr};
    
    void reset() {
        active = false;
        old_value = nullptr;
        new_value = nullptr;
        changed_fields = 0;
        value_size = 0;
        build_delta_fn = nullptr;
        apply_delta_fn = nullptr;
    }
};

inline DeltaContext& getDeltaContext() {
    thread_local DeltaContext ctx;
    return ctx;
}

// RAII helper to set and clear delta context
template<typename ValueType>
class ScopedDeltaContext {
public:
    ScopedDeltaContext(const ValueType& old_val, const ValueType& new_val, uint32_t changed) {
        auto& ctx = getDeltaContext();
        ctx.active = true;
        ctx.old_value = &old_val;
        ctx.new_value = &new_val;
        ctx.changed_fields = changed;
        ctx.value_size = sizeof(ValueType);
        ctx.build_delta_fn = &buildDeltaWrapper<ValueType>;
        ctx.apply_delta_fn = &applyDeltaWrapper<ValueType>;
    }
    
    ~ScopedDeltaContext() {
        getDeltaContext().reset();
    }
    
private:
    template<typename T>
    static std::string buildDeltaWrapper(const void* old_ptr, const void* new_ptr, uint32_t changed) {
        const T* old_val = static_cast<const T*>(old_ptr);
        const T* new_val = static_cast<const T*>(new_ptr);
        return buildDeltaFromStructs(*old_val, *new_val, changed);
    }
    
    template<typename T>
    static bool applyDeltaWrapper(const char* delta_data, size_t delta_len, void* value_ptr) {
        T* value = static_cast<T*>(value_ptr);
        return applyDeltaToStruct(delta_data, delta_len, *value);
    }
};

} // namespace mako

#endif // _LIB_COLUMN_DELTA_H_
