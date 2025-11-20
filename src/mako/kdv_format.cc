#include "kdv_format.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <sstream>
#include <iostream>
#include <chrono>
#include <functional>

namespace mako {
namespace kdv {

inline uint64_t compute_payload_hash(const char* data, size_t size) {
    // Simple FNV-1a hash for fast fingerprinting.
    // Skip the first 8 bytes which contain volatile timestamps:
    // - latest_commit_timestamp (4 bytes)
    // - st_time (4 bytes)
    // This allows us to hash the stable transaction payload for better key identification.
    const size_t skip_bytes = 8;
    const char* hash_start = (size > skip_bytes) ? (data + skip_bytes) : data;
    const size_t hash_size = (size > skip_bytes) ? (size - skip_bytes) : size;

    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < hash_size; ++i) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(hash_start[i]));
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Hash for individual logical records within a log (table_id + key bytes).
inline uint64_t compute_record_hash(uint16_t table_id, const char* key_data, size_t key_len) {
    uint64_t hash = 14695981039346656037ULL;
    const unsigned char* buf = reinterpret_cast<const unsigned char*>(&table_id);
    for (size_t i = 0; i < sizeof(table_id); ++i) {
        hash ^= static_cast<uint64_t>(buf[i]);
        hash *= 1099511628211ULL;
    }
    const unsigned char* key_buf = reinterpret_cast<const unsigned char*>(key_data);
    for (size_t i = 0; i < key_len; ++i) {
        hash ^= static_cast<uint64_t>(key_buf[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// KDVPartitionState implementation with per-key LRU cache

KDVPartitionState::KDVPartitionState(size_t max_cache_size)
    : max_cache_size_(max_cache_size), access_counter_(0), eviction_count_(0) {
}

void KDVPartitionState::setBase(uint64_t key_hash, uint64_t seq, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if we need to evict
    if (key_states_.size() >= max_cache_size_ && key_states_.find(key_hash) == key_states_.end()) {
        evictLRU();
    }
    
    KDVKeyState& state = key_states_[key_hash];
    state.base_ = value;
    state.base_seq_ = seq;
    state.chain_len_ = 0;
    state.last_access_time_ = ++access_counter_;
}

bool KDVPartitionState::hasBase(uint64_t key_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = key_states_.find(key_hash);
    return it != key_states_.end() && !it->second.base_.empty();
}

const std::string& KDVPartitionState::getBase(uint64_t key_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = key_states_.find(key_hash);
    if (it == key_states_.end()) {
        static const std::string empty;
        return empty;
    }
    const_cast<KDVPartitionState*>(this)->updateAccessTime(key_hash);
    return it->second.base_;
}

uint64_t KDVPartitionState::getBaseSeq(uint64_t key_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = key_states_.find(key_hash);
    if (it == key_states_.end()) {
        return 0;
    }
    const_cast<KDVPartitionState*>(this)->updateAccessTime(key_hash);
    return it->second.base_seq_;
}

uint16_t KDVPartitionState::getChainLen(uint64_t key_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = key_states_.find(key_hash);
    if (it == key_states_.end()) {
        return 0;
    }
    const_cast<KDVPartitionState*>(this)->updateAccessTime(key_hash);
    return it->second.chain_len_;
}

void KDVPartitionState::incrementChain(uint64_t key_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = key_states_.find(key_hash);
    if (it != key_states_.end()) {
        it->second.chain_len_++;
        updateAccessTime(key_hash);
    }
}

void KDVPartitionState::resetChain(uint64_t key_hash, uint64_t seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = key_states_.find(key_hash);
    if (it != key_states_.end()) {
        it->second.base_seq_ = seq;
        it->second.chain_len_ = 0;
        updateAccessTime(key_hash);
    }
}

size_t KDVPartitionState::getCacheSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return key_states_.size();
}

size_t KDVPartitionState::getEvictionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return eviction_count_;
}

void KDVPartitionState::evictLRU() {
    // Find the entry with the smallest last_access_time_
    uint64_t min_access_time = UINT64_MAX;
    uint64_t lru_key = 0;
    
    for (const auto& pair : key_states_) {
        if (pair.second.last_access_time_ < min_access_time) {
            min_access_time = pair.second.last_access_time_;
            lru_key = pair.first;
        }
    }
    
    if (min_access_time != UINT64_MAX) {
        key_states_.erase(lru_key);
        eviction_count_++;
    }
}

void KDVPartitionState::updateAccessTime(uint64_t key_hash) {
    auto it = key_states_.find(key_hash);
    if (it != key_states_.end()) {
        it->second.last_access_time_ = ++access_counter_;
    }
}

// KDVStoreState implementation

KDVStoreState::KDVStoreState() 
    : max_chain_len_(64),
      max_delta_size_ratio_(0.9),
      max_base_age_(10000) {
}

KDVStoreState& KDVStoreState::getInstance() {
    // Use a leaky singleton to avoid shutdown-order issues
    // The singleton is intentionally never freed to prevent destructor
    // from running after other subsystems (threads, logging, etc.) are torn down
    static KDVStoreState* instance = new KDVStoreState();
    return *instance;
}

KDVPartitionState& KDVStoreState::getPartitionState(uint32_t partition_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = partition_states_.find(partition_id);
    if (it == partition_states_.end()) {
        partition_states_[partition_id] = std::make_unique<KDVPartitionState>();
        return *partition_states_[partition_id];
    }
    return *it->second;
}

void KDVStoreState::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    partition_states_.clear();
}

// Delta computation and application

std::string compute_delta(const std::string& base, const std::string& value) {
    size_t base_len = base.size();
    size_t value_len = value.size();
    
    // Find common prefix length
    size_t prefix_len = 0;
    size_t min_len = std::min(base_len, value_len);
    while (prefix_len < min_len && base[prefix_len] == value[prefix_len]) {
        prefix_len++;
    }
    
    // Find common suffix length (but don't overlap with prefix)
    size_t suffix_len = 0;
    while (suffix_len < base_len - prefix_len && 
           suffix_len < value_len - prefix_len &&
           base[base_len - 1 - suffix_len] == value[value_len - 1 - suffix_len]) {
        suffix_len++;
    }
    
    // Extract middle region from value
    size_t middle_start = prefix_len;
    size_t middle_end = value_len - suffix_len;
    size_t middle_len = (middle_end > middle_start) ? (middle_end - middle_start) : 0;
    
    // Build delta: DeltaBlock header + middle data
    std::string delta;
    delta.resize(sizeof(DeltaBlock) + middle_len);
    
    DeltaBlock* block = reinterpret_cast<DeltaBlock*>(&delta[0]);
    block->prefix_len = static_cast<uint32_t>(prefix_len);
    block->suffix_len = static_cast<uint32_t>(suffix_len);
    block->middle_len = static_cast<uint32_t>(middle_len);
    
    if (middle_len > 0) {
        std::memcpy(&delta[sizeof(DeltaBlock)], 
                   &value[middle_start], 
                   middle_len);
    }
    
    return delta;
}

std::string apply_delta(const std::string& base, const std::string& delta) {
    if (delta.size() < sizeof(DeltaBlock)) {
        std::cerr << "[KDV] Error: Delta too small, size=" << delta.size() << std::endl;
        return "";
    }
    
    const DeltaBlock* block = reinterpret_cast<const DeltaBlock*>(delta.data());
    uint32_t prefix_len = block->prefix_len;
    uint32_t suffix_len = block->suffix_len;
    uint32_t middle_len = block->middle_len;
    
    // Validate delta structure
    if (delta.size() != sizeof(DeltaBlock) + middle_len) {
        std::cerr << "[KDV] Error: Delta size mismatch, expected=" 
                  << (sizeof(DeltaBlock) + middle_len) 
                  << ", actual=" << delta.size() << std::endl;
        return "";
    }
    
    // Validate base is large enough
    if (base.size() < prefix_len + suffix_len) {
        std::cerr << "[KDV] Error: Base too small for delta, base_size=" << base.size()
                  << ", prefix=" << prefix_len << ", suffix=" << suffix_len << std::endl;
        return "";
    }
    
    // Reconstruct: prefix + middle + suffix
    std::string result;
    result.reserve(prefix_len + middle_len + suffix_len);
    
    // Append prefix from base
    if (prefix_len > 0) {
        result.append(base.data(), prefix_len);
    }
    
    // Append middle from delta
    if (middle_len > 0) {
        result.append(delta.data() + sizeof(DeltaBlock), middle_len);
    }
    
    // Append suffix from base
    if (suffix_len > 0) {
        result.append(base.data() + base.size() - suffix_len, suffix_len);
    }
    
    return result;
}

// Policy decision with detailed instrumentation

struct WriteBaseReason {
    bool no_base = false;
    bool chain_too_long = false;
    bool delta_too_large = false;
    bool base_too_old = false;
};

static std::atomic<uint64_t> g_no_base_count{0};
static std::atomic<uint64_t> g_chain_too_long_count{0};
static std::atomic<uint64_t> g_delta_too_large_count{0};
static std::atomic<uint64_t> g_base_too_old_count{0};

bool should_write_base(const KDVPartitionState& partition_state,
                      uint64_t key_hash, size_t delta_size, size_t original_size, 
                      uint64_t seq_num, WriteBaseReason* reason) {
    auto& store = KDVStoreState::getInstance();
    
    // No base exists yet
    if (!partition_state.hasBase(key_hash)) {
        if (reason) reason->no_base = true;
        g_no_base_count++;
        return true;
    }
    
    // Chain too long
    if (partition_state.getChainLen(key_hash) >= store.getMaxChainLen()) {
        if (reason) reason->chain_too_long = true;
        g_chain_too_long_count++;
        return true;
    }
    
    // Delta not efficient (larger than threshold)
    if (original_size > 0 && 
        delta_size > static_cast<size_t>(store.getMaxDeltaSizeRatio() * original_size)) {
        if (reason) reason->delta_too_large = true;
        g_delta_too_large_count++;
        return true;
    }
    
    // Base too old
    if (seq_num > partition_state.getBaseSeq(key_hash) && 
        seq_num - partition_state.getBaseSeq(key_hash) > store.getMaxBaseAge()) {
        if (reason) reason->base_too_old = true;
        g_base_too_old_count++;
        return true;
    }
    
    return false;  // Write delta
}

void print_write_base_stats() {
    std::cout << "[KDV Policy Stats] no_base=" << g_no_base_count 
              << ", chain_too_long=" << g_chain_too_long_count
              << ", delta_too_large=" << g_delta_too_large_count
              << ", base_too_old=" << g_base_too_old_count
              << std::endl;
}

// Main encode/decode functions

std::string kdv_encode_log(uint32_t shard_id, uint32_t partition_id, 
                           uint64_t seq_num, uint64_t key_hash,
                           const char* data, size_t size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // If key_hash is 0, compute it from the payload
    if (key_hash == 0) {
        key_hash = compute_payload_hash(data, size);
    }
    
    auto& store = KDVStoreState::getInstance();
    auto& partition_state = store.getPartitionState(partition_id);
    
    std::string value(data, size);
    std::string result;
    
    KDVHeader header;
    header.version = 2;
    header.original_size = static_cast<uint32_t>(size);
    header.key_hash = key_hash;
    
    bool write_base = false;
    std::string delta;
    
    if (partition_state.hasBase(key_hash)) {
        // Compute delta against the base for this specific key
        delta = compute_delta(partition_state.getBase(key_hash), value);
        
        // Decide whether to write base or delta
        write_base = should_write_base(partition_state, key_hash, delta.size(), size, seq_num);
    } else {
        // First log for this key, must write base
        write_base = true;
    }
    
    if (write_base) {
        // Write as BASE
        header.mode = static_cast<uint8_t>(KDVEncodeMode::BASE);
        header.chain_len = 0;
        header.base_seq = seq_num;
        
        // Update partition state for this key
        partition_state.setBase(key_hash, seq_num, value);
        
        // Build result: header + original data
        result.resize(sizeof(KDVHeader) + size);
        std::memcpy(&result[0], &header, sizeof(KDVHeader));
        std::memcpy(&result[sizeof(KDVHeader)], data, size);
        
    } else {
        // Write as DELTA
        header.mode = static_cast<uint8_t>(KDVEncodeMode::DELTA);
        header.chain_len = partition_state.getChainLen(key_hash) + 1;
        header.base_seq = partition_state.getBaseSeq(key_hash);
        
        // Increment chain length for this key
        partition_state.incrementChain(key_hash);
        
        // Build result: header + delta
        result.resize(sizeof(KDVHeader) + delta.size());
        std::memcpy(&result[0], &header, sizeof(KDVHeader));
        std::memcpy(&result[sizeof(KDVHeader)], delta.data(), delta.size());
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    
    // Optional: Log encoding stats (can be disabled in production)
    static std::atomic<uint64_t> encode_count{0};
    static std::atomic<uint64_t> base_count{0};
    static std::atomic<uint64_t> delta_count{0};
    static std::atomic<uint64_t> total_original_bytes{0};
    static std::atomic<uint64_t> total_encoded_bytes{0};
    
    encode_count++;
    total_original_bytes += size;
    total_encoded_bytes += result.size();
    
    if (write_base) {
        base_count++;
    } else {
        delta_count++;
    }
    
    if (encode_count % 1000 == 0) {
        double compression_ratio = total_original_bytes > 0 ? 
            (double)total_encoded_bytes / total_original_bytes : 1.0;
        double bandwidth_reduction = total_original_bytes > 0 ?
            (1.0 - compression_ratio) * 100.0 : 0.0;
        
        std::cout << "[KDV Encode] count=" << encode_count 
                  << ", bases=" << base_count 
                  << ", deltas=" << delta_count
                  << ", compression_ratio=" << compression_ratio
                  << ", bandwidth_reduction=" << bandwidth_reduction << "%"
                  << ", cache_size=" << partition_state.getCacheSize()
                  << ", evictions=" << partition_state.getEvictionCount()
                  << ", time_us=" << (duration_ns / 1000.0)
                  << std::endl;
        print_write_base_stats();
    }
    
    return result;
}

// Helper structures for version 3 record-wise encoding.
struct ParsedRecord {
    std::string key;
    uint16_t table_id;
    std::string value;
};

struct ParsedSegment {
    uint32_t commit_ts;
    uint16_t kv_count;
    uint32_t len_of_kv;
    std::vector<ParsedRecord> records;
    uint32_t trailer_ts;
    uint32_t trailer_st_time;
};

static bool parse_transaction_stream(const char* data, size_t size,
                                     std::vector<ParsedSegment>& segments) {
    segments.clear();

    if (size == 0) {
        return false;
    }

    const char* ptr = data;
    const char* end = data + size;

    while (ptr < end) {
        if (end - ptr < static_cast<ptrdiff_t>(sizeof(uint32_t) + sizeof(uint16_t) +
                                               sizeof(uint32_t) + 2 * sizeof(uint32_t))) {
            return false;
        }

        ParsedSegment seg;
        std::memcpy(&seg.commit_ts, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        std::memcpy(&seg.kv_count, ptr, sizeof(uint16_t));
        ptr += sizeof(uint16_t);

        std::memcpy(&seg.len_of_kv, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        const char* kv_start = ptr;
        const char* kv_end = kv_start + seg.len_of_kv;
        if (kv_end + 2 * sizeof(uint32_t) > end) {
            return false;
        }

        seg.records.clear();
        seg.records.reserve(seg.kv_count);

        uint16_t seen = 0;
        while (ptr < kv_end && seen < seg.kv_count) {
            if (ptr + sizeof(uint16_t) > kv_end) {
                return false;
            }
            uint16_t key_len = 0;
            std::memcpy(&key_len, ptr, sizeof(uint16_t));
            ptr += sizeof(uint16_t);

            if (ptr + key_len > kv_end) {
                return false;
            }
            std::string key(ptr, key_len);
            ptr += key_len;

            if (ptr + sizeof(uint16_t) > kv_end) {
                return false;
            }
            uint16_t val_len = 0;
            std::memcpy(&val_len, ptr, sizeof(uint16_t));
            ptr += sizeof(uint16_t);

            if (ptr + val_len > kv_end) {
                return false;
            }
            std::string value(ptr, val_len);
            ptr += val_len;

            if (ptr + sizeof(uint16_t) > kv_end) {
                return false;
            }
            uint16_t table_id = 0;
            std::memcpy(&table_id, ptr, sizeof(uint16_t));
            ptr += sizeof(uint16_t);

            ParsedRecord rec;
            rec.key = std::move(key);
            rec.table_id = table_id;
            rec.value = std::move(value);
            seg.records.emplace_back(std::move(rec));

            ++seen;
        }

        if (seen != seg.kv_count || ptr != kv_end) {
            return false;
        }

        std::memcpy(&seg.trailer_ts, kv_end, sizeof(uint32_t));
        std::memcpy(&seg.trailer_st_time, kv_end + sizeof(uint32_t), sizeof(uint32_t));

        segments.emplace_back(std::move(seg));

        ptr = kv_end + 2 * sizeof(uint32_t);
    }

    return ptr == end;
}

std::string kdv_encode_log_recordwise(uint32_t shard_id, uint32_t partition_id,
                                      uint64_t seq_num, const char* data, size_t size) {
    std::vector<ParsedSegment> segments;
    if (!parse_transaction_stream(data, size, segments)) {
        return kdv_encode_log(shard_id, partition_id, seq_num, 0, data, size);
    }

    if (segments.empty()) {
        return kdv_encode_log(shard_id, partition_id, seq_num, 0, data, size);
    }

    auto& store = KDVStoreState::getInstance();
    auto& partition_state = store.getPartitionState(partition_id);

    std::string payload;
    payload.reserve(size);

    uint16_t segment_count = static_cast<uint16_t>(segments.size());
    payload.append(reinterpret_cast<const char*>(&segment_count), sizeof(uint16_t));

    bool any_delta = false;

    for (const auto& seg : segments) {
        payload.append(reinterpret_cast<const char*>(&seg.commit_ts), sizeof(uint32_t));
        uint16_t kv_count = static_cast<uint16_t>(seg.records.size());
        payload.append(reinterpret_cast<const char*>(&kv_count), sizeof(uint16_t));

        for (const auto& rec : seg.records) {
            uint16_t key_len = static_cast<uint16_t>(rec.key.size());
            uint16_t table_id = rec.table_id;

            payload.append(reinterpret_cast<const char*>(&key_len), sizeof(uint16_t));
            payload.append(rec.key.data(), rec.key.size());
            payload.append(reinterpret_cast<const char*>(&table_id), sizeof(uint16_t));

            uint64_t record_hash = compute_record_hash(table_id, rec.key.data(), rec.key.size());

            bool write_base = false;
            std::string delta;

            if (partition_state.hasBase(record_hash)) {
                delta = compute_delta(partition_state.getBase(record_hash), rec.value);
                WriteBaseReason reason;
                write_base = should_write_base(partition_state,
                                               record_hash,
                                               delta.size(),
                                               rec.value.size(),
                                               seq_num,
                                               &reason);
            } else {
                write_base = true;
            }

            uint8_t record_mode = write_base
                ? static_cast<uint8_t>(KDVEncodeMode::BASE)
                : static_cast<uint8_t>(KDVEncodeMode::DELTA);
            payload.push_back(static_cast<char>(record_mode));

            std::string encoded_value;
            if (write_base) {
                encoded_value = rec.value;
                partition_state.setBase(record_hash, seq_num, rec.value);
            } else {
                encoded_value = std::move(delta);
                partition_state.incrementChain(record_hash);
                any_delta = true;
            }

            uint32_t enc_len = static_cast<uint32_t>(encoded_value.size());
            payload.append(reinterpret_cast<const char*>(&enc_len), sizeof(uint32_t));
            if (!encoded_value.empty()) {
                payload.append(encoded_value.data(), encoded_value.size());
            }
        }

        payload.append(reinterpret_cast<const char*>(&seg.trailer_ts), sizeof(uint32_t));
        payload.append(reinterpret_cast<const char*>(&seg.trailer_st_time), sizeof(uint32_t));
    }

    // Build final buffer: KDVHeader (v3) + payload
    std::string result;
    result.resize(sizeof(KDVHeader));

    KDVHeader header;
    header.version = 3;
    header.mode = any_delta
        ? static_cast<uint8_t>(KDVEncodeMode::DELTA)
        : static_cast<uint8_t>(KDVEncodeMode::BASE);
    header.chain_len = 0;
    header.base_seq = 0;
    header.original_size = static_cast<uint32_t>(size);
    header.key_hash = static_cast<uint64_t>(partition_id);

    std::memcpy(&result[0], &header, sizeof(KDVHeader));
    result.append(payload);

    return result;
}

std::string kdv_decode_log_recordwise(uint32_t shard_id, uint32_t partition_id,
                                      uint64_t seq_num, const char* data, size_t size) {
    if (size < sizeof(KDVHeader) + sizeof(uint16_t) +
               sizeof(uint32_t) + sizeof(uint16_t) + 2 * sizeof(uint32_t)) {
        return "";
    }

    const KDVHeader* header = reinterpret_cast<const KDVHeader*>(data);
    const char* ptr = data + sizeof(KDVHeader);
    const char* end = data + size;

    uint16_t segment_count = 0;
    std::memcpy(&segment_count, ptr, sizeof(uint16_t));
    ptr += sizeof(uint16_t);

    std::vector<ParsedSegment> segments;
    segments.reserve(segment_count);

    auto& store = KDVStoreState::getInstance();
    auto& partition_state = store.getPartitionState(partition_id);

    for (uint16_t s = 0; s < segment_count; ++s) {
        if (ptr + sizeof(uint32_t) + sizeof(uint16_t) > end) {
            return "";
        }

        ParsedSegment seg;
        std::memcpy(&seg.commit_ts, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        std::memcpy(&seg.kv_count, ptr, sizeof(uint16_t));
        ptr += sizeof(uint16_t);

        seg.records.clear();
        seg.records.reserve(seg.kv_count);

        for (uint16_t i = 0; i < seg.kv_count; ++i) {
            if (ptr + sizeof(uint16_t) > end) {
                return "";
            }
            uint16_t key_len = 0;
            std::memcpy(&key_len, ptr, sizeof(uint16_t));
            ptr += sizeof(uint16_t);

            if (ptr + key_len > end) {
                return "";
            }
            std::string key(ptr, key_len);
            ptr += key_len;

            if (ptr + sizeof(uint16_t) > end) {
                return "";
            }
            uint16_t table_id = 0;
            std::memcpy(&table_id, ptr, sizeof(uint16_t));
            ptr += sizeof(uint16_t);

            if (ptr + sizeof(uint8_t) + sizeof(uint32_t) > end) {
                return "";
            }
            uint8_t record_mode = static_cast<uint8_t>(*ptr);
            ptr += sizeof(uint8_t);

            uint32_t enc_len = 0;
            std::memcpy(&enc_len, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);

            if (ptr + enc_len > end) {
                return "";
            }
            std::string enc_value(ptr, enc_len);
            ptr += enc_len;

            uint64_t record_hash = compute_record_hash(table_id, key.data(), key.size());

            std::string value;
            if (record_mode == static_cast<uint8_t>(KDVEncodeMode::BASE)) {
                value = enc_value;
                partition_state.setBase(record_hash, seq_num, value);
            } else if (record_mode == static_cast<uint8_t>(KDVEncodeMode::DELTA)) {
                if (!partition_state.hasBase(record_hash)) {
                    std::cerr << "[KDV] Error: No base for record when decoding delta (partition "
                              << partition_id << ", table_id=" << table_id << ")" << std::endl;
                    return "";
                }
                value = apply_delta(partition_state.getBase(record_hash), enc_value);
                if (value.empty()) {
                    std::cerr << "[KDV] Error: Failed to apply record-level delta (partition "
                              << partition_id << ", table_id=" << table_id << ")" << std::endl;
                    return "";
                }
            } else {
                std::cerr << "[KDV] Error: Unknown record mode=" << (int)record_mode << std::endl;
                return "";
            }

            ParsedRecord rec;
            rec.key = std::move(key);
            rec.table_id = table_id;
            rec.value = std::move(value);
            seg.records.emplace_back(std::move(rec));
        }

        if (ptr + 2 * sizeof(uint32_t) > end) {
            return "";
        }
        std::memcpy(&seg.trailer_ts, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        std::memcpy(&seg.trailer_st_time, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        segments.emplace_back(std::move(seg));
    }

    if (ptr != end) {
        std::cerr << "[KDV] Warning: Extra bytes at end of v3 payload" << std::endl;
    }

    // Reconstruct original transaction log (possibly multiple segments).
    std::string result;
    result.reserve(header->original_size);

    for (const auto& seg : segments) {
        result.append(reinterpret_cast<const char*>(&seg.commit_ts), sizeof(uint32_t));

        uint16_t kv_count = static_cast<uint16_t>(seg.records.size());
        result.append(reinterpret_cast<const char*>(&kv_count), sizeof(uint16_t));

        size_t len_of_kv_offset = result.size();
        uint32_t len_of_kv = 0;
        result.append(reinterpret_cast<const char*>(&len_of_kv), sizeof(uint32_t));

        size_t kv_region_start = result.size();
        for (const auto& rec : seg.records) {
            uint16_t key_len = static_cast<uint16_t>(rec.key.size());
            uint16_t val_len = static_cast<uint16_t>(rec.value.size());
            uint16_t table_id = rec.table_id;

            result.append(reinterpret_cast<const char*>(&key_len), sizeof(uint16_t));
            result.append(rec.key.data(), rec.key.size());

            result.append(reinterpret_cast<const char*>(&val_len), sizeof(uint16_t));
            if (!rec.value.empty()) {
                result.append(rec.value.data(), rec.value.size());
            }

            result.append(reinterpret_cast<const char*>(&table_id), sizeof(uint16_t));
        }
        size_t kv_region_end = result.size();
        len_of_kv = static_cast<uint32_t>(kv_region_end - kv_region_start);
        std::memcpy(&result[len_of_kv_offset], &len_of_kv, sizeof(uint32_t));

        result.append(reinterpret_cast<const char*>(&seg.trailer_ts), sizeof(uint32_t));
        result.append(reinterpret_cast<const char*>(&seg.trailer_st_time), sizeof(uint32_t));
    }

    if (result.size() != header->original_size) {
        std::cerr << "[KDV] Warning: v3 decoded size mismatch, expected="
                  << header->original_size << ", actual=" << result.size() << std::endl;
    }

    return result;
}

std::string kdv_decode_log(uint32_t shard_id, uint32_t partition_id,
                           uint64_t seq_num, const char* data, size_t size) {
    auto start_time = std::chrono::high_resolution_clock::now();

    if (size < sizeof(KDVHeader)) {
        return "";
    }

    const KDVHeader* header = reinterpret_cast<const KDVHeader*>(data);

    if (header->magic != KDV_MAGIC) {
        return "";
    }

    std::string result;

    if (header->version == 3) {
        result = kdv_decode_log_recordwise(shard_id, partition_id, seq_num, data, size);
    } else {
        if (header->version != 1 && header->version != 2) {
            std::cerr << "[KDV] Error: Unsupported KDV version=" << (int)header->version << std::endl;
            return "";
        }

        // Extract key_hash from header (v2) or use partition-based fallback (v1)
        uint64_t key_hash = 0;
        if (header->version == 2) {
            key_hash = header->key_hash;
        } else {
            // Version 1: use partition_id as key_hash for backward compatibility
            key_hash = partition_id;
        }

        auto& store = KDVStoreState::getInstance();
        auto& partition_state = store.getPartitionState(partition_id);

        const char* payload = data + sizeof(KDVHeader);
        size_t payload_size = size - sizeof(KDVHeader);

        if (header->mode == static_cast<uint8_t>(KDVEncodeMode::BASE)) {
            result.assign(payload, payload_size);
            partition_state.setBase(key_hash, seq_num, result);
        } else if (header->mode == static_cast<uint8_t>(KDVEncodeMode::DELTA)) {
            if (!partition_state.hasBase(key_hash)) {
                std::cerr << "[KDV] Error: No base found for key_hash " << key_hash
                          << " in partition " << partition_id
                          << " when decoding delta at seq " << seq_num << std::endl;
                return "";
            }

            std::string delta(payload, payload_size);
            result = apply_delta(partition_state.getBase(key_hash), delta);

            if (result.empty()) {
                std::cerr << "[KDV] Error: Failed to apply delta for key_hash " << key_hash
                          << " in partition " << partition_id
                          << " at seq " << seq_num << std::endl;
                return "";
            }
        } else {
            std::cerr << "[KDV] Error: Unknown KDV mode=" << (int)header->mode << std::endl;
            return "";
        }

        if (result.size() != header->original_size) {
            std::cerr << "[KDV] Warning: Decoded size mismatch, expected=" << header->original_size
                      << ", actual=" << result.size() << std::endl;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();

    static std::atomic<uint64_t> decode_count{0};
    static std::atomic<uint64_t> base_decode_count{0};
    static std::atomic<uint64_t> delta_decode_count{0};

    decode_count++;
    if (header->mode == static_cast<uint8_t>(KDVEncodeMode::BASE)) {
        base_decode_count++;
    } else {
        delta_decode_count++;
    }

    if (decode_count % 1000 == 0) {
        std::cout << "[KDV Decode] count=" << decode_count
                  << ", bases=" << base_decode_count
                  << ", deltas=" << delta_decode_count
                  << ", time_us=" << (duration_ns / 1000.0)
                  << std::endl;
    }

    return result;
}

} // namespace kdv
} // namespace mako
