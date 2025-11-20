#ifndef MAKO_KDV_FORMAT_H
#define MAKO_KDV_FORMAT_H

#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>
#include <list>
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
 * KDV Header Structure
 * 
 * Version 1 (20 bytes): Original format without key_hash
 * Version 2 (28 bytes): Adds key_hash for per-key delta compression
 * 
 * Stored at the beginning of each encoded log entry.
 * Provides metadata for decoding and chain management.
 */
struct KDVHeader {
    uint32_t magic;            // Magic number (0x4B445630 = "KDV0")
    uint8_t version;           // Format version (1 or 2)
    uint8_t mode;              // KDVEncodeMode (BASE or DELTA)
    uint16_t chain_len;        // Number of deltas since last base
    uint64_t base_seq;         // Sequence number of base (0 if this is base)
    uint32_t original_size;    // Original uncompressed size
    uint64_t key_hash;         // Hash of logical key (version 2 only)
    
    KDVHeader() 
        : magic(KDV_MAGIC), version(2), mode(0), chain_len(0), base_seq(0), original_size(0), key_hash(0) {}
} __attribute__((packed));

static_assert(sizeof(KDVHeader) == 28, "KDVHeader must be 28 bytes");

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
 * Per-Key State for KDV Encoding
 * 
 * Tracks the last base value and chain length for a specific key.
 */
struct KDVKeyState {
    std::string base_;
    uint64_t base_seq_;
    uint16_t chain_len_;
    uint64_t last_access_time_;  // For LRU eviction
    
    KDVKeyState() : base_seq_(0), chain_len_(0), last_access_time_(0) {}
};

/**
 * Per-Partition State for KDV Encoding with LRU Cache
 * 
 * Tracks base values per key with LRU eviction policy.
 * Thread-safe for concurrent access.
 */
class KDVPartitionState {
public:
    KDVPartitionState(size_t max_cache_size = 10000);
    
    void setBase(uint64_t key_hash, uint64_t seq, const std::string& value);
    bool hasBase(uint64_t key_hash) const;
    const std::string& getBase(uint64_t key_hash) const;
    uint64_t getBaseSeq(uint64_t key_hash) const;
    uint16_t getChainLen(uint64_t key_hash) const;
    void incrementChain(uint64_t key_hash);
    void resetChain(uint64_t key_hash, uint64_t seq);
    
    // Statistics
    size_t getCacheSize() const;
    size_t getEvictionCount() const;
    
private:
    void evictLRU();
    void updateAccessTime(uint64_t key_hash);
    
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, KDVKeyState> key_states_;
    size_t max_cache_size_;
    uint64_t access_counter_;
    size_t eviction_count_;
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
 * @param key_hash Hash of the logical key being updated
 * @param data Pointer to log data
 * @param size Size of log data in bytes
 * @return Encoded log (header + compressed data)
 * 
 * Encoding logic:
 * 1. Look up per-key state using key_hash
 * 2. Check if we should write a base (chain too long, delta too large, etc.)
 * 3. If base: write header with mode=BASE + original data
 * 4. If delta: compute delta from last base, write header with mode=DELTA + delta
 */
std::string kdv_encode_log(uint32_t shard_id, uint32_t partition_id, 
                           uint64_t seq_num, uint64_t key_hash,
                           const char* data, size_t size);

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
 * 1. Read KDVHeader (extracts key_hash from header for v2)
 * 2. If mode=BASE: update per-key state, return data after header
 * 3. If mode=DELTA: apply delta to base from per-key state, return reconstructed data
 * 
 * Note: key_hash is read from the header (v2) or falls back to partition-based state (v1)
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
 * (Version 2 – whole-value KDV policy)
 *
 * Check if we should write a base instead of delta
 * 
 * @param partition_state Current partition state
 * @param delta_size Size of computed delta
 * @param original_size Size of original value
 * @param seq_num Current sequence number
 * @param key_hash Logical key identifier within partition
 * @return true if should write base, false if should write delta
 * 
 * Conditions for writing base:
 * 1. No base exists yet
 * 2. Chain length >= max_chain_len
 * 3. Delta size > max_delta_size_ratio * original_size
 * 4. Sequence distance from base > max_base_age
 */
bool should_write_base(const KDVPartitionState& partition_state,
                      uint64_t key_hash, size_t delta_size,
                      size_t original_size, uint64_t seq_num,
                      struct WriteBaseReason* reason = nullptr);

/**
 * Encode a transaction log with per-record KDV compression (version 3).
 *
 * The input `data` is the original log blob produced by Transaction::serialize_util().
 * The returned string contains a KDVHeader (version=3) followed by a compact,
 * record-wise encoded payload. kdv_decode_log() understands version 3 and will
 * reconstruct the original log bytes.
 *
 * This API is intended for use by Paxos replication and RocksDB persistence,
 * and is not used by the existing unit tests that exercise the whole-value
 * KDV behavior.
 */
std::string kdv_encode_log_recordwise(uint32_t shard_id, uint32_t partition_id,
                                      uint64_t seq_num, const char* data, size_t size);

/**
 * Decode a version 3 per-record KDV-encoded log.
 *
 * Normally callers should use kdv_decode_log(), which will dispatch based
 * on the header version. This helper is exposed for completeness and tests.
 */
std::string kdv_decode_log_recordwise(uint32_t shard_id, uint32_t partition_id,
                                      uint64_t seq_num, const char* data, size_t size);

} // namespace kdv
} // namespace mako

#endif // MAKO_KDV_FORMAT_H
