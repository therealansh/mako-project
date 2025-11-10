#include "delta_compaction.h"
#include <chrono>
#include <thread>
#include <iostream>

namespace mako {

DeltaCompactionWorker::DeltaCompactionWorker() {}

DeltaCompactionWorker::~DeltaCompactionWorker() {
    stop();
}

DeltaCompactionWorker& DeltaCompactionWorker::getInstance() {
    static DeltaCompactionWorker instance;
    return instance;
}

void DeltaCompactionWorker::start(uint32_t interval_ms, 
                                   std::function<void(const std::string& key, const std::string& value, uint64_t version)> callback) {
    if (running_.load()) {
        return;  // Already running
    }

    interval_ms_ = interval_ms;
    persist_callback_ = callback;
    shutdown_flag_ = false;
    running_ = true;

    worker_thread_ = std::thread(&DeltaCompactionWorker::workerThread, this);
    
    std::cout << "[DeltaCompaction] Background compaction worker started (interval=" 
              << interval_ms_ << "ms)" << std::endl;
}

void DeltaCompactionWorker::stop() {
    if (!running_.load()) {
        return;  // Not running
    }

    shutdown_flag_ = true;
    
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    
    running_ = false;
    
    std::cout << "[DeltaCompaction] Background compaction worker stopped" << std::endl;
}

void DeltaCompactionWorker::triggerCompaction() {
    if (running_.load()) {
        performCompaction();
    }
}

void DeltaCompactionWorker::workerThread() {
    while (!shutdown_flag_.load()) {
        // Perform compaction
        performCompaction();
        
        // Sleep for interval
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
}

void DeltaCompactionWorker::performCompaction() {
    if (!isDeltaEnabled()) {
        return;  // Delta replication disabled
    }

    auto& manager = DeltaChainManager::getInstance();
    
    // Get keys that need compaction
    auto keys = manager.getKeysNeedingCompaction();
    
    if (keys.empty()) {
        return;  // Nothing to compact
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    
    size_t keys_compacted = 0;
    size_t deltas_collapsed = 0;
    size_t bytes_saved = 0;
    
    for (const auto& key : keys) {
        // Get chain stats before compaction
        std::string value;
        if (!manager.getValue(key, value)) {
            continue;  // Key doesn't exist anymore
        }
        
        auto stats_before = manager.getStats();
        
        // Perform compaction
        if (manager.compactChain(key)) {
            keys_compacted++;
            
            auto stats_after = manager.getStats();
            deltas_collapsed += (stats_before.total_deltas - stats_after.total_deltas);
            bytes_saved += (stats_before.total_bytes - stats_after.total_bytes);
            
            // If persist callback is provided, persist the compacted value
            if (persist_callback_) {
                // Get the compacted base value and version
                std::string compacted_value;
                if (manager.getValue(key, compacted_value)) {
                    persist_callback_(key, compacted_value, 0);  // Version 0 for now
                }
            }
        }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    // Update statistics
    total_compactions_.fetch_add(1);
    total_keys_compacted_.fetch_add(keys_compacted);
    total_deltas_collapsed_.fetch_add(deltas_collapsed);
    total_bytes_saved_.fetch_add(bytes_saved);
    total_compaction_time_us_.fetch_add(duration_us);
    
    if (keys_compacted > 0) {
        std::cout << "[DeltaCompaction] Compacted " << keys_compacted << " keys, "
                  << "collapsed " << deltas_collapsed << " deltas, "
                  << "saved " << bytes_saved << " bytes, "
                  << "took " << duration_us << " us" << std::endl;
    }
}

DeltaCompactionWorker::CompactionStats DeltaCompactionWorker::getStats() const {
    CompactionStats stats;
    stats.total_compactions = total_compactions_.load();
    stats.total_keys_compacted = total_keys_compacted_.load();
    stats.total_deltas_collapsed = total_deltas_collapsed_.load();
    stats.total_bytes_saved = total_bytes_saved_.load();
    
    uint64_t total_time = total_compaction_time_us_.load();
    uint64_t total_compactions = total_compactions_.load();
    stats.avg_compaction_time_us = total_compactions > 0 ? total_time / total_compactions : 0;
    
    return stats;
}

} // namespace mako
