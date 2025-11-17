# KDV Store Design: Storage Disaggregation with Delta-Based Replication

## Overview

This document describes the design and implementation of a Key-Delta-Value (KDV) layer for Mako that implements storage disaggregation with delta-based replication. The KDV layer sits between transaction logs and both network replication (Paxos) and persistent storage (RocksDB), providing significant bandwidth and disk space savings through delta encoding.

**Motivation**: Inspired by Aurora's log-based storage disaggregation, this design reduces cross-datacenter bandwidth and persistent storage requirements by sending/storing only deltas (differences) between consecutive transaction logs rather than full logs.

**Key Benefits**:
- Reduced cross-datacenter bandwidth (target: 40-70% savings for small updates)
- Reduced persistent storage footprint (target: 40-70% savings)
- Minimal latency overhead (target: <5% increase)
- Transparent to existing transaction processing logic

## Phase 1: Current System Architecture

### 1.1 Log Generation (Write Path)

**Location**: `src/mako/benchmarks/sto/Transaction.hh` (lines 120-163) and `src/mako/benchmarks/sto/Transaction.cc` (lines 600-816)

**Key Components**:
- `StringAllocator`: Per-thread log buffer that accumulates transaction logs
- `queueLog`: Byte array containing serialized transaction operations
- `serialize_util()`: Function that serializes transaction write sets into log format

**Log Format** (current):
```
For each transaction in batch:
  1. Transaction metadata (timestamps, partition info)
  2. For each write operation:
     - Operation type (insert/update/delete)
     - Key length + Key bytes
     - Value length + Value bytes
     - Table ID (with delete flag in high bit)
  3. Batch trailer:
     - latest_commit_timestamp (uint32_t)
     - start_time for latency tracking (uint32_t)
```

**Log Construction Flow**:
```
Transaction commit
  → serialize_util() builds log in StringAllocator buffer
  → When batch full (entries >= batch_size):
    → getLogOnly() retrieves accumulated log
    → Append batch metadata (timestamp, latency tracker)
    → Call add_log_to_nc() for replication
    → Call persistAsync() for RocksDB persistence
    → resetMemory() clears buffer
```

**Key Files**:
- `Transaction.hh:138`: Main call to `add_log_to_nc((char *)queueLog, pos, TThread::getPartitionID(), batch_size)`
- `Transaction.hh:146`: Main call to `persistence.persistAsync((const char*)queueLog, pos, shard_id, TThread::getPartitionID(), callback)`
- `Transaction.cc:777`: Helper thread call to `add_log_to_nc()`
- `Transaction.cc:792`: Helper thread call to `persistAsync()`

### 1.2 Geo-Replication Path (Paxos)

**Location**: `src/deptran/paxos_main_helper.cc` (lines 411-426)

**Key Function**: `add_log_to_nc(const char* log, int len, uint32_t par_id, int batch_size)`

**Replication Flow**:
```
add_log_to_nc()
  → Check if current node is leader for partition
  → If leader:
    → Call add_log_without_queue(log, len, par_id)
      → Find PaxosWorker for partition
      → Lock partition mutex
      → worker->IncSubmit()
      → worker->Submit(log, len, par_id)
        → Paxos consensus protocol
        → Replicate to followers
        → Apply to local state machine
```

**Key Observations**:
- Log is sent as opaque byte array (const char* log, int len)
- No interpretation of log contents during replication
- Leader election handled by ElectionState singleton
- Per-partition mutex ensures ordering

**Key Files**:
- `s_main.h:27`: Declaration of `add_log_to_nc()`
- `paxos_main_helper.cc:411-426`: Implementation of `add_log_to_nc()`
- `paxos_main_helper.cc:185-199`: Implementation of `add_log_without_queue()`

### 1.3 RocksDB Persistence

**Location**: `src/mako/rocksdb_persistence.h` and `src/mako/rocksdb_persistence.cc`

**Architecture**:
- **Per-partition databases**: Separate RocksDB instance per partition to eliminate lock contention
- **Asynchronous writes**: Background worker threads handle persistence
- **Ordered callbacks**: Guarantees callbacks execute in sequence number order per partition

**Key Data Structures**:
```cpp
struct PersistRequest {
    std::string key;           // Generated key: "shard:partition:epoch:seq"
    std::string value;         // Log data (queueLog)
    std::function<void(bool)> callback;
    uint64_t sequence_number;  // Per-partition sequence
    uint32_t partition_id;
};

struct PartitionQueue {
    std::queue<PersistRequest> queue;
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::atomic<size_t> pending_writes;
};
```

