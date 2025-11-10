# Survey: Delta-Based Storage Techniques

## Executive Summary

This document surveys delta-based storage techniques used in modern distributed databases, focusing on Amazon Aurora, differential update mechanisms, and delta compression approaches. The goal is to inform the design of a Key-Delta-Value (KDV) store for Mako that reduces write bandwidth while maintaining read performance.

## 1. Amazon Aurora's Storage Architecture

### Overview
Amazon Aurora is a cloud-native relational database that decouples compute from storage, using a log-structured storage system that only transmits redo log records (deltas) to storage nodes rather than full data pages.

### Key Design Principles

**Log-Structured Storage**: Aurora's storage layer is fundamentally log-structured. Instead of writing full 16KB database pages on every update, Aurora only writes redo log records (typically 100-1000 bytes) to the storage layer. The storage nodes are responsible for materializing pages from the log.

**Write Amplification Reduction**: Traditional databases write full pages even for small updates, causing significant write amplification. Aurora reduces network traffic by 10-30x by transmitting only log records. For example, updating a single 8-byte field in a 16KB page requires transmitting only ~100 bytes instead of 16KB.

**Asynchronous Page Materialization**: Storage nodes materialize pages from redo logs asynchronously in the background. This decouples the critical path of transaction commit from page writes, reducing commit latency. Pages are only materialized when:
- A read request arrives for that page
- Background compaction is triggered
- Storage space needs to be reclaimed

**Quorum-Based Replication**: Aurora replicates redo logs across 6 storage nodes in 3 availability zones using a 4/6 write quorum and 3/6 read quorum. This ensures durability without waiting for all replicas, reducing commit latency.

**Log Sequence Numbers (LSN)**: Each redo log record has a monotonically increasing LSN. Storage nodes track the highest LSN they've received and applied. The database only acknowledges a transaction as committed once a write quorum of storage nodes have persisted the log records up to that transaction's commit LSN.

### Relevance to Mako

Aurora's approach is highly relevant for Mako's geo-replicated architecture:
- **Bandwidth Reduction**: Transmitting deltas instead of full values can reduce cross-datacenter bandwidth by 10-30x
- **Decoupled Commit**: Transaction commit can complete once deltas are replicated, without waiting for full value materialization
- **Asynchronous Compaction**: Background threads can collapse delta chains without blocking transaction processing

**Key Difference**: Aurora's storage nodes are stateful and handle page materialization. In Mako, we need to decide whether delta reconstruction happens on the sender, receiver, or both.

## 2. Differential Update Mechanisms

### Copy-on-Write (CoW) with Deltas

Many modern storage systems use copy-on-write with delta encoding:

**B-Tree Delta Encoding**: Systems like LMDB and WiredTiger use delta encoding within B-tree nodes. When a node is updated, instead of copying the entire node, they store:
- Base version of the node
- Delta records describing changes (key insertions, deletions, updates)
- Pointer to previous version for MVCC

**Benefits**:
- Reduced write amplification for small updates
- Efficient MVCC implementation (old versions remain accessible)
- Better cache utilization (multiple versions share base data)

**Challenges**:
- Read amplification (must apply deltas on read)
- Compaction required to prevent unbounded delta chains
- Complexity in handling concurrent updates

### Operation-Based Deltas

Some systems store operation logs instead of byte-level diffs:

**Redis AOF (Append-Only File)**: Redis can persist operations (SET, INCR, LPUSH) instead of full snapshots. On recovery, operations are replayed to reconstruct state.

**Benefits**:
- Very compact for certain operations (INCR is ~10 bytes vs potentially KB of data)
- Natural fit for command-based systems
- Easy to implement partial replication

**Challenges**:
- Not applicable to all data types
- Replay can be slow for long operation logs
- Requires semantic understanding of operations

**Relevance to Mako**: For TPC-C workloads, many updates are incremental (e.g., incrementing counters, appending to lists). Operation-based deltas could be highly effective for these patterns.

### Structural Sharing

Functional data structures use structural sharing to minimize copying:

**Persistent Data Structures**: Clojure, Haskell, and other functional languages use persistent data structures where updates create new versions that share structure with old versions.

