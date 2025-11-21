#!/bin/bash
#
# KDV Network and Storage Evaluation Script
#
# This script specifically measures:
# 1. Network bandwidth reduction (Paxos replication traffic)
# 2. Storage space reduction (RocksDB disk usage)
# 3. KDV compression ratios
#
# Focus: Repeated updates to same keys with varying delta sizes
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="${PROJECT_ROOT}/results/kdv_network_storage_eval"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RUNTIME=60  # seconds per experiment
THREADS=6
NUM_KEYS=10000  # Smaller key space for more repeated updates

echo "========================================="
echo "KDV Network & Storage Evaluation"
echo "========================================="
echo "Output directory: $OUTPUT_DIR"
echo "Runtime per experiment: ${RUNTIME}s"
echo "Timestamp: $TIMESTAMP"
echo ""

mkdir -p "$OUTPUT_DIR"

# Workload configurations designed to maximize KDV benefits
# Format: "name:read_pct,write_pct,rmw_pct,scan_pct:record_size:update_bytes:description"
WORKLOADS=(
    # Scenario 1: Heavy repeated small updates (best case for KDV)
    "small_updates_heavy:10,0,90,0:1024:16:Heavy updates with 16-byte changes (1.5% of record)"

    # Scenario 2: Moderate repeated medium updates
    "medium_updates_moderate:30,0,70,0:1024:256:Moderate updates with 256-byte changes (25% of record)"

    # Scenario 3: Balanced workload with small updates
    "small_updates_balanced:50,0,50,0:1024:16:Balanced workload with small deltas"

    # Scenario 4: Read-heavy with occasional small updates
    "small_updates_read_heavy:90,0,10,0:1024:16:Read-heavy with occasional 16-byte updates"

    # Scenario 5: Large updates (worst case - should fall back to base)
    "large_updates:50,0,50,0:1024:920:Large updates (90% of record - should use base encoding)"

    # Scenario 6: Varying record sizes with small updates
    "small_record_small_delta:50,0,50,0:256:16:256-byte records, 16-byte updates (6.25% change)"
    "large_record_small_delta:50,0,50,0:4096:64:4KB records, 64-byte updates (1.5% change)"
)

cleanup() {
    echo "Cleaning up processes..." >&2
    pkill -9 dbtest || true
    sleep 2
    rm -rf /tmp/mako_rocksdb_shard* || true
    rm -f ${PROJECT_ROOT}/kdv_eval_*.log || true
}

# Extract metrics from all log files - check each for different metrics
extract_metrics() {
    local exp_name=$1
    local kdv_enabled=$2
    local log_pattern="${PROJECT_ROOT}/kdv_eval_${exp_name}_*.log"

    # Debug: show which logs we're checking
    echo "DEBUG: Checking logs matching: $log_pattern" >&2
    ls -lh $log_pattern 2>/dev/null | awk '{print "  ", $NF}' >&2

    # Throughput - check all logs (might be in localhost/p1/p2)
    local throughput="N/A"
    for log in $log_pattern; do
        [ ! -f "$log" ] && continue
        local t=$(grep -oP 'agg_throughput:\s+\K[0-9.]+' "$log" 2>/dev/null | tail -1 || true)
        if [ -n "$t" ] && [ "$t" != "N/A" ]; then
            throughput="$t"
            echo "DEBUG: Found throughput $throughput in $(basename $log)" >&2
            break
        fi
    done

    # Network bytes - sum across all logs that report it
    local network_bytes=0
    local found_network=false
    for log in $log_pattern; do
        [ ! -f "$log" ] && continue
        # Try both "Final statistics" and periodic "Sent X logs" messages
        local nb=$(grep '\[Paxos Network\]' "$log" 2>/dev/null | grep -oP 'total bytes[: ]+\K[0-9]+' | tail -1 || true)
        if [ -n "$nb" ] && [ "$nb" -gt 0 ]; then
            network_bytes=$((network_bytes + nb))
            found_network=true
            echo "DEBUG: Found network bytes $nb in $(basename $log)" >&2
        fi
    done
    [ "$found_network" = false ] && network_bytes="N/A"

    # KDV compression metrics (only if KDV enabled) - check all logs
    local original_bytes="N/A"
    local encoded_bytes="N/A"
    local compression_ratio="N/A"

    if [ "$kdv_enabled" = "true" ]; then
        for log in $log_pattern; do
            [ ! -f "$log" ] && continue
            local ob=$(grep -oP 'Total original bytes:\s+\K[0-9]+' "$log" 2>/dev/null | tail -1 || true)
            local eb=$(grep -oP 'Total encoded bytes:\s+\K[0-9]+' "$log" 2>/dev/null | tail -1 || true)
            local cr=$(grep -oP 'Compression ratio:\s+\K[0-9.]+' "$log" 2>/dev/null | tail -1 || true)

            if [ -n "$ob" ] && [ "$ob" != "0" ]; then
                original_bytes="$ob"
                encoded_bytes="$eb"
                compression_ratio="$cr"
                echo "DEBUG: Found KDV stats in $(basename $log): orig=$ob encoded=$eb ratio=$cr" >&2
                break
            fi
        done
    fi

    # RocksDB disk usage
    local rocksdb_bytes
    rocksdb_bytes=$(du -sb /tmp/mako_rocksdb_shard* 2>/dev/null | awk '{sum+=$1} END {print sum}' || echo "N/A")
    [ -z "$rocksdb_bytes" ] && rocksdb_bytes="N/A"
    echo "DEBUG: RocksDB size: $rocksdb_bytes bytes" >&2

    echo "$throughput,$network_bytes,$original_bytes,$encoded_bytes,$compression_ratio,$rocksdb_bytes"
}

