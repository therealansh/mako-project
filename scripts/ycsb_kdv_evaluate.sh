#!/bin/bash
#
# 
#
#

set -e

SKIP_BASELINE=0
SKIP_KDV=0
SKIP_BUILD=0
DURATION=60
OUTPUT_DIR="results/ycsb_kdv_eval"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-baseline)
            SKIP_BASELINE=1
            shift
            ;;
        --skip-kdv)
            SKIP_KDV=1
            shift
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --duration)
            DURATION="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --help)
            echo "YCSB KDV Evaluation Script"
            echo ""
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --skip-baseline     Skip baseline (non-KDV) experiments"
            echo "  --skip-kdv          Skip KDV experiments"
            echo "  --skip-build        Skip rebuilding the project"
            echo "  --duration SECS     Duration for each experiment (default: 60)"
            echo "  --output-dir DIR    Output directory for results (default: results/ycsb_kdv_eval)"
            echo "  --help              Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

mkdir -p "$OUTPUT_DIR"

LOGFILE="$OUTPUT_DIR/evaluation_${TIMESTAMP}.log"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOGFILE"
}

log "=========================================="
log "YCSB KDV Evaluation Script"
log "=========================================="
log "Configuration:"
log "  Duration: ${DURATION}s"
log "  Output directory: $OUTPUT_DIR"
log "  Skip baseline: $SKIP_BASELINE"
log "  Skip KDV: $SKIP_KDV"
log "  Skip build: $SKIP_BUILD"
log "=========================================="

if [ $SKIP_BUILD -eq 0 ]; then
    log "Building project..."
    export PATH="$HOME/.cargo/bin:$PATH"
    make clean
    make -j4 2>&1 | tee "$OUTPUT_DIR/build_${TIMESTAMP}.log"
    log "Build completed"
else
    log "Skipping build (--skip-build specified)"
fi