**Persistence Flow**:
```
persistAsync(data, size, shard_id, partition_id, callback)
  → Generate key: "shard:partition:epoch:seq_num"
  → Create PersistRequest with log data as value
  → Enqueue to partition-specific queue
  → Notify worker thread
  → Worker thread:
    → Dequeue request
    → Write to partition-specific RocksDB: Put(key, value)
    → Call handlePersistComplete()
      → Mark sequence as persisted
      → Process ordered callbacks (execute in sequence order)
```

**Key Layout**:
- Key format: `"{shard_id:03d}:{partition_id:03d}:{epoch:08d}:{seq_num:016d}"`
- Example: `"000:001:00000001:0000000000000042"`
- Lexicographic ordering ensures sequential iteration during replay

**Key Files**:
- `rocksdb_persistence.h:53-144`: RocksDBPersistence class definition
- `rocksdb_persistence.cc:225-310`: `persistAsync()` implementation
- `rocksdb_persistence.cc:155-164`: `generateKey()` implementation
- `rocksdb_persistence.cc:312-398`: Worker thread implementation

### 1.4 Replay/Read Path

**Location**: `src/mako/benchmarks/rocksdb_replay_app.cc`

**Replay Flow**:
```
main()
  → findRocksDBPath() - locate RocksDB directory
  → parseMetadata() - read epoch, shard_id, num_partitions from "meta" key
  → initWithDB_replay() - initialize in-memory database
  → loadAllData():
    → For each partition:
      → Open RocksDB partition database
      → Iterate all keys (skip "meta")
      → Store log values in LoadedLog structures
  → replayWorker() (parallel per partition):
    → For each log:
      → Call treplay_in_same_thread_opt_mbta_v2(partition_id, log.data, log.size, db, num_shards)
        → Deserialize log
        → Apply operations to in-memory Masstree
        → Rebuild database state
```

**Key Observations**:
- Logs are iterated in key order (which is sequence order due to key format)
- Each log value is passed directly to replay function
- Replay function expects current log format (no KDV awareness yet)

**Key Files**:
- `rocksdb_replay_app.cc:94-123`: `loadAllData()` - reads all logs from RocksDB
- `rocksdb_replay_app.cc:73-92`: `replayWorker()` - replays logs in parallel
- `rocksdb_replay_app.cc:80`: Call to `treplay_in_same_thread_opt_mbta_v2()`

### 1.5 Current Pipeline Summary

```
┌─────────────────────────────────────────────────────────────────┐
│                     Transaction Commit                           │
│                            ↓                                     │
│                  serialize_util() builds log                     │
│                            ↓                                     │
│                    queueLog (byte array)                         │
│                            ↓                                     │
│              ┌─────────────┴─────────────┐                      │
│              ↓                           ↓                       │
│    add_log_to_nc()              persistAsync()                   │
│         (Paxos)                  (RocksDB)                       │
│              ↓                           ↓                       │
│    Network replication          Disk persistence                │
│    (full log sent)              (full log stored)               │
│              ↓                           ↓                       │
│    Followers receive            RocksDB key-value:              │
│    full log                     key="shard:part:epoch:seq"      │
│                                 value=queueLog                   │
│                                         ↓                        │
│                                 Replay: iterate keys             │
│                                 treplay_in_same_thread_opt_mbta_v2()│
└─────────────────────────────────────────────────────────────────┘
```

**Key Insight**: The log (queueLog) is treated as an opaque byte array at both replication and persistence layers. This makes it ideal for inserting a KDV encoding layer that is transparent to the rest of the system.

## Phase 2: KDV Layer Design (To be completed)

### 2.1 Abstraction Boundary

The KDV layer will sit between transaction log generation and both network/disk:

```
Transaction commit → queueLog → [KDV ENCODE] → encoded_log → {Paxos, RocksDB}
                                                                    ↓
Recovery/Follower ← decoded_log ← [KDV DECODE] ← encoded_log ← {Paxos, RocksDB}
```

**Key-Delta-Value Terminology**:
- **Key** (at KDV level): `(shard_id, partition_id, sequence_number)` - identifies a log entry
- **Value** (at KDV level): `queueLog` blob - the transaction log data
- **Delta**: Difference between consecutive values for the same partition

### 2.2 Integration Points

Based on Phase 1 analysis, KDV will integrate at these locations:

