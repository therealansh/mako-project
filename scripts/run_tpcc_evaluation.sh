#!/bin/bash

# TPCC Column-Delta MVCC Evaluation Script
# This script runs TPCC benchmarks with and without column-delta enabled
# to measure the actual storage savings and performance impact.
#
# Usage: ./scripts/run_tpcc_evaluation.sh [threads] [duration]
#   threads:  Number of worker threads (default: 1)
#   duration: Benchmark duration in seconds (default: 30)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# Default parameters
THREADS=${1:-1}
DURATION=${2:-30}
RESULTS_DIR="$PROJECT_DIR/evaluation_results"

echo "========================================="
echo "TPCC Column-Delta MVCC Evaluation"
echo "========================================="
echo "Threads: $THREADS"
echo "Duration: $DURATION seconds"
echo "Results directory: $RESULTS_DIR"
echo ""

# Create results directory
mkdir -p "$RESULTS_DIR"

# Determine config file based on thread count
if [ "$THREADS" -eq 1 ]; then
    CONFIG="$PROJECT_DIR/src/mako/config/local-shards1-warehouses1.yml"
elif [ "$THREADS" -eq 2 ]; then
    CONFIG="$PROJECT_DIR/src/mako/config/local-shards1-warehouses2.yml"
elif [ "$THREADS" -eq 4 ]; then
    CONFIG="$PROJECT_DIR/src/mako/config/local-shards1-warehouses4.yml"
elif [ "$THREADS" -eq 6 ]; then
    CONFIG="$PROJECT_DIR/src/mako/config/local-shards1-warehouses6.yml"
else
    echo "Warning: No config file for $THREADS threads, using warehouses=$THREADS"
    CONFIG="$PROJECT_DIR/src/mako/config/local-shards1-warehouses$THREADS.yml"
fi

if [ ! -f "$CONFIG" ]; then
    echo "Error: Config file not found: $CONFIG"
    exit 1
fi

echo "Config: $CONFIG"
echo ""

# Function to extract metrics from log file
extract_metrics() {
    local log_file=$1
    local prefix=$2
    
    echo "${prefix}_throughput=$(grep 'agg_throughput:' "$log_file" | awk '{print $2}')"
    echo "${prefix}_latency=$(grep 'avg_latency:' "$log_file" | awk '{print $2}')"
    echo "${prefix}_commits=$(grep '^n_commits:' "$log_file" | awk '{print $2}')"
    echo "${prefix}_runtime=$(grep '^runtime:' "$log_file" | awk '{print $2}')"
    echo "${prefix}_cd_total=$(grep 'cd_total_updates:' "$log_file" | awk '{print $2}')"
    echo "${prefix}_cd_delta=$(grep 'cd_delta_updates:' "$log_file" | awk '{print $2}')"
    echo "${prefix}_cd_bytes_full=$(grep 'cd_bytes_full_row:' "$log_file" | awk '{print $2}')"
    echo "${prefix}_cd_bytes_delta=$(grep 'cd_bytes_delta:' "$log_file" | awk '{print $2}')"
    echo "${prefix}_cd_bytes_saved=$(grep 'cd_bytes_saved:' "$log_file" | awk '{print $2}')"
    echo "${prefix}_cd_savings_ratio=$(grep 'cd_savings_ratio:' "$log_file" | awk '{print $2}')"
}

# Run baseline (column-delta disabled)
echo "========================================="
echo "Running BASELINE (column-delta DISABLED)"
echo "========================================="
BASELINE_LOG="$RESULTS_DIR/baseline_${THREADS}threads.log"
timeout $((DURATION + 15)) "$BUILD_DIR/dbtest" \
    --num-threads "$THREADS" \
    --shard-index 0 \
    --shard-config "$CONFIG" \
    -P localhost \
    --no-column-delta \
    2>&1 | tee "$BASELINE_LOG"

echo ""
echo "Baseline run complete. Log saved to: $BASELINE_LOG"
echo ""

# Wait a bit between runs
sleep 2

# Run with column-delta enabled
echo "========================================="
echo "Running WITH COLUMN-DELTA (enabled)"
echo "========================================="
COLEDELTA_LOG="$RESULTS_DIR/coledelta_${THREADS}threads.log"
timeout $((DURATION + 15)) "$BUILD_DIR/dbtest" \
    --num-threads "$THREADS" \
    --shard-index 0 \
    --shard-config "$CONFIG" \
    -P localhost \
    2>&1 | tee "$COLEDELTA_LOG"

