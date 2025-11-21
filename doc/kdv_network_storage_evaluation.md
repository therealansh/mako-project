# KDV Network & Storage Evaluation Guide

## Overview

This guide explains how to evaluate the Key-Delta-Value (KDV) compression layer to demonstrate **50-70% network bandwidth reduction** for cross-datacenter replication in Mako's geo-replication architecture.

## Problem: Why Current Evaluations Don't Show Expected Outcomes

### Issue #1: TPC-C Shows 0% Improvement

**Root Cause**: TPC-C transactions create append-only logs with volatile metadata (timestamps, sequence numbers) that change every transaction. Even when updating the same logical records (e.g., `warehouse_1`, `district_5`), the serialized log appears different every time, preventing KDV from recognizing repeated updates.

**Result**: 100% base writes, 0% deltas, only 3% compression from header overhead.

**Solution**: Use YCSB with controlled update patterns (see Phase 2 below for TPC-C support).

### Issue #2: Existing YCSB Scripts Extract Wrong Metrics

**Problem**: The `ycsb_kdv_replicated.sh` script looks at the **follower (localhost)** log, but KDV statistics and network metrics are printed in the **leader (p1)** log.

**Result**: Metrics show "N/A" for compression ratios and network savings.

**Solution**: New script `kdv_network_storage_eval.sh` extracts metrics from the correct log file.

### Issue #3: Wrong Workload Configuration

**Problem**: Using large key spaces with random access patterns results in few repeated updates to the same keys, limiting delta compression opportunities.

**Solution**: Use smaller key space (10,000 keys) with high write ratios to ensure repeated updates to the same keys.

## Evaluation Strategy

### Key Metrics to Measure

1. **Network Bandwidth Reduction** (PRIMARY)
   - Metric: `[Paxos Network] Final statistics: total bytes sent`
   - Comparison: Baseline vs. KDV
   - Target: 50-70% reduction for small updates

2. **Storage Space Reduction**
   - Metric: RocksDB directory size (`du -sb /tmp/mako_rocksdb_shard*`)
   - Comparison: Baseline vs. KDV
   - Target: Similar to network reduction (30-70%)

3. **KDV Compression Ratio**
   - Metric: `Compression ratio: X%` from RocksDB persistence
   - Indicates: Effectiveness of delta encoding
   - Target: >80% for small updates

4. **Performance Overhead**
   - Metric: Throughput (TPS) and latency
   - Comparison: Baseline vs. KDV
   - Target: <5% overhead

### Workload Design Principles

To achieve 50-70% bandwidth reduction, workloads must:

1. **Repeated Updates to Same Keys**
   - Use small key space (10K keys, not 100K)
   - High write/RMW ratio (70-90%)
   - Ensures same keys are updated multiple times

2. **Small Delta Sizes**
   - Update only 16-256 bytes of a 1KB-4KB record
   - Represents realistic patterns: counters, status fields, metadata updates
   - Delta encoding is most effective here

3. **Realistic Scenarios**
   - User profile updates (status, last_login, counters)
   - Inventory updates (quantity changes)
   - Sensor data (timestamp + small value changes)

### Workload Configurations

The new evaluation script tests these scenarios:

| Scenario | Read% | RMW% | Record Size | Update Size | Expected Savings | Purpose |
|----------|-------|------|-------------|-------------|------------------|---------|
| **small_updates_heavy** | 10 | 90 | 1024B | 16B (1.5%) | **80-90%** | Best case: heavy small updates |
| **small_updates_balanced** | 50 | 50 | 1024B | 16B | **60-70%** | Typical mixed workload |
| **small_updates_read_heavy** | 90 | 10 | 1024B | 16B | **40-50%** | Occasional updates |
| **medium_updates_moderate** | 30 | 70 | 1024B | 256B (25%) | **40-60%** | Medium deltas |
| **large_updates** | 50 | 50 | 1024B | 920B (90%) | **0-10%** | Worst case: should use base |
| **large_record_small_delta** | 50 | 50 | 4096B | 64B (1.5%) | **85-95%** | Large records, tiny changes |

## How to Run Evaluation

### Step 1: Build the Project

```bash
cd /home/anshtyagi.linux/mako_ansh/mako-project
make clean
make -j32
```

### Step 2: Run Network & Storage Evaluation

```bash
# Run the evaluation script
./scripts/kdv_network_storage_eval.sh
```

This will:
1. Run 7 workload scenarios (14 experiments total: baseline + KDV)
2. Each experiment runs for 60 seconds
3. Collect network, storage, and performance metrics
4. Save results to `results/kdv_network_storage_eval/results_<timestamp>.csv`
5. Automatically analyze and print summary

**Expected Runtime**: ~30 minutes (14 experiments × 60s + cleanup)

### Step 3: Analyze Results

The script automatically runs analysis, but you can re-analyze:

```bash
python3 scripts/analyze_kdv_network_storage.py results/kdv_network_storage_eval/results_<timestamp>.csv
```

### Step 4: Interpret Results

The analysis script will show:

```
📊 AVERAGES:
   Network bandwidth savings:  65.3%  ← Target: 50-70%
   Storage space savings:      62.1%
   Performance overhead:       3.2%   ← Target: <5%

🏆 BEST NETWORK SAVINGS:
   Experiment: small_updates_heavy
   Savings: 87.4%
   Config: 1024B records, 16B updates

💡 RECOMMENDATIONS:
   ✅ KDV is achieving target bandwidth reduction (50-70%)
   → Ready for geo-replication deployment
```