1. **Encode before Paxos** (`Transaction.hh:138`, `Transaction.cc:777`):
   ```cpp
   // Before: add_log_to_nc((char *)queueLog, pos, ...)
   // After:  std::string encoded = kdv_encode_log(..., queueLog, pos);
   //         add_log_to_nc(encoded.data(), encoded.size(), ...)
   ```

2. **Encode before RocksDB** (`Transaction.hh:146`, `Transaction.cc:792`):
   ```cpp
   // Before: persistAsync((const char*)queueLog, pos, ...)
   // After:  std::string encoded = kdv_encode_log(..., queueLog, pos);
   //         persistAsync(encoded.data(), encoded.size(), ...)
   ```

3. **Decode in Paxos receiver** (to be located in Phase 5):
   ```cpp
   // Before: treplay_in_same_thread_opt_mbta_v2(..., log.data, log.size, ...)
   // After:  std::string decoded = kdv_decode_log(..., log.data, log.size);
   //         treplay_in_same_thread_opt_mbta_v2(..., decoded.data(), decoded.size(), ...)
   ```

4. **Decode in RocksDB replay** (`rocksdb_replay_app.cc:80`):
   ```cpp
   // Before: treplay_in_same_thread_opt_mbta_v2(..., log.value.data(), log.value.size(), ...)
   // After:  std::string decoded = kdv_decode_log(..., log.value.data(), log.value.size());
   //         treplay_in_same_thread_opt_mbta_v2(..., decoded.data(), decoded.size(), ...)
   ```

### 2.3 Delta Format Design

**KDV Header** (16 bytes, packed):
```cpp
struct KDVHeader {
    uint8_t version;           // Format version (currently 1)
    uint8_t mode;              // KDVEncodeMode: BASE=0, DELTA=1
    uint16_t chain_len;        // Number of deltas since last base
    uint64_t base_seq;         // Sequence number of base (0 if this is base)
    uint32_t original_size;    // Original uncompressed size
};
```

**Encoded Log Format**:
```
[KDVHeader (16 bytes)]
[Payload (variable)]

If mode == BASE:
  Payload = original log data (unmodified)

If mode == DELTA:
  Payload = DeltaBlock structure:
    [prefix_len: uint32_t]
    [suffix_len: uint32_t]
    [middle_len: uint32_t]
    [middle_data: middle_len bytes]
```

**Delta Computation Algorithm** (Simple Blockwise):
```
compute_delta(base, value):
  1. Find common prefix length p:
     p = 0
     while p < min(base.size(), value.size()) and base[p] == value[p]:
       p++
  
  2. Find common suffix length s:
     s = 0
     while s < min(base.size() - p, value.size() - p) 
           and base[base.size() - 1 - s] == value[value.size() - 1 - s]:
       s++
  
  3. Extract middle region:
     middle = value[p : value.size() - s]
  
  4. Return DeltaBlock:
     {prefix_len: p, suffix_len: s, middle_len: middle.size(), middle_data: middle}
```

**Delta Application Algorithm**:
```
apply_delta(base, delta):
  1. Parse DeltaBlock from delta
  2. Extract prefix: base[0 : prefix_len]
  3. Extract middle: delta[12 : 12 + middle_len]  // Skip DeltaBlock header
  4. Extract suffix: base[base.size() - suffix_len : base.size()]
  5. Return: prefix + middle + suffix
```

**Compression Ratio Example**:
- Original log: 1024 bytes
- Small update (16 bytes changed in middle): 
  - Delta: 12 bytes (DeltaBlock) + 16 bytes (middle) = 28 bytes
  - Compression: 97.3% savings
- Large update (most bytes changed):
  - Delta might be larger than original
  - Policy: write as BASE instead

### 2.4 Delta Chain Policies

**Policy Parameters** (configurable via `KDVStoreState`):

1. **MaxChainLen** (default: 16)
   - Maximum number of consecutive deltas before forcing a new base
   - Prevents unbounded chain length that would slow down decoding
   - Trade-off: Lower = more bases = less compression, Higher = longer chains = slower decode

2. **MaxDeltaSizeRatio** (default: 0.7)
   - If `delta_size / original_size > 0.7`, write as base instead
   - Prevents storing deltas that don't save much space
   - Example: If delta is 800 bytes for 1000 byte log, write full base instead

3. **MaxBaseAge** (default: 1000 sequences)
   - Maximum sequence number distance from last base
   - Prevents very old bases that might cause issues during recovery
   - Example: If last base was seq 100, force new base at seq 1100

