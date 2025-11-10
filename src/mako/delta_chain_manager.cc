#include "delta_chain_manager.h"
#include <chrono>
#include <algorithm>

namespace mako {

DeltaChainManager::DeltaChainManager() {}

DeltaChainManager::~DeltaChainManager() {}

DeltaChainManager& DeltaChainManager::getInstance() {
    static DeltaChainManager instance;
    return instance;
}

uint64_t DeltaChainManager::getCurrentTimeMs() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

void DeltaChainManager::invalidateCache(ChainEntry& entry) {
    entry.cache_valid = false;
    entry.cached_value.clear();
}

bool DeltaChainManager::storeDelta(const std::string& key, const DeltaRecord& delta) {
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    
    auto it = chains_.find(key);
    if (it == chains_.end()) {
        // Create new chain entry
        auto entry = std::make_unique<ChainEntry>();
        entry->creation_time = getCurrentTimeMs();
        entry->last_access_time = entry->creation_time;
        
        // If this is a full value, store it as base
        if (delta.type == DeltaType::FULL_VALUE) {
            entry->chain.base_value = delta.payload;
            entry->chain.base_version = delta.version;
            entry->chain.chain_length = 0;
        } else {
            // This shouldn't happen - first write should be full value
            // But handle it gracefully by treating payload as base
            entry->chain.base_value = delta.payload;
            entry->chain.base_version = delta.version;
            entry->chain.chain_length = 0;
        }
        
        chains_[key] = std::move(entry);
        return true;
    }
    
    auto& entry = it->second;
    std::lock_guard<std::mutex> entry_lock(entry->mutex);
    
    // Add delta to chain
    try {
        entry->chain.addDelta(delta);
        entry->last_access_time = getCurrentTimeMs();
        invalidateCache(*entry);
        
        // Update max chain length stat
        uint64_t current_max = g_delta_stats.max_chain_length.load();
        while (entry->chain.chain_length > current_max) {
            if (g_delta_stats.max_chain_length.compare_exchange_weak(current_max, entry->chain.chain_length)) {
                break;
            }
        }
        
        // Check if compaction is needed
        return !needsCompaction(key);
    } catch (const std::exception& e) {
        // Delta version mismatch or other error
        return false;
    }
}

bool DeltaChainManager::getValue(const std::string& key, std::string& value) {
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    
    auto it = chains_.find(key);
    if (it == chains_.end()) {
        return false;
    }
    
    auto& entry = it->second;
    std::lock_guard<std::mutex> entry_lock(entry->mutex);
    
    // Check cache first
    if (entry->cache_valid) {
        value = entry->cached_value;
        entry->last_access_time = getCurrentTimeMs();
        return true;
    }
    
    // Reconstruct from base + deltas
    auto start_time = std::chrono::high_resolution_clock::now();
    value = entry->chain.reconstruct();
    auto end_time = std::chrono::high_resolution_clock::now();
    
    // Update read latency stats
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    g_delta_stats.read_latency_us_sum.fetch_add(duration_us);
    g_delta_stats.read_count.fetch_add(1);
    
    // Cache the reconstructed value
    entry->cached_value = value;
    entry->cache_valid = true;
    entry->last_access_time = getCurrentTimeMs();
    
    return true;
}

void DeltaChainManager::storeBaseValue(const std::string& key, const std::string& value, uint64_t version) {
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    
    auto it = chains_.find(key);
    if (it == chains_.end()) {
        // Create new chain entry
        auto entry = std::make_unique<ChainEntry>();
        entry->creation_time = getCurrentTimeMs();
        entry->last_access_time = entry->creation_time;
        entry->chain.base_value = value;
        entry->chain.base_version = version;
        entry->chain.chain_length = 0;
        chains_[key] = std::move(entry);
    } else {
        auto& entry = it->second;
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        entry->chain.base_value = value;
        entry->chain.base_version = version;
        entry->chain.deltas.clear();
        entry->chain.chain_length = 0;
        entry->last_access_time = getCurrentTimeMs();
        invalidateCache(*entry);
    }
}

bool DeltaChainManager::needsCompaction(const std::string& key) const {
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    
    auto it = chains_.find(key);
    if (it == chains_.end()) {
        return false;
    }
    
    auto& entry = it->second;
    std::lock_guard<std::mutex> entry_lock(entry->mutex);
    
    // Check chain length
    if (entry->chain.chain_length >= g_delta_config.max_chain_length) {
        return true;
    }
    
    // Check total bytes
    if (entry->chain.getTotalBytes() >= g_delta_config.max_chain_bytes) {
        return true;
    }
    
    // Check age
    uint64_t age_ms = getCurrentTimeMs() - entry->creation_time;
    if (age_ms >= g_delta_config.max_chain_age_ms && entry->chain.chain_length > 0) {
        return true;
    }
    
    return false;
}

bool DeltaChainManager::compactChain(const std::string& key) {
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    
    auto it = chains_.find(key);
    if (it == chains_.end()) {
        return false;
    }
    
    auto& entry = it->second;
    std::lock_guard<std::mutex> entry_lock(entry->mutex);
    
    if (entry->chain.chain_length == 0) {
        // Nothing to compact
        return true;
    }
    
    // Compact the chain
    entry->chain.compact();
    invalidateCache(*entry);
    
    // Update stats
    g_delta_stats.compactions_performed.fetch_add(1);
    
    return true;
}

std::vector<std::string> DeltaChainManager::getKeysNeedingCompaction() const {
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    
    std::vector<std::string> keys;
    for (const auto& pair : chains_) {
        if (needsCompaction(pair.first)) {
            keys.push_back(pair.first);
        }
    }
    
    return keys;
}

void DeltaChainManager::clear() {
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    chains_.clear();
}

DeltaChainManager::ChainStats DeltaChainManager::getStats() const {
    std::lock_guard<std::mutex> global_lock(global_mutex_);
    
    ChainStats stats;
    stats.total_chains = chains_.size();
    stats.total_deltas = 0;
    stats.max_chain_length = 0;
    stats.total_bytes = 0;
    stats.cached_values = 0;
    
    for (const auto& pair : chains_) {
        const auto& entry = pair.second;
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        
        stats.total_deltas += entry->chain.chain_length;
        stats.max_chain_length = std::max(stats.max_chain_length, (size_t)entry->chain.chain_length);
        stats.total_bytes += entry->chain.getTotalBytes();
        if (entry->cache_valid) {
            stats.cached_values++;
        }
    }
    
    return stats;
}

} // namespace mako
