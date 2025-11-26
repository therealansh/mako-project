#ifndef _LIB_COLUMN_DELTA_H_
#define _LIB_COLUMN_DELTA_H_

#include "common.h"
#include <cstring>
#include <vector>
#include <utility>

namespace mako {

// Column delta entry: (column_id, value_length, value_bytes)
struct ColumnDeltaEntry {
    uint8_t column_id;
    uint16_t value_length;
    // value_bytes follow immediately after in the serialized format
};

// Column delta header for COL_DELTA values
// Layout: [kind=COL_DELTA][num_columns][entries...]
// Each entry: [column_id:1][value_length:2][value_bytes:value_length]
struct ColumnDeltaHeader {
    uint8_t kind;        // Should be COL_DELTA (2)
    uint8_t num_columns; // Number of column entries
    // Entries follow immediately after
};

// Helper class for building and parsing column deltas
class ColumnDelta {
public:
    // Maximum size of a single column value in delta encoding
    static constexpr size_t MAX_COLUMN_VALUE_SIZE = 256;
    
    // Entry size overhead (column_id + value_length)
    static constexpr size_t ENTRY_HEADER_SIZE = sizeof(uint8_t) + sizeof(uint16_t);
    
    // Build a column delta from a list of (column_id, value_ptr, value_len) tuples
    // Returns the serialized delta string (without MVCC metadata)
    static std::string buildDelta(
        const std::vector<std::tuple<uint8_t, const char*, uint16_t>>& columns) {
        
        if (columns.empty() || columns.size() > MAX_DELTA_COLUMNS) {
            return ""; // Invalid delta
        }
        
        // Calculate total size needed
        size_t total_size = sizeof(ColumnDeltaHeader);
        for (const auto& col : columns) {
            total_size += ENTRY_HEADER_SIZE + std::get<2>(col);
        }
        
        std::string result;
        result.resize(total_size);
        char* ptr = result.data();
        
        // Write header
        ColumnDeltaHeader* header = reinterpret_cast<ColumnDeltaHeader*>(ptr);
        header->kind = COL_DELTA;
        header->num_columns = static_cast<uint8_t>(columns.size());
        ptr += sizeof(ColumnDeltaHeader);
        
        // Write each column entry
        for (const auto& col : columns) {
            uint8_t col_id = std::get<0>(col);
            const char* value_ptr = std::get<1>(col);
            uint16_t value_len = std::get<2>(col);
            
            // Write column_id
            *reinterpret_cast<uint8_t*>(ptr) = col_id;
            ptr += sizeof(uint8_t);
            
            // Write value_length
            *reinterpret_cast<uint16_t*>(ptr) = value_len;
            ptr += sizeof(uint16_t);
            
            // Write value_bytes
            std::memcpy(ptr, value_ptr, value_len);
            ptr += value_len;
        }
        
        return result;
    }
    
    // Parse a column delta and return the list of (column_id, value_offset, value_len)
    // value_offset is relative to the start of the delta data
    static bool parseDelta(
        const char* data,
        size_t data_len,
        std::vector<std::tuple<uint8_t, size_t, uint16_t>>& columns) {
        
        columns.clear();
        
        if (data_len < sizeof(ColumnDeltaHeader)) {
            return false;
        }
        
        const ColumnDeltaHeader* header = reinterpret_cast<const ColumnDeltaHeader*>(data);
        if (header->kind != COL_DELTA) {
            return false;
        }
        
        size_t offset = sizeof(ColumnDeltaHeader);
        for (uint8_t i = 0; i < header->num_columns; i++) {
            if (offset + ENTRY_HEADER_SIZE > data_len) {
                return false; // Truncated data
            }
            
            uint8_t col_id = *reinterpret_cast<const uint8_t*>(data + offset);
            offset += sizeof(uint8_t);
            
            uint16_t value_len = *reinterpret_cast<const uint16_t*>(data + offset);
            offset += sizeof(uint16_t);
            
            if (offset + value_len > data_len) {
                return false; // Truncated data
            }
            
            columns.emplace_back(col_id, offset, value_len);
            offset += value_len;
        }
        
        return true;
    }
    
    // Get the kind byte from a value buffer
    // Returns LEGACY_BASE if the buffer doesn't have a valid kind byte
    static ValueKind getValueKind(const char* data, size_t data_len) {
        if (data_len < KIND_BYTE_SIZE) {
            return LEGACY_BASE;
        }
        uint8_t kind = *reinterpret_cast<const uint8_t*>(data);
        if (kind == COL_BASE || kind == COL_DELTA) {
            return static_cast<ValueKind>(kind);
        }
        return LEGACY_BASE;
    }
    
    // Check if a value is a column delta
    static bool isColumnDelta(const char* data, size_t data_len) {
        return getValueKind(data, data_len) == COL_DELTA;
    }
    
    // Check if a value is a column base (full row with kind prefix)
    static bool isColumnBase(const char* data, size_t data_len) {
        return getValueKind(data, data_len) == COL_BASE;
    }
    
    // Get the user data portion of a value (after kind byte if present)
    // For LEGACY_BASE: returns the entire data
    // For COL_BASE: returns data after the kind byte
    // For COL_DELTA: returns the delta payload (after kind byte)
    static const char* getUserData(const char* data, size_t data_len, size_t& user_len) {
        ValueKind kind = getValueKind(data, data_len);
        if (kind == LEGACY_BASE) {
            user_len = data_len;
            return data;
        } else {
            user_len = data_len - KIND_BYTE_SIZE;
            return data + KIND_BYTE_SIZE;
        }
    }
    
    // Prepend kind byte to a value buffer
    static std::string prependKindByte(ValueKind kind, const std::string& value) {
        std::string result;
        result.resize(KIND_BYTE_SIZE + value.size());
        result[0] = static_cast<char>(kind);
        std::memcpy(result.data() + KIND_BYTE_SIZE, value.data(), value.size());
        return result;
    }
};

// Count number of bits set in a bitmask (for counting changed fields)
inline int countChangedFields(uint32_t changed) {
    int count = 0;
    while (changed) {
        count += changed & 1;
        changed >>= 1;
    }
    return count;
}

} // namespace mako

#endif // _LIB_COLUMN_DELTA_H_