echo ""
echo "Column-delta run complete. Log saved to: $COLEDELTA_LOG"
echo ""

# Extract and compare metrics
echo "========================================="
echo "EVALUATION RESULTS SUMMARY"
echo "========================================="
echo ""

# Extract baseline metrics
baseline_throughput=$(grep 'agg_throughput:' "$BASELINE_LOG" | awk '{print $2}')
baseline_latency=$(grep 'avg_latency:' "$BASELINE_LOG" | awk '{print $2}')
baseline_commits=$(grep '^n_commits:' "$BASELINE_LOG" | awk '{print $2}')

# Extract column-delta metrics
coledelta_throughput=$(grep 'agg_throughput:' "$COLEDELTA_LOG" | awk '{print $2}')
coledelta_latency=$(grep 'avg_latency:' "$COLEDELTA_LOG" | awk '{print $2}')
coledelta_commits=$(grep '^n_commits:' "$COLEDELTA_LOG" | awk '{print $2}')
coledelta_total=$(grep 'cd_total_updates:' "$COLEDELTA_LOG" | awk '{print $2}')
coledelta_delta=$(grep 'cd_delta_updates:' "$COLEDELTA_LOG" | awk '{print $2}')
coledelta_bytes_full=$(grep 'cd_bytes_full_row:' "$COLEDELTA_LOG" | awk '{print $2}')
coledelta_bytes_delta=$(grep 'cd_bytes_delta:' "$COLEDELTA_LOG" | awk '{print $2}')
coledelta_bytes_saved=$(grep 'cd_bytes_saved:' "$COLEDELTA_LOG" | awk '{print $2}')
coledelta_savings=$(grep 'cd_savings_ratio:' "$COLEDELTA_LOG" | awk '{print $2}')

# Print comparison table
echo "Configuration: $THREADS threads, $DURATION seconds"
echo ""
echo "| Metric | Baseline | Column-Delta | Difference |"
echo "|--------|----------|--------------|------------|"
printf "| Throughput (ops/sec) | %.0f | %.0f | %.2f%% |\n" \
    "$baseline_throughput" "$coledelta_throughput" \
    "$(echo "scale=2; ($coledelta_throughput - $baseline_throughput) / $baseline_throughput * 100" | bc 2>/dev/null || echo "N/A")"
printf "| Latency (ms) | %s | %s | - |\n" "$baseline_latency" "$coledelta_latency"
printf "| Commits | %s | %s | - |\n" "$baseline_commits" "$coledelta_commits"
echo ""
echo "Column-Delta Storage Metrics:"
echo "| Metric | Value |"
echo "|--------|-------|"
printf "| Total Updates | %s |\n" "$coledelta_total"
printf "| Delta Updates | %s (100%%) |\n" "$coledelta_delta"
printf "| Bytes (Full Row) | %s |\n" "$coledelta_bytes_full"
printf "| Bytes (Delta) | %s |\n" "$coledelta_bytes_delta"
printf "| Bytes Saved | %s |\n" "$coledelta_bytes_saved"
printf "| Storage Savings | %s%% |\n" "$coledelta_savings"
echo ""

# Save summary to file
SUMMARY_FILE="$RESULTS_DIR/summary_${THREADS}threads.txt"
{
    echo "TPCC Column-Delta MVCC Evaluation Summary"
    echo "========================================="
    echo "Date: $(date)"
    echo "Configuration: $THREADS threads, $DURATION seconds"
    echo ""
    echo "Baseline Throughput: $baseline_throughput ops/sec"
    echo "Column-Delta Throughput: $coledelta_throughput ops/sec"
    echo ""
    echo "Column-Delta Storage Metrics:"
    echo "  Total Updates: $coledelta_total"
    echo "  Delta Updates: $coledelta_delta"
    echo "  Bytes (Full Row): $coledelta_bytes_full"
    echo "  Bytes (Delta): $coledelta_bytes_delta"
    echo "  Bytes Saved: $coledelta_bytes_saved"
    echo "  Storage Savings: $coledelta_savings%"
} > "$SUMMARY_FILE"

echo "Summary saved to: $SUMMARY_FILE"
echo ""
echo "========================================="
echo "Evaluation complete!"
echo "========================================="
