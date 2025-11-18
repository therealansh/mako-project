#include "kdv_format.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <sstream>
#include <iostream>
#include <chrono>

namespace mako {
namespace kdv {

// KDVPartitionState implementation

void KDVPartitionState::setBase(uint64_t seq, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    base_ = value;
    base_seq_ = seq;
    chain_len_ = 0;
}

bool KDVPartitionState::hasBase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !base_.empty();
}

const std::string& KDVPartitionState::getBase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return base_;
}

uint64_t KDVPartitionState::getBaseSeq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return base_seq_;
}

uint16_t KDVPartitionState::getChainLen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chain_len_;
}

void KDVPartitionState::incrementChain() {
    std::lock_guard<std::mutex> lock(mutex_);
    chain_len_++;
}

void KDVPartitionState::resetChain(uint64_t seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    base_seq_ = seq;
    chain_len_ = 0;
}

// KDVStoreState implementation

KDVStoreState::KDVStoreState() 
    : max_chain_len_(16),
      max_delta_size_ratio_(0.7),
      max_base_age_(1000) {
}

KDVStoreState& KDVStoreState::getInstance() {
    static KDVStoreState instance;
    return instance;
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

// Policy decision

bool should_write_base(const KDVPartitionState& partition_state,
                      size_t delta_size, size_t original_size, uint64_t seq_num) {
    auto& store = KDVStoreState::getInstance();
    
    // No base exists yet
    if (!partition_state.hasBase()) {
        return true;
    }
    
    // Chain too long
    if (partition_state.getChainLen() >= store.getMaxChainLen()) {
        return true;
    }
    
    // Delta not efficient (larger than threshold)
    if (original_size > 0 && 
        delta_size > static_cast<size_t>(store.getMaxDeltaSizeRatio() * original_size)) {
        return true;
    }
    
    // Base too old
    if (seq_num > partition_state.getBaseSeq() && 
        seq_num - partition_state.getBaseSeq() > store.getMaxBaseAge()) {
        return true;
    }
    
    return false;  // Write delta
}

// Main encode/decode functions

std::string kdv_encode_log(uint32_t shard_id, uint32_t partition_id, 
                           uint64_t seq_num, const char* data, size_t size) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    auto& store = KDVStoreState::getInstance();
    auto& partition_state = store.getPartitionState(partition_id);
    
    std::string value(data, size);
    std::string result;
    
    KDVHeader header;
    header.version = 1;
    header.original_size = static_cast<uint32_t>(size);
    
    bool write_base = false;
    std::string delta;
    
    if (partition_state.hasBase()) {
        // Compute delta
        delta = compute_delta(partition_state.getBase(), value);
        
        // Decide whether to write base or delta
        write_base = should_write_base(partition_state, delta.size(), size, seq_num);
    } else {
        // First log for this partition, must write base
        write_base = true;
    }
    
    if (write_base) {
        // Write as BASE
        header.mode = static_cast<uint8_t>(KDVEncodeMode::BASE);
        header.chain_len = 0;
        header.base_seq = seq_num;
        
        // Update partition state
        partition_state.setBase(seq_num, value);
        
        // Build result: header + original data
        result.resize(sizeof(KDVHeader) + size);
        std::memcpy(&result[0], &header, sizeof(KDVHeader));
        std::memcpy(&result[sizeof(KDVHeader)], data, size);
        
    } else {
        // Write as DELTA
        header.mode = static_cast<uint8_t>(KDVEncodeMode::DELTA);
        header.chain_len = partition_state.getChainLen() + 1;
        header.base_seq = partition_state.getBaseSeq();
        
        // Increment chain length
        partition_state.incrementChain();
        
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
    
    encode_count++;
    if (write_base) {
        base_count++;
    } else {
        delta_count++;
    }
    
    if (encode_count % 1000 == 0) {
        double compression_ratio = size > 0 ? (double)result.size() / size : 1.0;
        std::cout << "[KDV Encode] count=" << encode_count 
                  << ", bases=" << base_count 
                  << ", deltas=" << delta_count
                  << ", compression=" << compression_ratio
                  << ", time_us=" << (duration_ns / 1000.0)
                  << std::endl;
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
    
    if (header->version != 1) {
        std::cerr << "[KDV] Error: Unsupported KDV version=" << (int)header->version << std::endl;
        return "";
    }
    
    auto& store = KDVStoreState::getInstance();
    auto& partition_state = store.getPartitionState(partition_id);
    
    std::string result;
    const char* payload = data + sizeof(KDVHeader);
    size_t payload_size = size - sizeof(KDVHeader);
    
    if (header->mode == static_cast<uint8_t>(KDVEncodeMode::BASE)) {
        // BASE mode: payload is original data
        result.assign(payload, payload_size);
        
        // Update partition state with new base
        partition_state.setBase(seq_num, result);
        
    } else if (header->mode == static_cast<uint8_t>(KDVEncodeMode::DELTA)) {
        // DELTA mode: apply delta to base
        if (!partition_state.hasBase()) {
            std::cerr << "[KDV] Error: No base found for partition " << partition_id 
                      << " when decoding delta at seq " << seq_num << std::endl;
            return "";
        }
        
        std::string delta(payload, payload_size);
        result = apply_delta(partition_state.getBase(), delta);
        
        if (result.empty()) {
            std::cerr << "[KDV] Error: Failed to apply delta for partition " << partition_id
                      << " at seq " << seq_num << std::endl;
            return "";
        }
        
    } else {
        std::cerr << "[KDV] Error: Unknown KDV mode=" << (int)header->mode << std::endl;
        return "";
    }
    
    // Validate reconstructed size matches header
    if (result.size() != header->original_size) {
        std::cerr << "[KDV] Warning: Decoded size mismatch, expected=" << header->original_size
                  << ", actual=" << result.size() << std::endl;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    
    // Optional: Log decoding stats
    static std::atomic<uint64_t> decode_count{0};
    decode_count++;
    
    if (decode_count % 1000 == 0) {
        std::cout << "[KDV Decode] count=" << decode_count
                  << ", time_us=" << (duration_ns / 1000.0)
                  << std::endl;
    }
    
    return result;
}

} // namespace kdv
} // namespace mako