cleanup() {
    log "Cleaning up..."
    pkill -9 dbtest || true
    pkill -9 deptran_server || true
    rm -rf /tmp/*_mako_rocksdb_shard* || true
    sleep 2
}

run_ycsb_experiment() {
    local exp_name=$1
    local workload_mix=$2  # Format: "R,W,RMW,Scan" e.g., "95,5,0,0"
    local record_size=$3
    local num_keys=$4
    local enable_kdv=$5
    local update_pattern=$6  # "small", "medium", "large"
    
    local exp_dir="$OUTPUT_DIR/${exp_name}"
    mkdir -p "$exp_dir"
    
    local update_bytes=0
    local update_mode="middle"
    case "$update_pattern" in
        small)
            update_bytes=16
            update_mode="middle"
            ;;
        medium)
            update_bytes=$((record_size / 2))
            update_mode="middle"
            ;;
        large)
            update_bytes=$((record_size * 9 / 10))
            update_mode="middle"
            ;;
    esac
    
    log "----------------------------------------"
    log "Running experiment: $exp_name"
    log "  Workload mix: $workload_mix"
    log "  Record size: $record_size bytes"
    log "  Number of keys: $num_keys"
    log "  KDV enabled: $enable_kdv"
    log "  Update pattern: $update_pattern (${update_bytes} bytes)"
    log "----------------------------------------"
    
    cleanup
    
    if [ "$enable_kdv" = "true" ]; then
        export MAKO_ENABLE_KDV_LOGS=1
        export DISABLE_DISK=OFF
    else
        unset MAKO_ENABLE_KDV_LOGS
        export DISABLE_DISK=OFF
    fi
    
    local output_file="$exp_dir/output.log"
    local metrics_file="$exp_dir/metrics.txt"
    
    log "Starting dbtest..."
    timeout $((DURATION + 30)) ./build/dbtest \
        --bench ycsb \
        --num-threads 4 \
        -w "$workload_mix" \
        -r $record_size \
        -u $update_bytes \
        -m $update_mode \
        2>&1 | tee "$output_file" || true
    
    log "Extracting metrics..."
    
    local tps=$(grep -oP 'agg_throughput: \K[0-9.]+' "$output_file" | tail -1 || echo "0")
    
    local p50=$(grep -oP 'RESULT.*p50.*\K[0-9.]+' "$output_file" | tail -1 || echo "0")
    local p95=$(grep -oP 'RESULT.*p95.*\K[0-9.]+' "$output_file" | tail -1 || echo "0")
    local p99=$(grep -oP 'RESULT.*p99.*\K[0-9.]+' "$output_file" | tail -1 || echo "0")
    
    local bases=0
    local deltas=0
    local compression_ratio=0
    local network_bytes=0
    local original_bytes=0
    
    if [ "$enable_kdv" = "true" ]; then
        bases=$(grep -oP '\[KDV Encode\].*bases=\K[0-9]+' "$output_file" | tail -1 || echo "0")
        deltas=$(grep -oP '\[KDV Encode\].*deltas=\K[0-9]+' "$output_file" | tail -1 || echo "0")
        compression_ratio=$(grep -oP 'Network compression: \K[0-9.]+' "$output_file" | tail -1 || echo "0")
        network_bytes=$(grep -oP 'total_network_bytes_sent: \K[0-9]+' "$output_file" | tail -1 || echo "0")
        original_bytes=$(grep -oP 'total_original_bytes: \K[0-9]+' "$output_file" | tail -1 || echo "0")
    fi
    
    local disk_usage=$(du -sb /tmp/*_mako_rocksdb_shard* 2>/dev/null | awk '{sum+=$1} END {print sum}' || echo "0")
    
    cat > "$metrics_file" <<EOF
experiment_name: $exp_name
workload_mix: $workload_mix
record_size: $record_size
num_keys: $num_keys
kdv_enabled: $enable_kdv
update_pattern: $update_pattern
duration: $DURATION
throughput_tps: $tps
latency_p50_us: $p50
latency_p95_us: $p95
latency_p99_us: $p99
kdv_bases: $bases
kdv_deltas: $deltas
kdv_compression_ratio: $compression_ratio
network_bytes: $network_bytes
original_bytes: $original_bytes
disk_usage_bytes: $disk_usage
timestamp: $(date '+%Y-%m-%d %H:%M:%S')
EOF
    
    log "Metrics saved to $metrics_file"
    log "Throughput: $tps TPS"
    log "Latency P50/P95/P99: $p50 / $p95 / $p99 us"
    if [ "$enable_kdv" = "true" ]; then
        log "KDV Bases/Deltas: $bases / $deltas"
        log "KDV Compression: $compression_ratio%"
    fi
    log "Disk usage: $disk_usage bytes"
    
    cleanup
}


EXPERIMENTS=(
    "read_heavy_95_5_baseline:95,0,5,0:1024:100000:small"
    "read_heavy_95_5_kdv:95,0,5,0:1024:100000:small"
    
    "balanced_50_50_baseline:50,0,50,0:1024:100000:small"
    "balanced_50_50_kdv:50,0,50,0:1024:100000:small"
    
    "write_heavy_20_80_baseline:20,0,80,0:1024:100000:small"
    "write_heavy_20_80_kdv:20,0,80,0:1024:100000:small"
    
    "update_small_baseline:50,0,50,0:1024:100000:small"
    "update_small_kdv:50,0,50,0:1024:100000:small"
    
    "update_medium_baseline:50,0,50,0:1024:100000:medium"
    "update_medium_kdv:50,0,50,0:1024:100000:medium"
    
    "update_large_baseline:50,0,50,0:1024:100000:large"
    "update_large_kdv:50,0,50,0:1024:100000:large"
    
    "record_100b_baseline:50,0,50,0:100:100000:small"
    "record_100b_kdv:50,0,50,0:100:100000:small"
    
    "record_1kb_baseline:50,0,50,0:1024:100000:small"
    "record_1kb_kdv:50,0,50,0:1024:100000:small"
    
    "record_4kb_baseline:50,0,50,0:4096:100000:small"
    "record_4kb_kdv:50,0,50,0:4096:100000:small"
)

for exp_config in "${EXPERIMENTS[@]}"; do
    IFS=':' read -r exp_name workload_mix record_size num_keys update_pattern <<< "$exp_config"
    
    if [[ "$exp_name" == *"baseline"* ]]; then
        if [ $SKIP_BASELINE -eq 1 ]; then
            log "Skipping baseline experiment: $exp_name"
            continue
        fi
        enable_kdv="false"
    else
        if [ $SKIP_KDV -eq 1 ]; then
            log "Skipping KDV experiment: $exp_name"
            continue
        fi
        enable_kdv="true"
    fi
    
    run_ycsb_experiment "$exp_name" "$workload_mix" "$record_size" "$num_keys" "$enable_kdv" "$update_pattern"
done

log "=========================================="
log "Generating summary report..."
log "=========================================="

SUMMARY_FILE="$OUTPUT_DIR/summary_${TIMESTAMP}.md"

cat > "$SUMMARY_FILE" <<'EOF'


This report summarizes the evaluation of the Key-Delta-Value (KDV) compression layer using YCSB workloads.


1. **Quantify bandwidth savings**: Measure reduction in cross-datacenter network traffic
2. **Quantify disk savings**: Measure reduction in persistent storage footprint
3. **Measure performance overhead**: Assess impact on throughput and latency
4. **Validate correctness**: Ensure KDV encoding/decoding preserves data integrity
5. **Characterize workload sensitivity**: Understand when KDV provides maximum benefit



| Experiment | Workload | Record Size | Keys | KDV | TPS | P50 (us) | P95 (us) | P99 (us) | Compression | Disk (MB) |
|------------|----------|-------------|------|-----|-----|----------|----------|----------|-------------|-----------|
EOF

for exp_config in "${EXPERIMENTS[@]}"; do
    IFS=':' read -r exp_name workload_mix record_size num_keys update_pattern <<< "$exp_config"
    
    metrics_file="$OUTPUT_DIR/${exp_name}/metrics.txt"
    if [ -f "$metrics_file" ]; then
        tps=$(grep -oP 'throughput_tps: \K[0-9.]+' "$metrics_file" || echo "N/A")
        p50=$(grep -oP 'latency_p50_us: \K[0-9.]+' "$metrics_file" || echo "N/A")
        p95=$(grep -oP 'latency_p95_us: \K[0-9.]+' "$metrics_file" || echo "N/A")
        p99=$(grep -oP 'latency_p99_us: \K[0-9.]+' "$metrics_file" || echo "N/A")
        compression=$(grep -oP 'kdv_compression_ratio: \K[0-9.]+' "$metrics_file" || echo "N/A")
        disk_bytes=$(grep -oP 'disk_usage_bytes: \K[0-9]+' "$metrics_file" || echo "0")
        disk_mb=$(echo "scale=2; $disk_bytes / 1024 / 1024" | bc || echo "N/A")
        kdv_enabled=$(grep -oP 'kdv_enabled: \K\w+' "$metrics_file" || echo "N/A")
        
        echo "| $exp_name | $workload_mix | $record_size | $num_keys | $kdv_enabled | $tps | $p50 | $p95 | $p99 | $compression% | $disk_mb |" >> "$SUMMARY_FILE"
    fi
done

cat >> "$SUMMARY_FILE" <<'EOF'



Compare network bytes sent between baseline and KDV experiments:

- **Read-heavy (95/5)**: Expected minimal benefit (few writes to compress)
- **Balanced (50/50)**: Expected moderate benefit
- **Write-heavy (20/80)**: Expected maximum benefit (many writes to compress)


Compare P50/P95/P99 latencies between baseline and KDV experiments:

- **Overhead**: Calculate `(kdv_latency - baseline_latency) / baseline_latency * 100%`
- **Expected**: <5% overhead for read operations (no decoding on read path)
- **Expected**: 2-8% overhead for write operations (encoding on critical path)


Compare compression ratios across different update patterns:

- **Small updates (16 bytes changed)**: Expected 90-95% compression
- **Medium updates (50% changed)**: Expected 40-60% compression
- **Large updates (90% changed)**: Expected 0-10% compression (fallback to base)


Compare compression effectiveness across different record sizes:

- **100 bytes**: Limited compression opportunity
- **1KB**: Good compression for small updates
- **4KB**: Best compression for small updates



TPC-C is not producing the expected 50-70% compression for the following reasons:

1. **Append-Only Log Structure**: TPC-C uses append-only transaction logs where each entry has a unique sequence number and volatile metadata (timestamps).

2. **Payload Hashing Issue**: The current KDV implementation (PR #5) computes `key_hash` by hashing the log payload. Even after skipping the first 8 bytes (volatile timestamps), the payload still contains:
   - Transaction-specific metadata that changes every time
   - Batch sequence numbers
   - Per-transaction timestamps
   - Variable transaction content

3. **No Per-Record Key Extraction**: The system doesn't parse the transaction log structure to extract true per-record keys (e.g., `warehouse_1`, `district_5`, `customer_123`). Without identifying logical record keys, KDV cannot recognize that multiple transactions are updating the same logical records.

4. **Result**: Every log entry appears unique to KDV, resulting in 0 deltas and only base writes (3% compression from header overhead only).


YCSB is ideal for evaluating KDV because:

1. **Simple Key-Value Operations**: YCSB performs direct get/put operations on individual keys, making it easy to track per-key updates.

2. **Controlled Update Patterns**: YCSB allows precise control over:
   - Which keys are updated (can target same keys repeatedly)
   - How much of each record changes (can modify specific byte ranges)
   - Read/write ratios

3. **Predictable Workload**: Unlike TPC-C's complex multi-table transactions, YCSB's simple operations make it easier to:
   - Verify correctness of delta encoding
   - Measure compression ratios accurately
   - Isolate performance bottlenecks

4. **Per-Key State Tracking**: YCSB's key-value model naturally aligns with KDV's per-key state tracking, allowing proper delta compression.


1. **Use YCSB for KDV evaluation** until per-record key extraction is implemented for TPC-C
2. **Implement Phase 2** (per-record key extraction) to enable TPC-C evaluation:
   - Parse transaction log structure
   - Extract table_id + primary_key for each record update
   - Use `hash(table_id, primary_key)` as `key_hash` instead of payload hash
3. **Focus on write-heavy YCSB workloads** to demonstrate maximum KDV benefit
4. **Test with varying update sizes** to characterize compression effectiveness


- Individual experiment outputs: `$OUTPUT_DIR/<experiment_name>/output.log`
- Individual experiment metrics: `$OUTPUT_DIR/<experiment_name>/metrics.txt`
- Evaluation log: `$OUTPUT_DIR/evaluation_<timestamp>.log`
- This summary: `$OUTPUT_DIR/summary_<timestamp>.md`

EOF

log "Summary report generated: $SUMMARY_FILE"
log "=========================================="
log "Evaluation complete!"
log "=========================================="
log "Results directory: $OUTPUT_DIR"
log "Summary report: $SUMMARY_FILE"
log "Log file: $LOGFILE"

cat "$SUMMARY_FILE"
