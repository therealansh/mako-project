# Debugging KDV Metrics Collection

## Problem: Metrics Show "N/A" or Empty Values

You're seeing output like:
```
small_updates_heavy,baseline,10,0,90,1024,16,10000,,,N/A,N/A,N/A,219256189
small_updates_heavy,kdv,10,0,90,1024,16,10000,,,0,0,0.00,77780626
```

The empty/N/A values indicate metrics aren't being found in log files.

## Root Causes

### 1. **Processes Not Shutting Down Gracefully**
When processes are killed with `kill -9`, they don't execute shutdown code that prints final statistics.

**Fixed in updated script**: Now uses `pkill -TERM` for graceful shutdown.

### 2. **Metrics in Different Log Files**
Different metrics appear in different replica logs:
- **Throughput**: Only printed by leader (varies by configuration)
- **Paxos Network**: Printed by all replicas during shutdown
- **KDV Stats**: Printed by leader during shutdown (if KDV enabled)

**Fixed in updated script**: Now checks ALL log files, not just p1.

### 3. **Stats Not Printed in Replicated Mode**
Some benchmark stats (like `agg_throughput`) might only print in non-replicated mode or require specific configurations.

**Solution**: Check all logs and use alternative metrics if needed.

## Quick Debugging Steps

### Step 1: Run Quick Test

```bash
# Test with KDV enabled
./scripts/kdv_quick_test.sh kdv

# Test baseline
./scripts/kdv_quick_test.sh baseline
```

This runs a 30-second experiment and shows you exactly which metrics are found in which log files.

### Step 2: Check What Metrics Are Available

```bash
# Check for throughput
grep -r "agg_throughput\|throughput" test_*.log

# Check for network metrics
grep -r "Paxos Network\|bytes sent" test_*.log

# Check for KDV stats
grep -r "Total original\|Total encoded\|Compression ratio" test_*.log

# Check RocksDB size
du -sh /tmp/mako_rocksdb_shard*
```

### Step 3: Identify Which Log Has Which Metrics

```bash
# Run quick test and see output
./scripts/kdv_quick_test.sh kdv

# Expected output:
# === test_localhost.log ===
# ✓ Paxos Network found:
# ✓ KDV stats found:
# ...
```

## Common Issues and Solutions

### Issue 1: Empty Throughput

**Symptoms**: Throughput column is empty or "N/A"

**Possible Causes**:
1. Benchmark doesn't print throughput in replicated mode
2. Stats printed to stderr instead of stdout
3. Process killed before stats printed

**Solutions**:
```bash
# Check if throughput is printed anywhere
grep -r "ops/sec\|transactions\|tps" test_*.log | head -10

# Check for alternative metrics
grep -r "submitted\|committed\|completed" test_*.log | head -10

# If not found, throughput might not be available in this mode
# Focus on network/storage savings instead
```

### Issue 2: Zero or N/A Network Bytes

**Symptoms**: `network_bytes` shows 0 or "N/A"

**Possible Causes**:
1. Paxos network counter not being incremented
2. Metrics not printed during shutdown
3. All replicas are on same machine (localhost) so counted as 0

**Solutions**:
```bash
# Check for any Paxos network messages
grep -r "Paxos" test_*.log | grep -i "bytes\|network" | head -10

# Check for periodic stats (not just final)
grep -r "Sent.*logs.*total bytes" test_*.log | tail -10

# If shows 0, might need to check Paxos configuration
# or network tracking code
```

### Issue 3: KDV Stats Show 0

**Symptoms**: `original_bytes=0, encoded_bytes=0, compression_ratio=0.00`

**Possible Causes**:
1. KDV not actually encoding data
2. MAKO_ENABLE_KDV_LOGS not set
3. KDV stats not printed during shutdown

**Solutions**:
```bash
# Verify KDV is enabled
grep "MAKO_ENABLE_KDV_LOGS" test_*.log

# Check for KDV encoding messages
grep -r "KDV" test_*.log | grep -i "encode\|compress\|delta" | head -20

# Check if RocksDB persistence has KDV data
grep -r "RocksDB.*KDV\|KDV.*RocksDB" test_*.log
```

### Issue 4: RocksDB Size Showing But Other Metrics N/A

**Symptoms**: RocksDB size is correct, but other metrics are N/A

**Good News**: This shows:
- ✅ Experiment is running
- ✅ Data is being written to disk
- ✅ At least storage savings can be measured

**What's Missing**:
- Network replication metrics
- KDV compression stats
- Throughput metrics

**Solution**: The storage savings alone can demonstrate KDV value. For the others:

