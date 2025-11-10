# Delta-Based Replication Evaluation Plan

## Overview

This document provides a comprehensive plan for evaluating the delta-based replication implementation in Mako. The evaluation measures bandwidth savings, latency impact, and correctness of the delta replication system.

## Current Implementation Status

**Completed:**
- ✅ Phase 1: Core Infrastructure (delta computation, serialization, CRC32 verification)
- ✅ Phase 2: Write Path Integration (sender-side delta computation, receiver-side delta application)
- ✅ Phase 6: Evaluation Instrumentation (delta statistics printing in benchmark output)

**Architecture:**
- Deltas are computed on sender side during transaction commit
- Deltas are transmitted over the network (saves bandwidth)
- Deltas are applied on receiver side to reconstruct full values
- Full values are stored in local MassTrees (no storage savings, only bandwidth savings)

## Evaluation Objectives

1. **Bandwidth Savings**: Measure reduction in bytes transmitted over the network
2. **Latency Impact**: Measure impact on transaction commit latency (p50/p99)
3. **Throughput Impact**: Measure impact on transactions per second
4. **Correctness**: Verify no data corruption or checksum errors
5. **Delta Effectiveness**: Measure percentage of updates that benefit from delta encoding

## Evaluation Metrics

### Primary Metrics (from g_delta_stats)

```cpp
struct DeltaStats {
    std::atomic<uint64_t> bytes_sent_full;      // Bytes sent as full values
    std::atomic<uint64_t> bytes_sent_delta;     // Bytes sent as deltas
    std::atomic<uint64_t> deltas_applied;       // Number of deltas applied on receiver
    std::atomic<uint64_t> deltas_computed;      // Number of deltas computed on sender
    std::atomic<uint64_t> checksum_errors;      // Number of checksum verification failures
};
```

**Bandwidth Savings Calculation:**
```
bandwidth_savings = (bytes_sent_full - bytes_sent_delta) / (bytes_sent_full + bytes_sent_delta) * 100%
```

### Secondary Metrics (from benchmark output)

- `agg_persist_throughput`: Transactions per second
- `avg_persist_latency`: Average commit latency (ms)
- `NewOrder_remote_commit_latency`: Remote transaction latency (ms)
- `NewOrder_remote_abort_ratio`: Abort rate (%)

## Test Scenarios

### Scenario 1: Baseline (Delta Replication Disabled)

**Purpose:** Establish baseline performance without delta replication

**Configuration:**
```bash
# Do NOT set MAKO_ENABLE_DELTA_REPLICATION
export MAKO_ENABLE_DELTA_REPLICATION=0  # or leave unset
```

**Run Command:**
```bash
cd /home/ubuntu/repos/mako-project
./examples/test_1shard_replication.sh 6
```

**Expected Output:**
- `agg_persist_throughput`: ~X ops/sec (baseline)
- `avg_persist_latency`: ~Y ms (baseline)
- Delta statistics should show all zeros (disabled)

**Collect Metrics:**
- Throughput (ops/sec)
- Latency (ms)
- Abort ratio (%)

---

### Scenario 2: Delta Replication Enabled

**Purpose:** Measure bandwidth savings and performance impact with delta replication

**Configuration:**
```bash
export MAKO_ENABLE_DELTA_REPLICATION=1
```

**Run Command:**
```bash
cd /home/ubuntu/repos/mako-project
MAKO_ENABLE_DELTA_REPLICATION=1 ./examples/test_1shard_replication.sh 6
```

**Expected Output:**
```
--- delta replication statistics ---
bytes_sent_full: XXXXX bytes
bytes_sent_delta: YYYYY bytes
deltas_applied: ZZZZZ
deltas_computed: ZZZZZ
checksum_errors: 0
bandwidth_savings: XX.XX%
```

**Collect Metrics:**
- Bandwidth savings (%)
- Throughput (ops/sec) - compare with baseline
- Latency (ms) - compare with baseline
- Abort ratio (%) - should be similar to baseline
- Checksum errors - should be 0

---

### Scenario 3: Multi-Shard Evaluation

**Purpose:** Measure delta effectiveness with cross-shard transactions

**Configuration:**
```bash
export MAKO_ENABLE_DELTA_REPLICATION=1
```

**Run Command:**
```bash
cd /home/ubuntu/repos/mako-project
MAKO_ENABLE_DELTA_REPLICATION=1 ./examples/test_2shard_replication.sh 6
```

**Expected Output:**
- Higher bandwidth savings due to more remote transactions
- Similar latency impact as single-shard

**Collect Metrics:**
- Bandwidth savings (%) - should be higher than single-shard
- Cross-shard transaction latency
- Remote transaction ratio

---

## Evaluation Procedure

### Step 1: Build with Evaluation Instrumentation

```bash
cd /home/ubuntu/repos/mako-project
export PATH="$HOME/.cargo/bin:$PATH"
make -j4
```

**Verify:** Build completes successfully

---

### Step 2: Run Baseline Test (Delta Disabled)

```bash
cd /home/ubuntu/repos/mako-project
./examples/test_1shard_replication.sh 6 > baseline_results.log 2>&1
```

**Extract Metrics:**
```bash
grep "agg_persist_throughput" baseline_results.log
grep "avg_persist_latency" baseline_results.log
grep "NewOrder_remote_commit_latency" baseline_results.log
grep "NewOrder_remote_abort_ratio" baseline_results.log
```

