# KDV Store Evaluation Methodology

## Overview

This document describes the evaluation methodology for the Key-Delta-Value (KDV) layer implementation in Mako, including workloads, metrics, and expected results.

## Evaluation Goals

1. **Quantify bandwidth savings**: Measure reduction in cross-datacenter network traffic
2. **Quantify disk savings**: Measure reduction in persistent storage footprint
3. **Measure performance overhead**: Assess impact on throughput and latency
4. **Validate correctness**: Ensure KDV encoding/decoding preserves data integrity
5. **Characterize workload sensitivity**: Understand when KDV provides maximum benefit

## Metrics

### Primary Metrics

**Bandwidth Savings**:
- `total_network_bytes_sent` (from `paxos_main_helper.cc`)
- Measured with and without KDV enabled
- Reported as: `(baseline_bytes - kdv_bytes) / baseline_bytes * 100%`

**Disk Savings**:
- `total_original_bytes_` vs `total_encoded_bytes_` (from `RocksDBPersistence`)
- RocksDB directory size: `du -sh /tmp/*_mako_rocksdb_shard*`
- Reported as: `(original_bytes - encoded_bytes) / original_bytes * 100%`

**Throughput**:
- Transactions per second (TPS) from benchmark logs
- Measured with and without KDV enabled
- Reported as: `(kdv_tps - baseline_tps) / baseline_tps * 100%` (overhead)

**Latency**:
- P50, P95, P99 latency from benchmark logs
- Measured with and without KDV enabled
- Reported as: `(kdv_latency - baseline_latency) / baseline_latency * 100%` (overhead)

### Secondary Metrics

**Encode/Decode Performance**:
- Average encode time per log (microseconds)
- Average decode time per log (microseconds)
- Logged every 1000 operations by KDV library

**Chain Statistics**:
- Base vs delta ratio (from KDV encode logs)
- Average chain length before base write
- Delta size distribution

**Replay Performance**:
- Total replay time from `rocksdb_replay_app`
- Measured with and without KDV decoding

## Workloads

### YCSB Variants

**Read-Heavy (95/5)**:
- 95% read operations, 5% write operations
- Expected: Minimal KDV benefit (few writes to compress)
- Purpose: Establish baseline overhead for read-dominated workloads

**Balanced (50/50)**:
- 50% read operations, 50% write operations
- Expected: Moderate KDV benefit
- Purpose: Evaluate typical mixed workload performance

**Write-Heavy (20/80)**:
- 20% read operations, 80% write operations
- Expected: Maximum KDV benefit (many writes to compress)
- Purpose: Demonstrate best-case compression scenario

### Update Size Patterns

**Small Updates (16 bytes changed in 1KB record)**:
- Modify only a small portion of each record
- Expected: 90-95% compression ratio
- Purpose: Demonstrate optimal delta encoding scenario

**Medium Updates (50% of record changed)**:
- Modify approximately half of each record
- Expected: 40-60% compression ratio
- Purpose: Evaluate typical update patterns

**Large Updates (90% of record changed)**:
- Modify most of each record
- Expected: Minimal compression (fallback to base)
- Purpose: Verify policy correctly identifies inefficient deltas

### Record Sizes

Test with varying record sizes:
- **100 bytes**: Small records (typical for metadata)
- **1KB**: Medium records (typical for application data)
- **4KB**: Large records (typical for documents/blobs)

Expected: Larger records provide more opportunity for delta compression

### Replication Scenarios

**Single Shard with Replication**:
```bash
./ci/ci.sh shard1Replication
```
- Purpose: Isolate replication overhead
- Metrics: Network bandwidth, replay time

**Two Shards with Replication**:
```bash
./ci/ci.sh shard2Replication
```
- Purpose: Evaluate multi-shard coordination
- Metrics: Cross-shard transaction latency, network bandwidth

## Experimental Procedure

### Baseline Experiments

1. **Disable KDV**:
   ```bash
   unset MAKO_ENABLE_KDV_LOGS
   ```

2. **Run workload**:
   ```bash
   ./ci/ci.sh shard1Replication
   ```

3. **Collect metrics**:
   - Throughput and latency from benchmark output
   - Network bytes from Paxos logs
   - Disk usage: `du -sh /tmp/*_mako_rocksdb_shard*`

4. **Save results**:
   ```bash
   mkdir -p results/kdv_baseline
   cp benchmark_output.log results/kdv_baseline/
   du -sh /tmp/*_mako_rocksdb_shard* > results/kdv_baseline/disk_usage.txt
   ```

### KDV Experiments

1. **Enable KDV**:
   ```bash
   export MAKO_ENABLE_KDV_LOGS=1
   ```

2. **Run same workload**:
   ```bash
   ./ci/ci.sh shard1Replication
   ```

3. **Collect metrics**:
   - Throughput and latency from benchmark output
   - Network bytes from Paxos logs
   - Disk usage: `du -sh /tmp/*_mako_rocksdb_shard*`
   - KDV statistics from `[KDV Encode]` and `[KDV Decode]` logs
   - Compression ratio from `RocksDBPersistence::printKDVStats()`

4. **Save results**:
   ```bash
   mkdir -p results/kdv_experiments
   cp benchmark_output.log results/kdv_experiments/
   du -sh /tmp/*_mako_rocksdb_shard* > results/kdv_experiments/disk_usage.txt
   ```