**Decision Algorithm** (`should_write_base()`):
```cpp
bool should_write_base(partition_state, delta_size, original_size, seq_num) {
    // No base exists yet
    if (!partition_state.hasBase()) {
        return true;
    }
    
    // Chain too long
    if (partition_state.getChainLen() >= max_chain_len) {
        return true;
    }
    
    // Delta not efficient
    if (delta_size > max_delta_size_ratio * original_size) {
        return true;
    }
    
    // Base too old
    if (seq_num - partition_state.getBaseSeq() > max_base_age) {
        return true;
    }
    
    return false;  // Write delta
}
```

**Chain Management During Encoding**:
```
For each log to encode:
  1. Get partition state
  2. Compute delta from current base
  3. Check should_write_base()
  4. If write base:
     - Set mode = BASE
     - Store full log as payload
     - Update partition state: setBase(seq_num, log_data)
     - Reset chain_len = 0
  5. Else write delta:
     - Set mode = DELTA
     - Store delta as payload
     - Increment chain_len
     - Set base_seq to current base's sequence
```

**Chain Management During Decoding**:
```
For each log to decode:
  1. Read KDVHeader
  2. If mode == BASE:
     - Update partition state: setBase(seq_num, payload)
     - Return payload
  3. If mode == DELTA:
     - Get base from partition state
     - Apply delta to base
     - Return reconstructed value
     - Note: Don't update partition state (base remains unchanged)
```

### 2.5 Configuration and Deployment

**Configuration Flag** (to be added to `BenchmarkConfig`):
```cpp
class BenchmarkConfig {
    // ... existing fields ...
    bool enable_kdv_logs_;
    
public:
    bool getEnableKDVLogs() const { return enable_kdv_logs_; }
    void setEnableKDVLogs(bool enable) { enable_kdv_logs_ = enable; }
};
```

**Configuration Sources**:
1. Environment variable: `MAKO_ENABLE_KDV_LOGS=1`
2. YAML configuration: `enable_kdv_logs: true`
3. Command-line flag: `--enable-kdv-logs`

**Deployment Strategy**:
1. **Phase 1**: Deploy with KDV disabled (default)
   - Verify no performance regression
   - Ensure backward compatibility

2. **Phase 2**: Enable KDV on followers first
   - Followers decode KDV logs from leader
   - Leader still sends full logs
   - Verify correctness

3. **Phase 3**: Enable KDV on leaders
   - Leaders send KDV-encoded logs
   - Measure bandwidth savings
   - Monitor latency impact

4. **Phase 4**: Enable KDV for RocksDB persistence
   - Measure disk savings
   - Verify replay correctness

**Backward Compatibility**:
- KDV header version field allows format evolution
- Non-KDV logs can be detected (no valid KDV header)
- Decoder can fall back to treating as raw log if needed

**Monitoring Metrics** (to be added):
```cpp
struct KDVStats {
    std::atomic<uint64_t> total_encodes{0};
    std::atomic<uint64_t> total_decodes{0};
    std::atomic<uint64_t> total_bases_written{0};
    std::atomic<uint64_t> total_deltas_written{0};
    std::atomic<uint64_t> total_original_bytes{0};
    std::atomic<uint64_t> total_encoded_bytes{0};
    std::atomic<uint64_t> total_encode_time_ns{0};
    std::atomic<uint64_t> total_decode_time_ns{0};
    
    double getCompressionRatio() const {
        return total_original_bytes > 0 
            ? (double)total_encoded_bytes / total_original_bytes 
            : 1.0;
    }
    
    double getAvgEncodeTimeUs() const {
        return total_encodes > 0 
            ? (double)total_encode_time_ns / total_encodes / 1000.0 
            : 0.0;
    }
};
```

## Implementation Status

- [x] Phase 0: Background research and baseline establishment
- [x] Phase 1: Current system understanding and documentation
- [ ] Phase 2: KDV design (API, delta format, policies)
- [ ] Phase 3: KDV library implementation and testing
- [ ] Phase 4: RocksDB persistence integration
- [ ] Phase 5: Paxos replication integration
- [ ] Phase 6: Compaction and chain management
- [ ] Phase 7: Evaluation and experiments
- [ ] Phase 8: Final documentation

## References

- Aurora: "Amazon Aurora: Design Considerations for High Throughput Cloud-Native Relational Databases" (SIGMOD 2017)
- Mako: "Mako: Speculative Distributed Transactions with Geo-Replication" (OSDI 2025)
- Current Mako documentation: `doc/introduction.md`, `doc/architecture.md`, `doc/disk_persistence.md`
