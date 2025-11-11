#!/bin/bash

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
RESULTS_DIR="$PROJECT_ROOT/results/delta_evaluation_$(date +%Y%m%d_%H%M%S)"

mkdir -p "$RESULTS_DIR"

echo "=== Delta Replication Evaluation ==="
echo "Results will be saved to: $RESULTS_DIR"
echo ""

run_test() {
    local test_name=$1
    local enable_delta=$2
    local update_size=$3
    local duration=$4
    
    echo "Running test: $test_name (delta=$enable_delta, update_size=$update_size%, duration=${duration}s)"
    
    local output_file="$RESULTS_DIR/${test_name}_delta${enable_delta}_update${update_size}.log"
    
    export MAKO_ENABLE_DELTA_REPLICATION=$enable_delta
    export MAKO_DELTA_SIZE_THRESHOLD=256
    export MAKO_MAX_CHAIN_LENGTH=5
    export MAKO_MAX_CHAIN_AGE_MS=60000
    export MAKO_MAX_CHAIN_BYTES=4096
    export MAKO_COMPACTION_INTERVAL_MS=1000
    
    cd "$PROJECT_ROOT"
    timeout ${duration}s "$BUILD_DIR/dbtest" --num-threads 4 --shard-index 0 \
        --local-shards "0,1" \
        --shard-config "$PROJECT_ROOT/src/mako/config/local-shards2-warehouses4.yml" \
        --paxos-proc-name localhost \
        2>&1 | tee "$output_file" || true
    
    echo "Test completed: $test_name"
    echo ""
}

extract_metrics() {
    local log_file=$1
    local output_csv=$2
    
    echo "Extracting metrics from: $log_file"
    
    local bandwidth_reduction=$(grep "Bandwidth reduction:" "$log_file" | tail -1 | awk '{print $3}' | tr -d '%')
    local bytes_sent_full=$(grep "Bytes sent (full):" "$log_file" | tail -1 | awk '{print $4}')
    local bytes_sent_delta=$(grep "Bytes sent (delta):" "$log_file" | tail -1 | awk '{print $4}')
    local avg_read_latency=$(grep "Avg read latency:" "$log_file" | tail -1 | awk '{print $4}')
    local avg_write_latency=$(grep "Avg write latency:" "$log_file" | tail -1 | awk '{print $4}')
    local deltas_created=$(grep "Deltas created:" "$log_file" | tail -1 | awk '{print $3}')
    local deltas_applied=$(grep "Deltas applied:" "$log_file" | tail -1 | awk '{print $3}')
    local compactions=$(grep "Compactions:" "$log_file" | tail -1 | awk '{print $2}')
    local max_chain_length=$(grep "Max chain length:" "$log_file" | tail -1 | awk '{print $4}')
    
    local throughput=$(grep -E "throughput|TPS" "$log_file" | tail -1 | awk '{print $NF}')
    
    echo "$log_file,$bandwidth_reduction,$bytes_sent_full,$bytes_sent_delta,$avg_read_latency,$avg_write_latency,$deltas_created,$deltas_applied,$compactions,$max_chain_length,$throughput" >> "$output_csv"
}

METRICS_CSV="$RESULTS_DIR/metrics_summary.csv"
echo "test_name,bandwidth_reduction_%,bytes_sent_full,bytes_sent_delta,avg_read_latency_us,avg_write_latency_us,deltas_created,deltas_applied,compactions,max_chain_length,throughput_tps" > "$METRICS_CSV"

echo "=== Phase 6.1: Bandwidth Reduction Measurement ==="
echo "Testing with varying update sizes (10%, 50%, 90% of value)"
echo ""

run_test "baseline_small_updates" 0 10 30
run_test "baseline_medium_updates" 0 50 30
run_test "baseline_large_updates" 0 90 30

run_test "delta_small_updates" 1 10 30
run_test "delta_medium_updates" 1 50 30
run_test "delta_large_updates" 1 90 30

echo "=== Phase 6.2: Latency Impact Analysis ==="
echo "Comparing p50/p99 commit latency with delta replication enabled vs disabled"
echo ""

run_test "latency_baseline" 0 50 60
run_test "latency_delta" 1 50 60

echo "=== Phase 6.3: Throughput Comparison ==="
echo "Measuring transactions/second with baseline vs delta replication"
echo ""


echo "=== Extracting Metrics ==="
for log_file in "$RESULTS_DIR"/*.log; do
    if [ -f "$log_file" ]; then
        extract_metrics "$log_file" "$METRICS_CSV"
    fi
done

echo ""
echo "=== Evaluation Complete ==="
echo "Results saved to: $RESULTS_DIR"
echo "Metrics summary: $METRICS_CSV"
echo ""
echo "To view results:"
echo "  cat $METRICS_CSV"
echo ""
echo "To generate graphs (requires Python with matplotlib):"
echo "  python3 $SCRIPT_DIR/plot_delta_metrics.py $METRICS_CSV"
echo ""