### Replay Experiments

1. **Baseline replay** (without KDV):
   ```bash
   ./build/rocksdb_replay_app
   ```

2. **KDV replay** (with decoding):
   ```bash
   ./build/rocksdb_replay_app --enable-kdv-logs
   ```

3. **Compare replay times**:
   - Total time to replay all logs
   - Throughput (transactions replayed per second)

## Expected Results

### Bandwidth Savings

**Small Updates (16 bytes in 1KB)**:
- Expected savings: 90-95%
- Rationale: Delta contains only changed region + metadata

**Medium Updates (50% changed)**:
- Expected savings: 40-60%
- Rationale: Delta contains ~50% of data + metadata

**Large Updates (90% changed)**:
- Expected savings: 0-10%
- Rationale: Policy writes base instead of inefficient delta

### Disk Savings

Similar to bandwidth savings, as same encoding is used for RocksDB persistence.

### Performance Overhead

**Throughput**:
- Expected overhead: 1-5%
- Rationale: Encode/decode adds 1-5 microseconds per log

**Latency**:
- Expected overhead: 2-8%
- Rationale: Encode on critical path, decode on follower path

**Replay Time**:
- Expected overhead: 5-15%
- Rationale: Decode all logs sequentially during replay

### Chain Behavior

**Base vs Delta Ratio**:
- Expected: 1 base per 8 deltas (12.5% bases)
- Rationale: MaxChainLen = 8

**Average Chain Length**:
- Expected: 4-6 deltas
- Rationale: Some chains reset early due to large updates

## Analysis and Visualization

### Compression Ratio vs Update Size

Plot compression ratio (encoded_size / original_size) against percentage of record changed:
- X-axis: Update size (% of record changed)
- Y-axis: Compression ratio
- Expected: Linear relationship with breakpoint at ~70% (MaxDeltaSizeRatio)

### Throughput vs KDV Mode

Bar chart comparing throughput:
- Baseline (KDV disabled)
- KDV enabled
- Expected: <5% difference

### Latency Distribution

Box plot or CDF comparing latency distributions:
- Baseline P50, P95, P99
- KDV P50, P95, P99
- Expected: Slight shift right for KDV (higher latency)

### Replay Time vs Chain Length

Plot replay time against average chain length:
- X-axis: Average chain length
- Y-axis: Total replay time (seconds)
- Expected: Linear relationship (longer chains = more decode operations)

### Network Bandwidth Over Time

Time series plot:
- X-axis: Time (seconds)
- Y-axis: Network bandwidth (MB/s)
- Two lines: Baseline vs KDV
- Expected: KDV line consistently lower

## Validation Checks

### Correctness

1. **Encode/Decode Identity**:
   ```cpp
   assert(kdv_decode_log(kdv_encode_log(data)) == data);
   ```

2. **Replay Consistency**:
   - Compare final database state with baseline
   - Verify all transactions replayed successfully

3. **Replication Consistency**:
   - Compare leader and follower states
   - Verify `replay_batch` counter matches expected value

### Performance Sanity Checks

1. **Throughput degradation < 10%**:
   - If overhead > 10%, investigate encode/decode bottlenecks

2. **Latency increase < 15%**:
   - If overhead > 15%, consider async encoding or batching

3. **Compression ratio > 50% for small updates**:
   - If compression < 50%, verify delta algorithm correctness

## Troubleshooting

### Low Compression Ratio

**Possible causes**:
- Updates are too large (policy writing bases)
- Chain length too short (frequent base writes)
- Workload has low locality (each update changes different regions)

**Solutions**:
- Increase `MaxChainLen` (e.g., 16 instead of 8)
- Increase `MaxDeltaSizeRatio` (e.g., 0.8 instead of 0.7)
- Analyze update patterns to understand workload characteristics

### High Performance Overhead

**Possible causes**:
- Encode/decode on critical path
- Lock contention in `KDVPartitionState`
- Frequent base writes (no delta benefit)

**Solutions**:
- Profile with `perf` to identify bottlenecks
- Consider async encoding (queue logs for background encoding)
- Optimize delta computation algorithm

### Replay Failures

**Possible causes**:
- Missing base for delta decoding
- Sequence number mismatch
- Corrupted encoded data

**Solutions**:
- Check `[KDV]` error logs for specific failure reasons
- Verify sequence numbers are monotonic and contiguous
- Add checksums to KDV header for corruption detection

## Future Evaluation Directions

1. **Geo-distributed experiments**: Measure actual cross-datacenter bandwidth savings
2. **Long-running experiments**: Evaluate disk savings over days/weeks
3. **Failure scenarios**: Test recovery after crashes with KDV-encoded logs
4. **Compaction experiments**: Measure benefit of offline delta chain compaction
5. **Adaptive policies**: Evaluate dynamic tuning of MaxChainLen and MaxDeltaSizeRatio

## Summary

This evaluation methodology provides a systematic approach to measuring the benefits and costs of the KDV layer. Key takeaways:

- **Focus on small update workloads** for maximum benefit
- **Monitor both bandwidth and disk savings** as primary metrics
- **Validate correctness** before optimizing performance
- **Characterize overhead** to ensure acceptable performance
- **Iterate on policies** based on workload characteristics
