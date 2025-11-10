#ifndef MAKO_DELTA_COMPACTION_H
#define MAKO_DELTA_COMPACTION_H

#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include "delta_chain_manager.h"

namespace mako {

/**
 * DeltaCompactionWorker manages background compaction of delta chains.
 * 
 * This class provides:
 * - Background thread that periodically checks for chains needing compaction
 * - Configurable compaction interval
 * - Graceful shutdown
 * - Statistics tracking
 */
class DeltaCompactionWorker {
public:
    static DeltaCompactionWorker& getInstance();

    /**
     * Start the background compaction worker
     * @param interval_ms Compaction check interval in milliseconds
     * @param callback Optional callback to persist compacted values to disk
     */
    void start(uint32_t interval_ms = 1000, 
               std::function<void(const std::string& key, const std::string& value, uint64_t version)> callback = nullptr);

    /**
     * Stop the background compaction worker
     */
    void stop();

    /**
     * Check if worker is running
     */
    bool isRunning() const { return running_.load(); }

    /**
     * Trigger immediate compaction check (for testing)
     */
    void triggerCompaction();

    /**
     * Get compaction statistics
     */
    struct CompactionStats {
        uint64_t total_compactions;
        uint64_t total_keys_compacted;
        uint64_t total_deltas_collapsed;
        uint64_t total_bytes_saved;
        uint64_t avg_compaction_time_us;
    };
    CompactionStats getStats() const;

private:
    DeltaCompactionWorker();
    ~DeltaCompactionWorker();

    DeltaCompactionWorker(const DeltaCompactionWorker&) = delete;
    DeltaCompactionWorker& operator=(const DeltaCompactionWorker&) = delete;

    void workerThread();
    void performCompaction();

    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_flag_{false};
    uint32_t interval_ms_{1000};

    std::function<void(const std::string& key, const std::string& value, uint64_t version)> persist_callback_;

    // Statistics
    std::atomic<uint64_t> total_compactions_{0};
    std::atomic<uint64_t> total_keys_compacted_{0};
    std::atomic<uint64_t> total_deltas_collapsed_{0};
    std::atomic<uint64_t> total_bytes_saved_{0};
    std::atomic<uint64_t> total_compaction_time_us_{0};
};

} // namespace mako

#endif // MAKO_DELTA_COMPACTION_H
