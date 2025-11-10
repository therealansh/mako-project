#include "delta_store.h"
#include <cstring>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <tuple>
#include <zlib.h>  // For CRC32

namespace mako {

// Global instances
DeltaStats g_delta_stats;
DeltaConfig g_delta_config;

// ============================================================================
// DeltaRecord Implementation
// ============================================================================

std::string DeltaRecord::serialize() const {
    std::ostringstream oss;
    
    // Write fixed-size fields
    oss.write(reinterpret_cast<const char*>(&type), sizeof(type));
    oss.write(reinterpret_cast<const char*>(&version), sizeof(version));
    oss.write(reinterpret_cast<const char*>(&base_version), sizeof(base_version));
    oss.write(reinterpret_cast<const char*>(&original_size), sizeof(original_size));
    oss.write(reinterpret_cast<const char*>(&timestamp), sizeof(timestamp));
    oss.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
    
    // Write payload size and data
    uint32_t payload_size = payload.size();
    oss.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
    oss.write(payload.data(), payload_size);
    
    return oss.str();
}

DeltaRecord DeltaRecord::deserialize(const std::string& data) {
    DeltaRecord record;
    std::istringstream iss(data);
    
    // Read fixed-size fields
    iss.read(reinterpret_cast<char*>(&record.type), sizeof(record.type));
    iss.read(reinterpret_cast<char*>(&record.version), sizeof(record.version));
    iss.read(reinterpret_cast<char*>(&record.base_version), sizeof(record.base_version));
    iss.read(reinterpret_cast<char*>(&record.original_size), sizeof(record.original_size));
    iss.read(reinterpret_cast<char*>(&record.timestamp), sizeof(record.timestamp));
    iss.read(reinterpret_cast<char*>(&record.checksum), sizeof(record.checksum));
    
    // Read payload
    uint32_t payload_size;
    iss.read(reinterpret_cast<char*>(&payload_size), sizeof(payload_size));
    record.payload.resize(payload_size);
    iss.read(&record.payload[0], payload_size);
    
    return record;
}

// ============================================================================
// DeltaMetadata Implementation
// ============================================================================

std::string DeltaMetadata::serialize() const {
    std::ostringstream oss;
    oss.write(reinterpret_cast<const char*>(&current_version), sizeof(current_version));
    oss.write(reinterpret_cast<const char*>(&base_version), sizeof(base_version));
    oss.write(reinterpret_cast<const char*>(&delta_count), sizeof(delta_count));
    oss.write(reinterpret_cast<const char*>(&last_compaction_time), sizeof(last_compaction_time));
    oss.write(reinterpret_cast<const char*>(&total_delta_bytes), sizeof(total_delta_bytes));
    return oss.str();
}

DeltaMetadata DeltaMetadata::deserialize(const std::string& data) {
    DeltaMetadata meta;
    std::istringstream iss(data);
    iss.read(reinterpret_cast<char*>(&meta.current_version), sizeof(meta.current_version));
    iss.read(reinterpret_cast<char*>(&meta.base_version), sizeof(meta.base_version));
    iss.read(reinterpret_cast<char*>(&meta.delta_count), sizeof(meta.delta_count));
    iss.read(reinterpret_cast<char*>(&meta.last_compaction_time), sizeof(meta.last_compaction_time));
    iss.read(reinterpret_cast<char*>(&meta.total_delta_bytes), sizeof(meta.total_delta_bytes));
    return meta;
}

// ============================================================================
// DeltaChain Implementation
// ============================================================================

std::string DeltaChain::reconstruct() const {
    std::string result = base_value;
    for (const auto& delta : deltas) {
        result = DeltaComputer::applyDelta(result, delta);
    }
    return result;
}

void DeltaChain::addDelta(const DeltaRecord& delta) {
    // Verify version sequence
    if (!deltas.empty()) {
        uint64_t expected_version = deltas.back().version + 1;
        if (delta.version != expected_version) {
            throw std::runtime_error("Delta version out of order!");
        }
    }
    
    // Verify base version matches current version
    uint64_t current_version = deltas.empty() ? base_version : deltas.back().version;
    if (delta.base_version != current_version) {
        throw std::runtime_error("Delta base version mismatch!");
    }
    
    deltas.push_back(delta);
    chain_length = deltas.size();
}

void DeltaChain::compact() {
    if (!deltas.empty()) {
        base_value = reconstruct();
        base_version = deltas.back().version;
        deltas.clear();
        chain_length = 0;
    }
}

// ============================================================================
// DeltaComputer Implementation
// ============================================================================

uint32_t DeltaComputer::computeChecksum(const std::string& data) {
    return crc32(0L, reinterpret_cast<const Bytef*>(data.data()), data.size());
}

DeltaType DeltaComputer::selectDeltaType(const std::string& old_value,
                                        const std::string& new_value) {
    // Always use FULL_VALUE for small values (overhead not worth it)
    if (new_value.size() < g_delta_config.size_threshold) {
        return DeltaType::FULL_VALUE;
    }
    
    // Estimate diff size
    size_t diff_size = estimateDiffSize(old_value, new_value);
    
    // Use diff if it saves > 30% bandwidth
    if (diff_size < new_value.size() * 0.7) {
        return DeltaType::SIMPLE_DIFF;
    }
    
    return DeltaType::FULL_VALUE;
}

size_t DeltaComputer::estimateDiffSize(const std::string& old_value,
                                       const std::string& new_value) {
    // Quick estimation: count different bytes
    size_t min_size = std::min(old_value.size(), new_value.size());
    size_t max_size = std::max(old_value.size(), new_value.size());
    size_t diff_bytes = 0;
    
    for (size_t i = 0; i < min_size; ++i) {
        if (old_value[i] != new_value[i]) {
            diff_bytes++;
        }
    }
    
    // Add size difference
    diff_bytes += (max_size - min_size);
    
    // Estimate overhead: 12 bytes per chunk (offset + length + data)
    // Assume chunks of ~16 bytes on average
    size_t estimated_chunks = (diff_bytes + 15) / 16;
    return estimated_chunks * 12 + diff_bytes;
}

std::string DeltaComputer::computeSimpleDiff(const std::string& old_value,
                                            const std::string& new_value) {
    std::ostringstream diff;
    
    // Find contiguous regions of differences
    std::vector<std::tuple<uint32_t, uint32_t, std::string>> chunks;
    
    size_t i = 0;
    size_t new_size = new_value.size();
    size_t old_size = old_value.size();
    
    while (i < new_size) {
        // Find start of difference
        while (i < std::min(new_size, old_size) && old_value[i] == new_value[i]) {
            i++;
        }
        
        if (i >= new_size) break;
        
        // Find end of difference
        size_t start = i;
        while (i < new_size && (i >= old_size || old_value[i] != new_value[i])) {
            i++;
        }
        
        // Add chunk
        uint32_t offset = start;
        uint32_t length = i - start;
        std::string data = new_value.substr(start, length);
        chunks.push_back(std::make_tuple(offset, length, data));
    }
    
    // Serialize chunks
    uint32_t num_chunks = chunks.size();
    diff.write(reinterpret_cast<const char*>(&num_chunks), sizeof(num_chunks));
    
    for (const auto& chunk : chunks) {
        uint32_t offset = std::get<0>(chunk);
        uint32_t length = std::get<1>(chunk);
        const std::string& data = std::get<2>(chunk);
        
        diff.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
        diff.write(reinterpret_cast<const char*>(&length), sizeof(length));
        diff.write(data.data(), length);
    }
    
    return diff.str();
}

std::string DeltaComputer::applySimpleDiff(const std::string& base,
                                          const std::string& diff_data,
                                          uint32_t original_size) {
    std::istringstream diff(diff_data);
    std::string result = base;
    
    // Ensure result is large enough
    if (result.size() < original_size) {
        result.resize(original_size);
    }
    
    // Read number of chunks
    uint32_t num_chunks;
    diff.read(reinterpret_cast<char*>(&num_chunks), sizeof(num_chunks));
    
    // Apply each chunk
    for (uint32_t i = 0; i < num_chunks; ++i) {
        uint32_t offset, length;
        diff.read(reinterpret_cast<char*>(&offset), sizeof(offset));
        diff.read(reinterpret_cast<char*>(&length), sizeof(length));
        
        // Ensure result is large enough for this chunk
        if (offset + length > result.size()) {
            result.resize(offset + length);
        }
        
        // Read and apply chunk data
        diff.read(&result[offset], length);
    }
    
    // Trim to original size
    result.resize(original_size);
    
    return result;
}

DeltaRecord DeltaComputer::computeDelta(const std::string& old_value,
                                       const std::string& new_value,
                                       uint64_t version,
                                       uint64_t base_version,
                                       uint64_t timestamp) {
    DeltaRecord delta;
    delta.version = version;
    delta.base_version = base_version;
    delta.original_size = new_value.size();
    delta.timestamp = timestamp;
    delta.checksum = computeChecksum(new_value);
    
    // Select delta type
    delta.type = selectDeltaType(old_value, new_value);
    
    // Compute payload based on type
    switch (delta.type) {
        case DeltaType::FULL_VALUE:
            delta.payload = new_value;
            g_delta_stats.bytes_sent_full.fetch_add(new_value.size());
            break;
            
        case DeltaType::SIMPLE_DIFF:
            delta.payload = computeSimpleDiff(old_value, new_value);
            g_delta_stats.bytes_sent_delta.fetch_add(delta.payload.size());
            break;
            
        case DeltaType::OPERATION_LOG:
            // Not implemented yet, fallback to full value
            delta.type = DeltaType::FULL_VALUE;
            delta.payload = new_value;
            g_delta_stats.bytes_sent_full.fetch_add(new_value.size());
            break;
    }
    
    g_delta_stats.deltas_created.fetch_add(1);
    
    return delta;
}

std::string DeltaComputer::applyDelta(const std::string& base,
                                     const DeltaRecord& delta) {
    std::string result;
    
    switch (delta.type) {
        case DeltaType::FULL_VALUE:
            result = delta.payload;
            break;
            
        case DeltaType::SIMPLE_DIFF:
            result = applySimpleDiff(base, delta.payload, delta.original_size);
            break;
            
        case DeltaType::OPERATION_LOG:
            throw std::runtime_error("OPERATION_LOG delta type not implemented");
            
        default:
            throw std::runtime_error("Unknown delta type");
    }
    
    // Verify checksum
    uint32_t computed_checksum = computeChecksum(result);
    if (computed_checksum != delta.checksum) {
        throw std::runtime_error("Delta checksum mismatch!");
    }
    
    g_delta_stats.deltas_applied.fetch_add(1);
    
    return result;
}

// ============================================================================
// DeltaStats Implementation
// ============================================================================

void DeltaStats::print() const {
    double bandwidth_reduction = getBandwidthReduction();
    double avg_read_latency = read_count.load() > 0 ? 
        (double)read_latency_us_sum.load() / read_count.load() : 0.0;
    double avg_write_latency = write_count.load() > 0 ?
        (double)write_latency_us_sum.load() / write_count.load() : 0.0;
    
    std::cout << "=== Delta Replication Metrics ===" << std::endl;
    std::cout << "Bandwidth reduction: " << bandwidth_reduction << "%" << std::endl;
    std::cout << "Bytes sent (full): " << bytes_sent_full.load() << std::endl;
    std::cout << "Bytes sent (delta): " << bytes_sent_delta.load() << std::endl;
    std::cout << "Avg read latency: " << avg_read_latency << " us" << std::endl;
    std::cout << "Avg write latency: " << avg_write_latency << " us" << std::endl;
    std::cout << "Deltas created: " << deltas_created.load() << std::endl;
    std::cout << "Deltas applied: " << deltas_applied.load() << std::endl;
    std::cout << "Compactions: " << compactions_performed.load() << std::endl;
    std::cout << "Max chain length: " << max_chain_length.load() << std::endl;
}

void DeltaStats::reset() {
    bytes_sent_full.store(0);
    bytes_sent_delta.store(0);
    deltas_created.store(0);
    deltas_applied.store(0);
    compactions_performed.store(0);
    max_chain_length.store(0);
    read_latency_us_sum.store(0);
    read_count.store(0);
    write_latency_us_sum.store(0);
    write_count.store(0);
}

// ============================================================================
// DeltaConfig Implementation
// ============================================================================

DeltaConfig DeltaConfig::loadFromEnv() {
    DeltaConfig config;
    
    // Check if delta replication is enabled
    const char* enabled_env = std::getenv("MAKO_ENABLE_DELTA_REPLICATION");
    config.enabled = (enabled_env != nullptr && std::string(enabled_env) == "1");
    
    // Load thresholds
    const char* size_threshold_env = std::getenv("MAKO_DELTA_SIZE_THRESHOLD");
    if (size_threshold_env) {
        config.size_threshold = std::stoul(size_threshold_env);
    }
    
    const char* max_chain_length_env = std::getenv("MAKO_MAX_CHAIN_LENGTH");
    if (max_chain_length_env) {
        config.max_chain_length = std::stoul(max_chain_length_env);
    }
    
    const char* max_chain_age_env = std::getenv("MAKO_MAX_CHAIN_AGE_MS");
    if (max_chain_age_env) {
        config.max_chain_age_ms = std::stoul(max_chain_age_env);
    }
    
    const char* max_chain_bytes_env = std::getenv("MAKO_MAX_CHAIN_BYTES");
    if (max_chain_bytes_env) {
        config.max_chain_bytes = std::stoul(max_chain_bytes_env);
    }
    
    const char* compaction_interval_env = std::getenv("MAKO_COMPACTION_INTERVAL_MS");
    if (compaction_interval_env) {
        config.compaction_interval_ms = std::stoul(compaction_interval_env);
    }
    
    return config;
}

} // namespace mako