**Example - Persistent Vector**: Updating element at index i in a 32-way tree only requires copying log32(n) nodes, sharing the rest.

**Benefits**:
- Automatic versioning (MVCC for free)
- Lock-free reads (old versions never modified)
- Memory efficient for small updates

**Challenges**:
- Requires specific data structure design
- Indirection overhead (pointer chasing)
- Not applicable to arbitrary byte arrays

**Relevance to Mako**: Limited applicability since Mako uses Masstree (not persistent data structure), but the principle of sharing unchanged data is valuable.

## 3. Delta Compression Techniques

### Binary Diff Algorithms

**xdelta / bsdiff**: Industry-standard binary diff algorithms that find common subsequences between old and new versions.

**Algorithm Overview**:
1. Build suffix array of old version
2. For each byte in new version, find longest match in old version
3. Encode as: COPY(offset, length) or INSERT(data)
4. Compress output with LZMA or similar

**Performance**:
- Compression ratio: 10-50x for similar versions
- Computation cost: O(n log n) time, O(n) space
- Decompression: O(n) time, very fast

**Challenges**:
- High CPU cost for compression (not suitable for hot path)
- Requires old version for decompression
- Not effective for random updates

**Relevance to Mako**: Too expensive for real-time transaction processing, but could be used in background compaction.

### Run-Length Encoding (RLE)

**Concept**: Encode runs of identical bytes as (value, count) pairs.

**Example**: "AAAABBBBCCCC" → "A4B4C4"

**Benefits**:
- Very fast encoding/decoding
- Effective for sparse updates (many unchanged bytes)
- Low CPU overhead

**Challenges**:
- Poor compression for random data
- Expansion possible if no runs exist

**Relevance to Mako**: Good fit for TPC-C where updates often modify small fields in large records (e.g., updating balance in 1KB customer record).

### Column-Level Deltas

**Columnar Storage Systems**: Systems like Parquet, ORC, and ClickHouse store data in columns and use delta encoding within columns.

**Delta Encoding for Integers**: Store first value, then deltas:
- Values: [100, 102, 105, 103]
- Encoded: [100, +2, +3, -2]
- Further compress with variable-length encoding

**Benefits**:
- Excellent compression for sorted/monotonic data
- Fast random access (can skip to any position)
- SIMD-friendly decompression

**Challenges**:
- Requires schema knowledge
- Not applicable to arbitrary binary data
- Decompression required for every access

**Relevance to Mako**: Limited applicability since Mako is a key-value store without schema, but could be useful if we add schema-aware optimizations.

### Hybrid Approaches

**Tiered Storage**: Many systems use different strategies for different tiers:
- **Hot tier**: Full values for fast access
- **Warm tier**: Delta-encoded with short chains
- **Cold tier**: Heavily compressed with long chains or full snapshots

**Adaptive Compression**: Select compression strategy based on data characteristics:
- Small updates → byte-level diff
- Large updates → full value
- Incremental updates → operation log
- Random updates → no compression

**Relevance to Mako**: This is the recommended approach. Use simple byte-level diffs for most cases, with adaptive selection based on update size and pattern.

## 4. Design Recommendations for Mako KDV Store

Based on the survey, here are recommendations for Mako's delta-based storage:

### Delta Format

**Primary Format: Simple Byte-Level Diff**
- Store changed regions as (offset, length, data) tuples
- Threshold: Only use deltas if they save >30% bandwidth
- Minimum value size: 256 bytes (avoid overhead for small values)

**Rationale**: Simple to implement, low CPU overhead, effective for TPC-C workloads where updates modify small fields in large records.

**Future Enhancement: Operation Log**
- For specific operations (increment, append), store operation instead of diff
- Requires application-level integration
- Can achieve 10-100x compression for certain patterns

### Read Path Design

**Lazy Reconstruction**: Reconstruct values from deltas on-demand during reads.

**Caching Strategy**:
- Cache reconstructed values in memory
- Use LRU eviction policy
- Cache hit rate should be >90% for hot keys

**Read Amplification Mitigation**:
- Limit delta chain length to 5-10 deltas
- Trigger compaction when chain length exceeds threshold
- Store full value periodically (every Nth update)

### Compaction Policy

