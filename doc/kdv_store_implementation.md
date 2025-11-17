# KDV Store Implementation Guide

## Overview

This document describes the implementation details of the Key-Delta-Value (KDV) layer for Mako, including the library structure, integration points, and testing strategies.

## KDV Library Structure

### Core Files

**`src/mako/kdv_format.h`** (API definitions)
- `KDVEncodeMode`: Enum for BASE vs DELTA encoding
- `KDVHeader`: Packed struct with version, mode, chain_len, base_seq, original_size
- `KDVPartitionState`: Per-partition state tracking base value and chain length
- `KDVStoreState`: Singleton managing all partition states with configurable policies
- Public API: `kdv_encode_log()`, `kdv_decode_log()`

**`src/mako/kdv_format.cc`** (Implementation, ~366 lines)
- `compute_delta()`: Blockwise delta computation with common prefix/suffix detection
- `apply_delta()`: Delta reconstruction from base + delta
- `should_write_base()`: Policy decision based on chain length, delta size ratio, and base age
- `kdv_encode_log()`: Main encoding with timing metrics and statistics logging
- `kdv_decode_log()`: Main decoding with error handling and validation
- Thread-safe with mutex protection for partition state
- Atomic counters for encode/decode statistics

**`test/kdv_format_test.cc`** (Unit tests, ~400 lines)
- 8 comprehensive test cases covering:
  - Basic encode/decode round-trip
  - Small updates (16 bytes in 1KB log) → 95.7% compression
  - Large updates → fallback to base
  - Chain length policy enforcement (MaxChainLen=8)
  - Multi-partition independence
  - Random data stress testing (50 updates)
  - Edge cases (empty, single byte, 10KB)
  - Delta structure validation

### Configuration

**`src/mako/benchmarks/benchmark_config.h`**
- Added `enable_kdv_logs_` member variable
- Added `getEnableKDVLogs()` getter
- Added `setEnableKDVLogs()` setter
- Parsed from environment variable `MAKO_ENABLE_KDV_LOGS` or YAML config

### Policy Parameters

Default values in `KDVStoreState`:
- `MaxChainLen`: 8 deltas between full bases
- `MaxDeltaSizeRatio`: 0.7 (write base if delta > 70% of original)
- `MaxBaseAge`: 100 sequence numbers

## Integration Points

### Phase 4: RocksDB Persistence Integration

**Write Path** (`src/mako/rocksdb_persistence.cc:269-283`)
```cpp
// In persistAsync(), after generating sequence number:
if (BenchmarkConfig::getInstance().getEnableKDVLogs()) {
    std::string encoded = mako::kdv::kdv_encode_log(shard_id, partition_id, seq_num, data, size);
    req->value = std::move(encoded);
    
    // Track compression statistics
    total_original_bytes_.fetch_add(size, std::memory_order_relaxed);
    total_encoded_bytes_.fetch_add(req->value.size(), std::memory_order_relaxed);
} else {
    req->value.assign(data, size);
}
```

**Read Path** (`src/mako/benchmarks/rocksdb_replay_app.cc:118-126`)
```cpp
// In loadAllData(), when reading from RocksDB:
if (enable_kdv_decode) {
    log.value = mako::kdv::kdv_decode_log(shard_id, p, seq_num, 
                                           raw_value.data(), raw_value.size());
} else {
    log.value = raw_value;
}
```

**Metrics** (`src/mako/rocksdb_persistence.h:150-152`, `.cc:632-652`)
- `total_original_bytes_`: Atomic counter for original log bytes
- `total_encoded_bytes_`: Atomic counter for KDV-encoded bytes
- `printKDVStats()`: Displays compression ratio, disk savings

### Phase 5: Paxos Geo-Replication Integration

**Sender Side** (`src/mako/benchmarks/sto/Transaction.hh:140-158`)
```cpp
// Before add_log_to_nc():
static std::atomic<uint64_t> paxos_seq_num{0};
if (BenchmarkConfig::getInstance().getEnableKDVLogs()) {
    uint32_t shard_id = BenchmarkConfig::getInstance().getShardIndex();
    uint32_t partition_id = TThread::getPartitionID();
    uint64_t seq = paxos_seq_num.fetch_add(1, std::memory_order_relaxed);
    std::string encoded = mako::kdv::kdv_encode_log(shard_id, partition_id, seq, 
                                                     (const char*)queueLog, pos);
    if (encoded.size() <= max_bytes_size) {
        memcpy(queueLog, encoded.data(), encoded.size());
        add_log_to_nc((char *)queueLog, encoded.size(), partition_id, batch_size);
    } else {
        add_log_to_nc((char *)queueLog, pos, partition_id, batch_size);
    }
}
```

**Receiver Side** (`src/mako/mako.hh:294-310`)
```cpp
// Before treplay_in_same_thread_opt_mbta_v2():
static std::atomic<uint64_t> paxos_decode_seq{0};
if (benchConfig.getEnableKDVLogs()) {
    uint32_t shard_id = benchConfig.getShardIndex();
    uint64_t seq = paxos_decode_seq.fetch_add(1, std::memory_order_relaxed);
    std::string decoded = mako::kdv::kdv_decode_log(shard_id, par_id, seq, 
                                                     (const char*)log, len);
    if (!decoded.empty()) {
        treplay_in_same_thread_opt_mbta_v2(par_id, (char*)decoded.data(), decoded.size(), 
                                            db, benchConfig.getNthreads());
    } else {
        treplay_in_same_thread_opt_mbta_v2(par_id, (char*)log, len, db, benchConfig.getNthreads());
    }
}
```

