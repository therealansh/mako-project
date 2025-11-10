#ifndef MAKO_DELTA_STORE_H
#define MAKO_DELTA_STORE_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <atomic>

namespace mako {

// Delta type enumeration
enum class DeltaType : uint8_t {
    FULL_VALUE = 0,      // Store complete new value
    SIMPLE_DIFF = 1,     // Store only changed bytes with offsets
    OPERATION_LOG = 2    // Store operation type + parameters (future)
};

// Delta record structure
struct DeltaRecord {
    DeltaType type;
    uint64_t version;           // Monotonically increasing version
    uint64_t base_version;      // Version this delta is based on
    uint32_t original_size;     // Size of full reconstructed value
    std::string payload;        // Delta data (format depends on type)
    uint64_t timestamp;         // Transaction timestamp
    uint32_t checksum;          // CRC32 of reconstructed value
    
    DeltaRecord() 
        : type(DeltaType::FULL_VALUE), version(0), base_version(0),
          original_size(0), timestamp(0), checksum(0) {}
    
    // Serialize to string for storage
    std::string serialize() const;
    
    // Deserialize from string
    static DeltaRecord deserialize(const std::string& data);
    
    // Get size of serialized delta
    size_t getSerializedSize() const {
        return sizeof(type) + sizeof(version) + sizeof(base_version) +
               sizeof(original_size) + sizeof(uint32_t) + payload.size() +
               sizeof(timestamp) + sizeof(checksum);
    }
};

// Delta metadata for tracking chain state
struct DeltaMetadata {
    uint64_t current_version;
    uint64_t base_version;
    uint32_t delta_count;
    uint64_t last_compaction_time;
    uint32_t total_delta_bytes;
    
    DeltaMetadata()
        : current_version(0), base_version(0), delta_count(0),
          last_compaction_time(0), total_delta_bytes(0) {}
    
    std::string serialize() const;
    static DeltaMetadata deserialize(const std::string& data);
};

// Delta chain for in-memory management
struct DeltaChain {
    std::string base_value;
    uint64_t base_version;
    std::vector<DeltaRecord> deltas;
    uint32_t chain_length;
    
    DeltaChain() : base_version(0), chain_length(0) {}
    
    // Reconstruct full value from base + deltas
    std::string reconstruct() const;
    
    // Add a new delta to the chain
    void addDelta(const DeltaRecord& delta);
    
    // Get total bytes in delta chain
    size_t getTotalBytes() const {
        size_t total = base_value.size();
        for (const auto& delta : deltas) {
            total += delta.getSerializedSize();
        }
        return total;
    }
    
    // Compact: collapse deltas into base
    void compact();
};

// Delta computation and application functions
class DeltaComputer {
public:
    // Compute delta between old and new values
    static DeltaRecord computeDelta(const std::string& old_value,
                                   const std::string& new_value,
                                   uint64_t version,
                                   uint64_t base_version,
                                   uint64_t timestamp);
    
    // Apply delta to base value
    static std::string applyDelta(const std::string& base,
                                 const DeltaRecord& delta);
    
    // Select optimal delta type based on value characteristics
    static DeltaType selectDeltaType(const std::string& old_value,
                                    const std::string& new_value);
    
    // Compute CRC32 checksum
    static uint32_t computeChecksum(const std::string& data);
    
private:
    // Compute simple diff (offset-based)
    static std::string computeSimpleDiff(const std::string& old_value,
                                        const std::string& new_value);
    
    // Apply simple diff
    static std::string applySimpleDiff(const std::string& base,
                                      const std::string& diff_data,
                                      uint32_t original_size);
    
    // Estimate diff size without computing full diff
    static size_t estimateDiffSize(const std::string& old_value,
                                   const std::string& new_value);
};

// Delta statistics for monitoring
struct DeltaStats {
    std::atomic<uint64_t> bytes_sent_full{0};
    std::atomic<uint64_t> bytes_sent_delta{0};
    std::atomic<uint64_t> deltas_created{0};
    std::atomic<uint64_t> deltas_applied{0};
    std::atomic<uint64_t> compactions_performed{0};
    std::atomic<uint64_t> max_chain_length{0};
    std::atomic<uint64_t> read_latency_us_sum{0};
    std::atomic<uint64_t> read_count{0};
    std::atomic<uint64_t> write_latency_us_sum{0};
    std::atomic<uint64_t> write_count{0};
    
    double getBandwidthReduction() const {
        uint64_t total_full = bytes_sent_full.load();
        uint64_t total_delta = bytes_sent_delta.load();
        if (total_full == 0) return 0.0;
        return 100.0 * (1.0 - (double)total_delta / total_full);
    }
    
    void print() const;
    void reset();
};

// Global delta statistics instance
extern DeltaStats g_delta_stats;

// Configuration for delta replication
struct DeltaConfig {
    bool enabled;
    uint32_t size_threshold;           // Min size to consider delta (bytes)
    uint32_t max_chain_length;         // Max deltas before compaction
    uint32_t max_chain_age_ms;         // Max age before compaction (ms)
    uint32_t max_chain_bytes;          // Max bytes before compaction
    uint32_t compaction_interval_ms;   // Compaction check interval (ms)
    
    DeltaConfig()
        : enabled(false), size_threshold(256), max_chain_length(5),
          max_chain_age_ms(60000), max_chain_bytes(4096),
          compaction_interval_ms(1000) {}
    
    // Load from environment variables
    static DeltaConfig loadFromEnv();
};

// Global delta configuration
extern DeltaConfig g_delta_config;

// Helper functions
inline bool isDeltaEnabled() {
    return g_delta_config.enabled;
}

inline void enableDelta(bool enable) {
    g_delta_config.enabled = enable;
}

// Helper function to compute delta or return full value
// Returns the data to transmit (either delta or full value) and updates stats
inline std::string computeDeltaOrFull(const std::string& old_value,
                                      const std::string& new_value,
                                      uint64_t version = 0,
                                      uint64_t base_version = 0,
                                      uint64_t timestamp = 0) {
    if (!isDeltaEnabled()) {
        g_delta_stats.bytes_sent_full.fetch_add(new_value.size());
        return new_value;
    }
    
    // Compute delta
    DeltaRecord delta = DeltaComputer::computeDelta(old_value, new_value, 
                                                     version, base_version, timestamp);
    
    // Return serialized delta
    return delta.serialize();
}

} // namespace mako

#endif // MAKO_DELTA_STORE_H