**Save Results:**
```
Baseline Throughput: ____ ops/sec
Baseline Latency: ____ ms
Baseline Abort Ratio: ____ %
```

---

### Step 3: Run Delta-Enabled Test

```bash
cd /home/ubuntu/repos/mako-project
MAKO_ENABLE_DELTA_REPLICATION=1 ./examples/test_1shard_replication.sh 6 > delta_results.log 2>&1
```

**Extract Metrics:**
```bash
grep "bytes_sent_full" delta_results.log
grep "bytes_sent_delta" delta_results.log
grep "deltas_applied" delta_results.log
grep "bandwidth_savings" delta_results.log
grep "checksum_errors" delta_results.log
grep "agg_persist_throughput" delta_results.log
grep "avg_persist_latency" delta_results.log
```

**Save Results:**
```
Delta Throughput: ____ ops/sec
Delta Latency: ____ ms
Bandwidth Savings: ____ %
Deltas Applied: ____
Checksum Errors: ____ (should be 0)
```

---

### Step 4: Calculate Performance Impact

```
Throughput Impact = (Delta Throughput - Baseline Throughput) / Baseline Throughput * 100%
Latency Impact = (Delta Latency - Baseline Latency) / Baseline Latency * 100%
```

**Expected Results:**
- Bandwidth Savings: 50-70% (for typical TPC-C workloads)
- Throughput Impact: -5% to +5% (minimal impact)
- Latency Impact: +5% to +15% (due to delta computation and receiver-side shard_get)
- Checksum Errors: 0 (correctness verification)

---

### Step 5: Verify Correctness

**Check for Errors:**
```bash
grep -i "error\|checksum\|corruption" delta_results.log
grep "checksum_errors: 0" delta_results.log
```

**Expected:** No errors, checksum_errors should be 0

**If checksum_errors > 0:**
- This indicates delta application failures
- Review delta computation logic in Transaction.cc
- Review delta application logic in server.cc
- Check for race conditions or version mismatches

---

## Results Template

### Evaluation Results Summary

**Test Environment:**
- Date: ____
- Mako Version: devin/1762808529-delta-replication
- Test Duration: 60 seconds
- Number of Threads: 6
- Workload: TPC-C (1-shard replication)

**Baseline (Delta Disabled):**
```
Throughput: ____ ops/sec
Latency: ____ ms
Abort Ratio: ____ %
```

**Delta-Enabled:**
```
Throughput: ____ ops/sec (___% change)
Latency: ____ ms (___% change)
Abort Ratio: ____ %
Bandwidth Savings: ____%
Bytes Sent Full: ____ bytes
Bytes Sent Delta: ____ bytes
Deltas Applied: ____
Checksum Errors: 0
```

**Analysis:**
- Bandwidth savings achieved: ____%
- Throughput impact: ____%
- Latency impact: ____%
- Correctness verified: YES/NO

---

## Troubleshooting

### Issue: Delta statistics show all zeros

**Cause:** Delta replication not enabled

**Solution:**
```bash
export MAKO_ENABLE_DELTA_REPLICATION=1
# OR
MAKO_ENABLE_DELTA_REPLICATION=1 ./examples/test_1shard_replication.sh 6
```

---

### Issue: Checksum errors > 0

**Cause:** Delta application failures or data corruption

**Solution:**
1. Check logs for detailed error messages
2. Verify old value retrieval logic in Transaction.cc
3. Verify delta application logic in server.cc
4. Check for race conditions in concurrent updates

---

### Issue: No bandwidth savings (bytes_sent_delta ≈ bytes_sent_full)

**Cause:** Deltas are larger than full values (not beneficial)

**Possible Reasons:**
- Small value sizes (delta overhead dominates)
- Large update sizes (most of value changed)
- Insert operations (no old value to delta against)

**Solution:** This is expected behavior - deltas are only used when beneficial

---

### Issue: High latency impact (>20%)

**Cause:** Receiver-side shard_get() overhead

**Solution:**
- This is a known limitation of current design
- Consider caching old values on receiver side
- Consider async delta application

---

## Next Steps After Evaluation

1. **Document Results:** Create evaluation report with graphs and analysis
2. **Update PR:** Add evaluation results to PR description
3. **Optimize:** If latency impact is too high, consider optimizations:
   - Cache old values on receiver side
   - Async delta application
   - Batch delta computation
4. **Future Work:** Consider implementing:
   - Phase 3: Read Path Integration (if storage savings needed)
   - Phase 4: RocksDB Integration (persist deltas to disk)
   - Phase 5: Background Compaction (collapse delta chains)

---

## Evaluation Checklist

- [ ] Build with evaluation instrumentation
- [ ] Run baseline test (delta disabled)
- [ ] Extract baseline metrics
- [ ] Run delta-enabled test
- [ ] Extract delta metrics
- [ ] Calculate bandwidth savings
- [ ] Calculate performance impact
- [ ] Verify correctness (checksum_errors = 0)
- [ ] Document results
- [ ] Update PR with evaluation results
- [ ] Commit evaluation results to repository

---

## Conclusion

This evaluation plan provides a systematic approach to measuring the effectiveness of delta-based replication in Mako. The primary goal is to demonstrate significant bandwidth savings (50-70%) with minimal performance impact (<10% latency increase) and perfect correctness (0 checksum errors).

The evaluation results will inform future optimization decisions and validate the delta replication design for geo-distributed Mako deployments.