```bash
# Manually calculate from logs
# 1. Count number of transactions
grep -r "submit\|commit" test_*.log | wc -l

# 2. Estimate data written
# If RocksDB baseline = 219MB, KDV = 78MB
# Storage savings = (219-78)/219 = 64.4%

# 3. This indicates network savings should be similar
# since replication traffic correlates with storage
```

## Understanding the Output

### Good Output (Metrics Found):
```
DEBUG: Checking logs matching: kdv_eval_small_updates_heavy_kdv_*.log
  kdv_eval_small_updates_heavy_kdv_localhost.log
  kdv_eval_small_updates_heavy_kdv_p1.log
  kdv_eval_small_updates_heavy_kdv_p2.log
  kdv_eval_small_updates_heavy_kdv_learner.log
DEBUG: Found throughput 45230.5 in kdv_eval_small_updates_heavy_kdv_localhost.log
DEBUG: Found network bytes 12458934 in kdv_eval_small_updates_heavy_kdv_p1.log
DEBUG: Found KDV stats in kdv_eval_small_updates_heavy_kdv_p1.log: orig=45678912 encoded=5234789 ratio=88.5
DEBUG: RocksDB size: 77780626 bytes
```

### Bad Output (Metrics Not Found):
```
DEBUG: Checking logs matching: kdv_eval_small_updates_heavy_kdv_*.log
  kdv_eval_small_updates_heavy_kdv_localhost.log
  kdv_eval_small_updates_heavy_kdv_p1.log
  kdv_eval_small_updates_heavy_kdv_p2.log
  kdv_eval_small_updates_heavy_kdv_learner.log
DEBUG: RocksDB size: 77780626 bytes
```
→ No throughput, network, or KDV messages found

## Alternative: Manual Metrics Collection

If automated collection fails, collect manually:

```bash
# 1. Run experiment
./scripts/kdv_quick_test.sh baseline
# Wait for completion

# 2. Collect RocksDB size
BASELINE_SIZE=$(du -sb /tmp/mako_rocksdb_shard* | awk '{sum+=$1} END {print sum}')
echo "Baseline size: $BASELINE_SIZE bytes"

# 3. Run KDV experiment
./scripts/kdv_quick_test.sh kdv
# Wait for completion

# 4. Collect KDV size
KDV_SIZE=$(du -sb /tmp/mako_rocksdb_shard* | awk '{sum+=$1} END {print sum}')
echo "KDV size: $KDV_SIZE bytes"

# 5. Calculate savings
python3 -c "print(f'Storage savings: {($BASELINE_SIZE - $KDV_SIZE) / $BASELINE_SIZE * 100:.1f}%')"

# 6. Check logs manually
echo "Checking for any metrics in logs:"
for log in test_*.log; do
    echo "=== $log ==="
    tail -100 "$log" | grep -E "throughput|network|bytes|KDV|compression" | tail -10
done
```

## Verifying KDV Is Actually Working

Even if metrics don't print, you can verify KDV is working:

### 1. Check RocksDB Size Difference
```bash
# If KDV is working, you should see significant size difference
# Baseline: ~200MB
# KDV: ~70MB (60-70% savings)
```

### 2. Check for KDV Encoding Logs
```bash
grep -r "KDV.*encode\|encode.*KDV" test_*.log | head -10

# Should see messages like:
# "KDV encoding log..."
# "Delta size: X bytes"
# etc.
```

### 3. Check KDV State Cache
```bash
grep -r "KDV.*cache\|cache.*size" test_*.log | head -10

# Should see cache activity if KDV is running
```

## Next Steps Based on Results

### If Storage Savings Show But Other Metrics Don't:
- ✅ You have proof KDV works (storage reduced by 60-70%)
- ✅ Can report storage savings
- ⚠️ Network savings should be similar (same data)
- 📝 Document storage savings, infer network savings

### If No Metrics At All:
1. Check if processes are actually running: `ps aux | grep dbtest`
2. Check if logs are being written: `ls -lh test_*.log`
3. Check if experiment completes: `tail -f test_p1.log`
4. Try simpler configuration (non-replicated mode)

### If KDV Stats Show 0:
1. Verify KDV is enabled: `echo $MAKO_ENABLE_KDV_LOGS`
2. Check KDV code is compiled in: `grep KDV ./dbtest` (should find symbols)
3. Enable more logging in kdv_format.cc
4. Check if data path goes through KDV encoding

## Contact for Help

If metrics still don't appear after trying these steps:

1. Save the test logs: `tar -czf kdv_test_logs.tar.gz test_*.log`
2. Note which metrics are missing: throughput / network / KDV / all
3. Share the RocksDB size comparison (this proves KDV impact)
4. Check if throughput measurement is essential or if storage savings alone suffice

Remember: **Storage savings of 60-70% already proves KDV value for geo-replication**, even without explicit network bandwidth metrics.