run_experiment() {
    local exp_name=$1
    local workload_mix=$2
    local record_size=$3
    local update_bytes=$4
    local kdv_enabled=$5
    local description=$6

    local mode="baseline"
    [ "$kdv_enabled" = "true" ] && mode="kdv"

    local full_name="${exp_name}_${mode}"

    echo ""
    echo "=========================================" >&2
    echo "Experiment: $full_name" >&2
    echo "Description: $description" >&2
    echo "Workload mix: $workload_mix" >&2
    echo "Record size: $record_size bytes" >&2
    echo "Update size: $update_bytes bytes" >&2
    echo "KDV enabled: $kdv_enabled" >&2
    echo "=========================================" >&2

    cleanup

    # Set KDV environment variable
    if [ "$kdv_enabled" = "true" ]; then
        export MAKO_ENABLE_KDV_LOGS=1
    else
        unset MAKO_ENABLE_KDV_LOGS
    fi

    # Start replicas
    # Note: Using smaller key space ($NUM_KEYS) to ensure repeated updates
    cd "$PROJECT_ROOT"

    echo "Starting 4 replicas..." >&2
    nohup bash bash/shard.sh 1 0 $THREADS localhost 0 1 ycsb -w $workload_mix -r $record_size -u $update_bytes -m middle -k $NUM_KEYS > kdv_eval_${full_name}_localhost.log 2>&1 &
    nohup bash bash/shard.sh 1 0 $THREADS learner 0 1 ycsb -w $workload_mix -r $record_size -u $update_bytes -m middle -k $NUM_KEYS > kdv_eval_${full_name}_learner.log 2>&1 &
    nohup bash bash/shard.sh 1 0 $THREADS p2 0 1 ycsb -w $workload_mix -r $record_size -u $update_bytes -m middle -k $NUM_KEYS > kdv_eval_${full_name}_p2.log 2>&1 &
    sleep 2
    nohup bash bash/shard.sh 1 0 $THREADS p1 0 1 ycsb -w $workload_mix -r $record_size -u $update_bytes -m middle -k $NUM_KEYS > kdv_eval_${full_name}_p1.log 2>&1 &
    local leader_pid=$!

    echo "Running for ${RUNTIME}s..." >&2
    sleep $RUNTIME

    echo "Stopping processes gracefully..." >&2
    # Send SIGTERM (not SIGKILL) to allow graceful shutdown and stats printing
    pkill -TERM dbtest 2>/dev/null || true

    # Wait for processes to shutdown and print final stats (up to 10 seconds)
    local waited=0
    while pgrep dbtest >/dev/null 2>&1 && [ $waited -lt 10 ]; do
        sleep 1
        waited=$((waited + 1))
    done

    # Force kill any remaining processes
    pkill -9 dbtest 2>/dev/null || true

    # Give time for final log writes
    sleep 2

    # Extract metrics from leader log
    local metrics
    metrics=$(extract_metrics "$full_name" "$kdv_enabled")

    # Parse workload mix
    local read_pct write_pct rmw_pct scan_pct
    IFS=',' read -r read_pct write_pct rmw_pct scan_pct <<< "$workload_mix"

    # Save detailed logs
    local exp_dir="$OUTPUT_DIR/$full_name"
    mkdir -p "$exp_dir"
    cp kdv_eval_${full_name}_*.log "$exp_dir/" 2>/dev/null || true

    echo "$exp_name,$mode,$read_pct,$write_pct,$rmw_pct,$record_size,$update_bytes,$NUM_KEYS,$metrics,\"$description\""
}

# Main evaluation
main() {
    # CSV header
    local header="experiment,mode,read_pct,rmw_pct,scan_pct,record_size,update_bytes,num_keys,throughput,network_bytes,original_bytes,encoded_bytes,compression_ratio_pct,rocksdb_bytes,description"
    local csv_file="$OUTPUT_DIR/results_${TIMESTAMP}.csv"
    echo "$header" > "$csv_file"
    echo "Results will be saved to: $csv_file"

    # Run each workload with baseline and KDV
    for workload_spec in "${WORKLOADS[@]}"; do
        IFS=':' read -r name mix record_size update_bytes desc <<< "$workload_spec"

        echo ""
        echo "========================================="
        echo "Testing: $name"
        echo "========================================="

        # Baseline (no KDV)
        result=$(run_experiment "$name" "$mix" "$record_size" "$update_bytes" "false" "$desc")
        echo "$result" >> "$csv_file"
        echo "Baseline: $result"

        sleep 2

        # KDV enabled
        result=$(run_experiment "$name" "$mix" "$record_size" "$update_bytes" "true" "$desc")
        echo "$result" >> "$csv_file"
        echo "KDV: $result"

        sleep 2
    done

    cleanup

    echo ""
    echo "========================================="
    echo "Evaluation Complete!"
    echo "========================================="
    echo "Results saved to: $csv_file"
    echo ""
    echo "Running analysis..."
    python3 "$SCRIPT_DIR/analyze_kdv_network_storage.py" "$csv_file"
}

cd "$PROJECT_ROOT"
main