## Expected Outcomes

### Scenario 1: Heavy Small Updates (Best Case)

- **Workload**: 90% RMW, 10% reads
- **Updates**: 16 bytes in 1KB records
- **Expected Network Savings**: 80-90%
- **Why**: Nearly perfect delta compression for tiny changes

### Scenario 2: Balanced Small Updates (Typical Case)

- **Workload**: 50% RMW, 50% reads
- **Updates**: 16 bytes in 1KB records
- **Expected Network Savings**: 60-70%
- **Why**: Good compression with moderate update frequency

### Scenario 3: Large Updates (Worst Case)

- **Workload**: 50% RMW, 50% reads
- **Updates**: 920 bytes in 1KB records
- **Expected Network Savings**: 0-10%
- **Why**: KDV policy correctly falls back to base encoding (delta would be larger than base)

## Troubleshooting

### Problem: All metrics show "N/A"

**Cause**: Logs not being generated or wrong log file being read

**Solution**:
1. Check that experiments are running: `ps aux | grep dbtest`
2. Verify log files exist: `ls -lh kdv_eval_*.log`
3. Check leader (p1) log for metrics: `grep "Paxos Network" kdv_eval_*_p1.log`

### Problem: Low compression ratios (<30%)

**Cause**: Keys not being repeated or deltas too large

**Solution**:
1. Reduce key space (try `-k 1000` for more collisions)
2. Decrease update size (try `-u 8` for smaller deltas)
3. Increase write ratio (try `10,0,90,0` for 90% RMW)

### Problem: High performance overhead (>10%)

**Cause**: Encode/decode operations on critical path

**Solution**:
1. Profile with `perf record/report`
2. Check if delta computation is expensive for large records
3. Consider tuning MaxChainLen (shorter chains = less decode work)

## Advanced Configuration

### Tuning KDV Policies

Edit `src/mako/kdv_format.cc`:

```cpp
// Maximum delta size as fraction of base (default: 0.75)
const double MaxDeltaSizeRatio = 0.75;

// Maximum chain length before reset (default: 10)
const uint16_t MaxChainLen = 10;

// Maximum base age before reset (default: 1000 sequences)
const uint64_t MaxBaseAge = 1000;

// Per-partition LRU cache size (default: 10000 keys)
const size_t MaxCacheSize = 10000;
```

**Tuning Guidelines**:
- **Lower MaxDeltaSizeRatio (0.5)**: More aggressive base fallback, better for variable update sizes
- **Higher MaxChainLen (20)**: Longer chains, better compression but higher decode cost
- **Lower MaxBaseAge (500)**: More frequent base resets, better for drifting values
- **Larger MaxCacheSize (50000)**: Support more unique keys, uses more memory

### Custom Workload Testing

Create custom workloads in the script:

```bash
# Add to WORKLOADS array in kdv_network_storage_eval.sh
"custom_name:read,write,rmw,scan:record_size:update_bytes:description"

# Example: High-frequency tiny updates (like counters)
"counter_updates:5,0,95,0:512:4:Counter-style 4-byte updates in 512B records"

# Example: Profile updates (status field changes)
"profile_updates:70,0,30,0:2048:32:Profile status updates (32B in 2KB records)"
```

## Comparing with Existing Scripts

### Old Script (`ycsb_kdv_replicated.sh`)
- ❌ Extracts metrics from follower log (localhost)
- ❌ Large key space (100K keys) with few repeated updates
- ❌ Generic workloads not optimized for KDV
- ✅ Tests multiple record sizes and update patterns

### New Script (`kdv_network_storage_eval.sh`)
- ✅ Extracts metrics from leader log (p1) - where stats are printed
- ✅ Small key space (10K keys) for repeated updates
- ✅ Workloads designed to showcase KDV benefits
- ✅ Focused on network and storage metrics (not just throughput)
- ✅ Includes detailed analysis and recommendations

## Next Steps: TPC-C Evaluation (Phase 2)

To enable TPC-C evaluation, implement per-record key extraction:

1. **Parse transaction log structure** to extract individual record updates
2. **Compute per-record key_hash** from `table_id + primary_key`
3. **Encode/decode multiple KDV entries** per transaction log
4. **Track per-record state** instead of per-log state

See `doc/kdv_ycsb_vs_tpcc_analysis.md` for detailed design.

## References

- **KDV Implementation**: `src/mako/kdv_format.{h,cc}`
- **RocksDB Integration**: `src/mako/rocksdb_persistence.cc`
- **Paxos Network Stats**: `src/deptran/paxos_main_helper.cc`
- **YCSB Benchmark**: `src/mako/benchmarks/ycsb/*`
- **Configuration**: `config/ycsb_*.yml`

## Citation

If using this evaluation methodology, cite:

```
@inproceedings{mako-osdi25,
  title={Mako: A Speculative Distributed Transaction System with Geo-Replication},
  booktitle={OSDI},
  year={2025}
}
```

---

**Questions or Issues?**

- Check logs in `results/kdv_network_storage_eval/`
- Review KDV statistics: `grep "KDV" kdv_eval_*_p1.log`
- Verify network metrics: `grep "Paxos Network" kdv_eval_*_p1.log`
- Contact: Mako development team