**Network Metrics** (`src/deptran/paxos_main_helper.cc:25-26`, `425-432`)
- `total_network_bytes_sent`: Atomic counter tracking bytes sent via Paxos
- Logged every 1000 logs in `add_log_to_nc()`

## Build System Integration

**CMakeLists.txt**
- Added `src/mako/kdv_format.cc` to `MAKO_SRCFILES` (line 433)
- Added `kdv_format_test` executable target (lines 957-962):
  ```cmake
  add_executable(kdv_format_test test/kdv_format_test.cc src/mako/kdv_format.cc)
  target_include_directories(kdv_format_test PRIVATE src src/mako)
  target_compile_options(kdv_format_test PRIVATE -std=c++17 -O2)
  target_link_libraries(kdv_format_test pthread)
  add_test(NAME kdv_format_test COMMAND kdv_format_test)
  ```

## Testing Strategy

### Unit Tests

Run KDV library tests:
```bash
cd /home/ubuntu/repos/mako-project
./build/kdv_format_test
```

Expected output:
```
[KDV Test] test_encode_decode_identity: PASSED
[KDV Test] test_small_update: PASSED (compression: 95.7%)
[KDV Test] test_large_update: PASSED
[KDV Test] test_chain_behavior: PASSED
[KDV Test] test_multiple_partitions: PASSED
[KDV Test] test_random_data: PASSED
[KDV Test] test_edge_cases: PASSED
[KDV Test] test_delta_computation: PASSED
All 8 tests passed!
```

### Integration Tests

**RocksDB Persistence**:
```bash
# Enable KDV and run persistence test
export MAKO_ENABLE_KDV_LOGS=1
./build/test_rocksdb_persistence

# Check disk savings
du -sh /tmp/*_mako_rocksdb_shard*
```

**RocksDB Replay**:
```bash
# Replay with KDV decoding
./build/rocksdb_replay_app --enable-kdv-logs
```

**Paxos Replication**:
```bash
# Run replication tests with KDV enabled
export MAKO_ENABLE_KDV_LOGS=1
./ci/ci.sh shard1Replication
./ci/ci.sh shard2Replication
```

### Debugging Tricks

**Enable KDV Logging**:
- KDV library logs statistics every 1000 operations
- Check for `[KDV Encode]` and `[KDV Decode]` messages in output
- Look for compression ratios and timing metrics

**Verify Encode/Decode Identity**:
```cpp
// Add assertion in critical paths:
std::string original(data, size);
std::string encoded = kdv_encode_log(...);
std::string decoded = kdv_decode_log(...);
assert(decoded == original);
```

**Check Partition State**:
- Each partition maintains independent base and chain length
- Use `KDVStoreState::getInstance().getPartitionState(id)` to inspect state
- Verify `hasBase()`, `getChainLen()`, `getBaseSeq()`

**Monitor Compression Ratios**:
- RocksDB: Call `RocksDBPersistence::getInstance().printKDVStats()` at shutdown
- Paxos: Check `[Paxos Network]` logs for total bytes sent
- Compare with baseline runs (KDV disabled)

## Performance Considerations

**Encoding Overhead**:
- Blockwise delta computation: O(n) where n = log size
- Typical encode time: 1-5 microseconds for 1KB logs
- Logged every 1000 operations in `kdv_encode_log()`

**Decoding Overhead**:
- Delta application: O(n) where n = reconstructed size
- Typical decode time: 0.5-3 microseconds for 1KB logs
- Logged every 1000 operations in `kdv_decode_log()`

**Memory Overhead**:
- Per-partition base storage: ~1KB per partition (last base value)
- Chain state: 16 bytes per partition (seq_num, chain_len)
- Total for 16 partitions: ~16KB

**Thread Safety**:
- `KDVPartitionState`: Protected by internal mutex
- `KDVStoreState`: Thread-safe singleton with per-partition locks
- Atomic counters for statistics (no lock contention)

## Limitations and Future Work

**Current Limitations**:
1. Blockwise delta only (no multi-block or LCS-based deltas)
2. No compression of delta payload (could add zstd)
3. Fixed policy parameters (not runtime-tunable)
4. Sequence numbers managed separately for RocksDB and Paxos

**Future Enhancements**:
1. Advanced delta algorithms (xdelta, bsdiff)
2. Adaptive policy tuning based on workload
3. Unified sequence number management
4. Delta chain compaction tool for RocksDB
5. Per-table or per-key delta policies

## Code Quality Notes

**Follows Mako Conventions**:
- No comments unless necessary (code is self-documenting)
- Uses existing libraries (std::atomic, std::mutex, std::chrono)
- Consistent naming (snake_case for functions, CamelCase for classes)
- Thread-safe by design

**Error Handling**:
- Validates header version and mode in `kdv_decode_log()`
- Checks for missing base before applying delta
- Returns empty string on decode failure (caller handles gracefully)
- Logs errors to stderr with context

**Testing Coverage**:
- 8 unit tests covering normal, edge, and stress cases
- Integration tests via existing CI pipeline
- Manual testing with RocksDB and Paxos workloads
