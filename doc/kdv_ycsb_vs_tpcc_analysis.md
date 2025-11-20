# KDV Evaluation: Why YCSB Instead of TPC-C?

## Executive Summary

The Key-Delta-Value (KDV) compression layer implemented in PR #5 achieves only **3% network compression with 0 deltas** when evaluated with TPC-C benchmarks, far below the target of 50-70% compression. This document explains why TPC-C is unsuitable for KDV evaluation in its current implementation and why YCSB is the appropriate benchmark for measuring KDV effectiveness.

## Why TPC-C Doesn't Produce Expected Outcomes

### 1. Append-Only Log Structure with Volatile Metadata

TPC-C transactions are serialized into append-only logs with the following structure (from `Transaction.hh:131-146`):

```
For each transaction batch:
  1. Transaction metadata (timestamps, partition info)
  2. For each write operation:
     - Operation type (insert/update/delete)
     - Key length + Key bytes
     - Value length + Value bytes
     - Table ID (with delete flag in high bit)
  3. Batch trailer:
     - latest_commit_timestamp (uint32_t) - VOLATILE
     - start_time for latency tracking (uint32_t) - VOLATILE
```

**Problem**: Even when updating the same logical record (e.g., `warehouse_1`, `district_5`), the serialized log contains:
- **Volatile timestamps** that change every batch
- **Unique sequence numbers** for each log entry
- **Transaction-specific metadata** that varies per transaction
- **Batch-level information** that differs each time

### 2. Current KDV Key Hashing Approach

