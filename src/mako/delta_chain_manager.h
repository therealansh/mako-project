#ifndef MAKO_DELTA_CHAIN_MANAGER_H
#define MAKO_DELTA_CHAIN_MANAGER_H

#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include "delta_store.h"

namespace mako {

/**
 * DeltaChainManager manages in-memory delta chains for keys.
 * 
 * This class provides:
 * - Storage and retrieval of delta chains per key
 * - Automatic reconstruction of full values from base + deltas
 * - LRU caching of reconstructed values
 * - Thread-safe access to delta chains
 * - Compaction triggering based on chain length/size/age
 */
class DeltaChainManager {
public:
    static DeltaChainManager& getInstance();

    /**
     * Store a delta for a key
     * @param key The key to store delta for
     * @param delta The delta record to store
     * @return true if delta was stored, false if chain needs compaction
     */
    bool storeDelta(const std::string& key, const DeltaRecord& delta);

    /**
     * Get full value for a key by reconstructing from base + deltas
     * @param key The key to retrieve
     * @param value Output parameter for the reconstructed value
     * @return true if key exists, false otherwise
     */
    bool getValue(const std::string& key, std::string& value);

    /**
     * Store a full base value for a key (used after compaction or initial write)
     * @param key The key to store
     * @param value The full value
     * @param version The version number
     */
    void storeBaseValue(const std::string& key, const std::string& value, uint64_t version);

    /**
     * Check if a key needs compaction
     * @param key The key to check
     * @return true if compaction is needed
     */
    bool needsCompaction(const std::string& key) const;

    /**
     * Compact a delta chain into a base value
     * @param key The key to compact
     * @return true if compaction succeeded
     */
    bool compactChain(const std::string& key);

    /**
     * Get list of keys that need compaction
     * @return vector of keys that need compaction
     */
    std::vector<std::string> getKeysNeedingCompaction() const;

    /**
     * Clear all delta chains (for testing)
     */
    void clear();

    /**
     * Get statistics about delta chains
     */
    struct ChainStats {
        size_t total_chains;
        size_t total_deltas;
        size_t max_chain_length;
        size_t total_bytes;
        size_t cached_values;
    };
    ChainStats getStats() const;

private:
    DeltaChainManager();
    ~DeltaChainManager();

    DeltaChainManager(const DeltaChainManager&) = delete;
    DeltaChainManager& operator=(const DeltaChainManager&) = delete;

    struct ChainEntry {
        DeltaChain chain;
        std::string cached_value;  // LRU cache of reconstructed value
        uint64_t last_access_time;
        uint64_t creation_time;
        bool cache_valid;
        mutable std::mutex mutex;

        ChainEntry() : last_access_time(0), creation_time(0), cache_valid(false) {}
    };

    std::unordered_map<std::string, std::unique_ptr<ChainEntry>> chains_;
    mutable std::mutex global_mutex_;

    // Helper to get current time in milliseconds
    uint64_t getCurrentTimeMs() const;

    // Helper to invalidate cache
    void invalidateCache(ChainEntry& entry);
};

} // namespace mako

#endif // MAKO_DELTA_CHAIN_MANAGER_H