**Trigger Conditions** (any of):
- Delta chain length > 5
- Total delta bytes > 4KB
- Chain age > 60 seconds
- Read latency > 2x baseline

**Compaction Strategy**:
- Background thread per partition
- Collapse deltas into base value
- Write new base value atomically
- Update metadata and invalidate old deltas

**Concurrency Control**:
- Use versioning to detect concurrent updates
- Abort compaction if value was updated during compaction
- Retry with new version

### RocksDB Integration

**Schema Design**:
- **Key**: Original key
- **Value**: Serialized DeltaRecord or full value
- **Metadata CF**: Store DeltaMetadata for each key

**Write Path**:
- Compute delta in transaction commit path
- Write delta to RocksDB asynchronously
- Use ordered callbacks to ensure delta application order

**Recovery Path**:
- Read base value and delta chain from RocksDB
- Reconstruct full value
- Rebuild in-memory index

**Compaction Integration**:
- Use RocksDB compaction callbacks to trigger delta compaction
- Merge deltas during RocksDB compaction
- Avoid double compaction (RocksDB + delta)

### Performance Targets

Based on Aurora and other systems:

**Bandwidth Reduction**: 50-70% for TPC-C workloads
- Small field updates (10% of value): 90% reduction
- Medium updates (50% of value): 50% reduction
- Large updates (90% of value): 10% reduction (use full value)

**Latency Impact**: <10% increase in p99 commit latency
- Delta computation: <100μs
- Receiver-side reconstruction: <200μs
- Amortized over network latency (1-10ms cross-datacenter)

**Throughput Impact**: <5% reduction in transactions/second
- Extra CPU for delta computation
- Extra memory for delta chains
- Offset by reduced network bandwidth

## 5. Related Work

### Academic Papers

**"Amazon Aurora: Design Considerations for High Throughput Cloud-Native Relational Databases" (SIGMOD 2017)**
- Describes log-structured storage and quorum-based replication
- Key insight: Decouple transaction commit from page writes
- Achieves 35x write reduction compared to traditional databases

**"WiscKey: Separating Keys from Values in SSD-conscious Storage" (FAST 2016)**
- Separates keys and values in LSM-trees
- Stores only value pointers in LSM-tree, values in log
- Reduces write amplification by 10x

**"Differential RAID: Rethinking RAID for SSD Reliability" (EuroSys 2015)**
- Uses delta encoding for RAID parity updates
- Reduces parity update cost by storing deltas
- Achieves 3-5x write reduction

### Industrial Systems

**Cassandra Incremental Repair**: Uses Merkle trees to identify differences between replicas, then transmits only deltas.

**MongoDB Oplog**: Stores operation log for replication, similar to operation-based deltas.

**CockroachDB Raft Log**: Replicates Raft log entries (deltas) instead of full values, materializes on followers.

## 6. Conclusion

Delta-based storage is a proven technique for reducing write bandwidth in distributed databases. The key trade-offs are:

**Benefits**:
- 50-70% bandwidth reduction for typical workloads
- Lower cross-datacenter costs
- Reduced network congestion

**Costs**:
- Read amplification (must reconstruct from deltas)
- Compaction overhead (background CPU/IO)
- Implementation complexity

For Mako, the recommended approach is:
1. Start with simple byte-level diffs (SIMPLE_DIFF)
2. Implement lazy reconstruction on read path
3. Add background compaction to limit chain length
4. Integrate with RocksDB for persistence
5. Evaluate with TPC-C and YCSB benchmarks

The existing Phase 1 and Phase 2 implementation provides a solid foundation. The remaining work (Phases 3-6) will complete the system and validate the performance benefits.

## References

1. Verbitski et al., "Amazon Aurora: Design Considerations for High Throughput Cloud-Native Relational Databases", SIGMOD 2017
2. Lu et al., "WiscKey: Separating Keys from Values in SSD-conscious Storage", FAST 2016
3. Kadekodi et al., "Differential RAID: Rethinking RAID for SSD Reliability", EuroSys 2015
4. xdelta: http://xdelta.org/
5. bsdiff: http://www.daemonology.net/bsdiff/
6. RocksDB Wiki: https://github.com/facebook/rocksdb/wiki
