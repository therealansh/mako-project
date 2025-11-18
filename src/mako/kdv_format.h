#ifndef MAKO_KDV_FORMAT_H
#define MAKO_KDV_FORMAT_H

#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>

namespace mako {
namespace kdv {

/**
 * KDV Encoding Mode
 * 
 * BASE: Full value stored (no delta compression)
 * DELTA: Delta-encoded relative to previous base
 */
enum class KDVEncodeMode : uint8_t {
    BASE = 0,   // Full value (no compression)
    DELTA = 1   // Delta-encoded value
};

/**
 * KDV Magic Number
 * 
 * Used to identify KDV-encoded data and distinguish it from raw data.
 * Value: 0x4B445630 = "KDV0" in ASCII
 */
constexpr uint32_t KDV_MAGIC = 0x4B445630;

/**
 * KDV Header Structure (20 bytes)
 * 
 * Stored at the beginning of each encoded log entry.
 * Provides metadata for decoding and chain management.
 */
struct KDVHeader {
    uint32_t magic;            // Magic number (0x4B445630 = "KDV0")
    uint8_t version;           // Format version (currently 1)
    uint8_t mode;              // KDVEncodeMode (BASE or DELTA)
    uint16_t chain_len;        // Number of deltas since last base
    uint64_t base_seq;         // Sequence number of base (0 if this is base)
    uint32_t original_size;    // Original uncompressed size
    
    KDVHeader() 
        : magic(KDV_MAGIC), version(1), mode(0), chain_len(0), base_seq(0), original_size(0) {}
} __attribute__((packed));

static_assert(sizeof(KDVHeader) == 20, "KDVHeader must be 20 bytes");

/**
 * Delta Representation
 * 
 * Simple blockwise delta format:
 * - Common prefix length (uint32_t)
 * - Common suffix length (uint32_t)
 * - Middle region length (uint32_t)
 * - Middle region data (variable)
 * 
 * Reconstruction: prefix + middle + suffix
 */
struct DeltaBlock {
    uint32_t prefix_len;
    uint32_t suffix_len;
    uint32_t middle_len;
    // Followed by middle_len bytes of data
} __attribute__((packed));

/**
 * Per-Partition State for KDV Encoding
 * 
 * Tracks the last base value and chain length for delta encoding.
 * Thread-safe for concurrent access.
 */
class KDVPartitionState {
public:
    KDVPartitionState() : base_seq_(0), chain_len_(0) {}
    
    void setBase(uint64_t seq, const std::string& value);
    bool hasBase() const;
    const std::string& getBase() const;
    uint64_t getBaseSeq() const;
    uint16_t getChainLen() const;
    void incrementChain();
    void resetChain(uint64_t seq);
    
private:
    mutable std::mutex mutex_;
    std::string base_;
    uint64_t base_seq_;
    uint16_t chain_len_;
};

/**
 * Global KDV State Manager
 * 
 * Singleton that manages per-partition encoding state.
 * Maintains base values and chain lengths for all partitions.
 */
class KDVStoreState {
public:
    static KDVStoreState& getInstance();
    
    KDVPartitionState& getPartitionState(uint32_t partition_id);
    void reset();
    
    // Configuration
    void setMaxChainLen(uint16_t len) { max_chain_len_ = len; }
    void setMaxDeltaSizeRatio(double ratio) { max_delta_size_ratio_ = ratio; }
    void setMaxBaseAge(uint64_t age) { max_base_age_ = age; }
    
    uint16_t getMaxChainLen() const { return max_chain_len_; }
    double getMaxDeltaSizeRatio() const { return max_delta_size_ratio_; }
    uint64_t getMaxBaseAge() const { return max_base_age_; }
    
private:
    KDVStoreState();
    ~KDVStoreState() = default;
    KDVStoreState(const KDVStoreState&) = delete;
    KDVStoreState& operator=(const KDVStoreState&) = delete;
    
    std::mutex mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<KDVPartitionState>> partition_states_;
    
    // Policy parameters
    uint16_t max_chain_len_;        // Default: 16
    double max_delta_size_ratio_;   // Default: 0.7
    uint64_t max_base_age_;         // Default: 1000 (sequences)
};

/**
 * Encode a log entry with KDV compression
 * 
 * @param shard_id Shard identifier
 * @param partition_id Partition identifier
 * @param seq_num Sequence number for this log
 * @param data Pointer to log data
 * @param size Size of log data in bytes
 * @return Encoded log (header + compressed data)
 * 
 * Encoding logic:
 * 1. Check if we should write a base (chain too long, delta too large, etc.)
 * 2. If base: write header with mode=BASE + original data
 * 3. If delta: compute delta from last base, write header with mode=DELTA + delta
 */
std::string kdv_encode_log(uint32_t shard_id, uint32_t partition_id, 
                           uint64_t seq_num, const char* data, size_t size);

/**
 * Decode a KDV-encoded log entry
 * 
 * @param shard_id Shard identifier
 * @param partition_id Partition identifier
 * @param seq_num Sequence number for this log
 * @param data Pointer to encoded log data
 * @param size Size of encoded log data in bytes
 * @return Decoded original log data
 * 
 * Decoding logic:
 * 1. Read KDVHeader
 * 2. If mode=BASE: update partition state, return data after header
 * 3. If mode=DELTA: apply delta to base from partition state, return reconstructed data
 */
std::string kdv_decode_log(uint32_t shard_id, uint32_t partition_id,
                           uint64_t seq_num, const char* data, size_t size);

/**
 * Compute delta between two byte strings
 * 
 * @param base Base string
 * @param value New string
 * @return Delta representation (DeltaBlock + middle data)
 * 
 * Algorithm:
 * 1. Find common prefix length
 * 2. Find common suffix length
 * 3. Extract middle region that differs
 * 4. Return DeltaBlock header + middle data
 */
std::string compute_delta(const std::string& base, const std::string& value);

/**
 * Apply delta to base to reconstruct value
 * 
 * @param base Base string
 * @param delta Delta representation
 * @return Reconstructed value
 * 
 * Algorithm:
 * 1. Parse DeltaBlock header
 * 2. Extract prefix from base
 * 3. Extract middle from delta
 * 4. Extract suffix from base
 * 5. Concatenate: prefix + middle + suffix
 */
std::string apply_delta(const std::string& base, const std::string& delta);

/**
 * Check if we should write a base instead of delta
 * 
 * @param partition_state Current partition state
 * @param delta_size Size of computed delta
 * @param original_size Size of original value
 * @param seq_num Current sequence number
 * @return true if should write base, false if should write delta
 * 
 * Conditions for writing base:
 * 1. No base exists yet
 * 2. Chain length >= max_chain_len
 * 3. Delta size > max_delta_size_ratio * original_size
 * 4. Sequence distance from base > max_base_age
 */
bool should_write_base(const KDVPartitionState& partition_state,
                      size_t delta_size, size_t original_size, uint64_t seq_num);

} // namespace kdv
} // namespace mako

#endif // MAKO_KDV_FORMAT_H