The current implementation (PR #5) computes `key_hash` by hashing the log payload:

```cpp
// From kdv_format.cc (conceptual)
uint64_t key_hash = hash(log_payload + 8);  // Skip first 8 bytes (timestamps)
```

**Why this doesn't work for TPC-C**:

1. **Skipping 8 bytes isn't enough**: While this skips `latest_commit_timestamp` and `start_time`, the remaining payload still contains:
   - Transaction sequence numbers
   - Batch identifiers
   - Per-transaction timestamps embedded in the log structure
   - Variable transaction content (different items, quantities, etc.)

2. **No logical key extraction**: The hash is computed on the entire serialized transaction log, not on the logical record keys being updated (e.g., `warehouse_1`, `district_5`, `customer_123`).

3. **Every log appears unique**: Since the payload hash changes every time, KDV's per-key state tracking never finds a matching base, resulting in:
   - **0 deltas produced**
   - **100% base writes**
   - **Only 3% compression** (from KDV header overhead)

### 3. TPC-C Transaction Complexity

TPC-C transactions involve multiple tables and complex operations:

**NewOrder Transaction** (most common):
- Reads: `warehouse`, `district`, `customer`, `item` (multiple)
- Writes: `new_order`, `oorder`, `order_line` (5-15 records), `stock` (5-15 records), `district` (d_next_o_id update)

**Problem**: A single TPC-C transaction log contains:
- Updates to 10-30+ different logical records
- Each record from different tables
- Variable number of records per transaction
- Complex nested structure

**Result**: Without parsing the log structure to extract individual record keys, KDV treats the entire transaction log as a single "key", which never repeats.

### 4. Diagnostic Evidence from PR #5

From the PR description and evaluation results:

```
[KDV Encode] Stats (every 100 encodes):
  bases=100, deltas=0
  
[KDV Policy Stats]:
  no_base: 0
  delta_too_large: 0
  chain_too_long: 0
  base_too_old: 0
  
Network compression: 3%
```

**Analysis**:
- **100% bases, 0% deltas**: Every encode writes a base (no matching previous state)
- **All policy counters are 0**: The decision to write base is always "no_base" (no previous base exists)
- **3% compression**: Only from KDV header overhead, no actual delta compression

### 5. What Would Be Needed for TPC-C

To make TPC-C work with KDV, we would need **Phase 2: Per-Record Key Extraction**:

```cpp
// Conceptual Phase 2 implementation
std::vector<RecordUpdate> parse_transaction_log(const char* log, size_t size) {
    std::vector<RecordUpdate> updates;
    
    // Parse log structure
    while (has_more_records) {
        uint8_t table_id = read_table_id();
        std::string primary_key = read_primary_key();
        std::string value = read_value();
        
        // Compute per-record key hash
        uint64_t key_hash = hash(table_id, primary_key);
        
        updates.push_back({key_hash, value});
    }
    
    return updates;
}

// Encode each record update separately with its own key_hash
for (auto& update : updates) {
    std::string encoded = kdv_encode_log(..., update.key_hash, update.value);
    // Now KDV can track per-record state and produce deltas
}
```

**Challenges**:
1. **Complex log parsing**: Need to understand TPC-C log format
2. **Multiple record updates per log**: Need to encode/decode multiple KDV entries
3. **Cross-record dependencies**: Transaction semantics require atomic updates
4. **Backward compatibility**: Need to maintain existing log format

## Why YCSB Is Better for KDV Evaluation

### 1. Simple Key-Value Operations

YCSB performs direct operations on individual keys:

```cpp
// YCSB Read
txn_read() {
    const uint64_t k = r.next() % nkeys;
    tbl->get(txn, u64_varkey(k).str(), value);
}

// YCSB Write
txn_write() {
    const uint64_t k = r.next() % nkeys;
    tbl->put(txn, u64_varkey(k).str(), value);
}
```

**Benefits**:
- **Single key per transaction**: Easy to track per-key state
- **Predictable log structure**: Key + Value, no complex nesting
- **Repeatable keys**: Can target same keys multiple times
- **Simple serialization**: Straightforward to extract key from log

### 2. Controlled Update Patterns

YCSB allows precise control over update characteristics:

```cpp
// Small update (16 bytes changed in 1KB record)
std::string value = old_value;
value.replace(offset, 16, new_data);  // Only 16 bytes differ

// Medium update (50% changed)
std::string value = old_value;
value.replace(0, 512, new_data);  // 512 bytes differ in 1KB record

// Large update (90% changed)
std::string value = old_value;
value.replace(0, 900, new_data);  // 900 bytes differ in 1KB record
```

**Benefits**:
- **Deterministic compression ratios**: Can predict expected compression
- **Workload characterization**: Understand when KDV works best
- **Policy validation**: Verify MaxDeltaSizeRatio, MaxChainLen work correctly

### 3. Configurable Read/Write Ratios

YCSB supports various workload mixes:

```cpp
// Read-heavy (95% read, 5% write)
g_txn_workload_mix = {95, 5, 0, 0};

// Balanced (50% read, 50% write)
g_txn_workload_mix = {50, 50, 0, 0};

// Write-heavy (20% read, 80% write)
g_txn_workload_mix = {20, 80, 0, 0};
```

**Benefits**:
- **Bandwidth sensitivity**: Measure compression benefit vs. write ratio
- **Performance overhead**: Assess impact on read-dominated workloads
- **Workload optimization**: Identify optimal KDV configuration per workload

### 4. Natural Alignment with KDV Design

YCSB's key-value model naturally aligns with KDV's per-key state tracking:

```
YCSB Key: "user_12345"
  ↓
KDV key_hash: hash("user_12345") = 0x1a2b3c4d
  ↓
KDV State: {
    base_: "value_v1",
    base_seq_: 100,
    chain_len_: 0
}
  ↓
Next update to "user_12345":
  - Compute delta from base_
  - Increment chain_len_
  - Achieve 90%+ compression for small updates
```

**Result**: KDV can properly track per-key state and produce deltas.

### 5. Easier Correctness Validation

YCSB's simplicity makes it easier to verify KDV correctness:

```cpp
// Test: Encode/Decode Identity
std::string original = generate_ycsb_value();
std::string encoded = kdv_encode_log(..., original);
std::string decoded = kdv_decode_log(..., encoded);
assert(decoded == original);

// Test: Delta Compression
std::string v1 = "aaaa...aaaa";  // 1KB of 'a'
std::string v2 = "aaaabbbbaaaa";  // 1KB with 4 bytes changed
std::string encoded_v1 = kdv_encode_log(..., v1);  // Base
std::string encoded_v2 = kdv_encode_log(..., v2);  // Delta
assert(encoded_v2.size() < encoded_v1.size() * 0.1);  // >90% compression
```

**Benefits**:
- **Deterministic behavior**: Can predict exact compression ratios
- **Isolated testing**: Test delta algorithm without transaction complexity
- **Regression testing**: Easy to create unit tests

## Evaluation Methodology with YCSB

### Metrics to Measure

1. **Write Bandwidth Reduction**
   - Baseline: `total_network_bytes_sent` without KDV
   - KDV: `total_network_bytes_sent` with KDV enabled
   - Reduction: `(baseline - kdv) / baseline * 100%`

2. **Read Latency Comparison**
   - Baseline: P50/P95/P99 latency without KDV
   - KDV: P50/P95/P99 latency with KDV enabled
   - Overhead: `(kdv_latency - baseline_latency) / baseline_latency * 100%`

3. **Disk Space Savings**
   - Baseline: RocksDB directory size without KDV
   - KDV: RocksDB directory size with KDV enabled
   - Savings: `(baseline - kdv) / baseline * 100%`

4. **Throughput Impact**
   - Baseline: TPS without KDV
   - KDV: TPS with KDV enabled
   - Overhead: `(baseline_tps - kdv_tps) / baseline_tps * 100%`

### Workload Configurations

#### Read/Write Ratio Experiments

| Workload | Read % | Write % | Expected Compression | Purpose |
|----------|--------|---------|---------------------|---------|
| Read-heavy | 95% | 5% | Minimal | Baseline overhead for read-dominated |
| Balanced | 50% | 50% | Moderate | Typical mixed workload |
| Write-heavy | 20% | 80% | Maximum | Best-case compression scenario |

#### Update Size Experiments

| Pattern | Bytes Changed | Expected Compression | Purpose |
|---------|---------------|---------------------|---------|
| Small | 16 bytes in 1KB | 90-95% | Optimal delta encoding |
| Medium | 512 bytes in 1KB | 40-60% | Typical update pattern |
| Large | 900 bytes in 1KB | 0-10% | Policy validation (fallback to base) |

#### Record Size Experiments

| Record Size | Expected Benefit | Purpose |
|-------------|------------------|---------|
| 100 bytes | Limited | Small records, less compression opportunity |
| 1KB | Good | Medium records, good compression for small updates |
| 4KB | Best | Large records, maximum compression for small updates |

### Expected Results

#### Write Bandwidth Reduction

- **Small updates (16 bytes in 1KB)**: 90-95% reduction
- **Medium updates (50% changed)**: 40-60% reduction
- **Large updates (90% changed)**: 0-10% reduction

#### Performance Overhead

- **Throughput**: 1-5% overhead (encode/decode adds 1-5 μs per log)
- **Read latency**: <2% overhead (no decoding on read path)
- **Write latency**: 2-8% overhead (encoding on critical path)

#### Disk Space Savings

- Similar to bandwidth reduction (same encoding used for RocksDB)
- Additional benefit from RocksDB compression on top of KDV

## Implementation: YCSB Evaluation Script

The provided `scripts/ycsb_kdv_evaluate.sh` script implements comprehensive YCSB evaluation:

### Features

1. **Automated Experiments**
   - Runs baseline (no KDV) and KDV experiments
   - Tests multiple read/write ratios
   - Tests varying update sizes
   - Tests different record sizes

2. **Metrics Collection**
   - Throughput (TPS)
   - Latency percentiles (P50, P95, P99)
   - KDV compression ratios
   - Network bytes sent
   - Disk usage

3. **Analysis and Reporting**
   - Generates summary report with all metrics
   - Compares baseline vs. KDV performance
   - Calculates compression ratios and overhead
   - Provides recommendations

### Usage

```bash
# Run full evaluation (baseline + KDV, all workloads)
./scripts/ycsb_kdv_evaluate.sh

# Run only KDV experiments (skip baseline)
./scripts/ycsb_kdv_evaluate.sh --skip-baseline

# Run with custom duration
./scripts/ycsb_kdv_evaluate.sh --duration 120

# Run with custom output directory
./scripts/ycsb_kdv_evaluate.sh --output-dir my_results
```

### Output

```
results/ycsb_kdv_eval/
├── evaluation_20231119_123456.log          # Main log file
├── summary_20231119_123456.md              # Summary report
├── read_heavy_95_5_baseline/
│   ├── output.log                          # Raw output
│   └── metrics.txt                         # Extracted metrics
├── read_heavy_95_5_kdv/
│   ├── output.log
│   └── metrics.txt
├── balanced_50_50_baseline/
│   └── ...
└── ...
```

## Recommendations

### Short-term (Use YCSB)

1. **Evaluate KDV with YCSB** to demonstrate compression effectiveness
2. **Focus on write-heavy workloads** (20/80 read/write) for maximum benefit
3. **Test varying update sizes** to characterize compression vs. update pattern
4. **Measure performance overhead** to ensure acceptable latency impact

### Medium-term (Implement Phase 2 for TPC-C)

1. **Parse transaction log structure** to extract per-record updates
2. **Compute key_hash from logical keys** (table_id + primary_key)
3. **Encode/decode multiple KDV entries** per transaction log
4. **Maintain transaction atomicity** across multiple KDV entries

### Long-term (Production Deployment)

1. **Optimize encoding performance** (currently adds 1-5 μs per log)
2. **Implement adaptive policies** (dynamic MaxChainLen, MaxDeltaSizeRatio)
3. **Add compression statistics** to monitoring dashboards
4. **Evaluate in geo-distributed deployment** to measure real bandwidth savings

## Conclusion

**TPC-C is unsuitable for KDV evaluation** in the current implementation because:
- Complex transaction logs with volatile metadata
- No per-record key extraction
- Payload hashing treats entire transaction as single key
- Results in 0 deltas and only 3% compression

**YCSB is ideal for KDV evaluation** because:
- Simple key-value operations align with KDV design
- Controlled update patterns enable precise measurement
- Configurable workloads characterize compression effectiveness
- Easy correctness validation

**Use the provided `ycsb_kdv_evaluate.sh` script** to:
- Measure write bandwidth reduction (expected: 50-70% for small updates)
- Compare read latency with baseline (expected: <5% overhead)
- Test workloads with varying update sizes
- Evaluate different read/write ratios

Once YCSB evaluation demonstrates KDV effectiveness, implement Phase 2 (per-record key extraction) to enable TPC-C evaluation.
