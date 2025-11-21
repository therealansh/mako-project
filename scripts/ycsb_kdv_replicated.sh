#!/bin/bash
#
# 
#
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_CSV="${1:-ycsb_kdv_evaluation.csv}"
RUNTIME=30  # seconds per run
THREADS=6

WORKLOAD_MIXES=(
    "95,0,5,0"   # Read-heavy with small RMW
    "50,0,50,0"  # Balanced read/RMW
    "20,0,80,0"  # Write-heavy
)

RECORD_SIZES=(100 1024 4096)

UPDATE_CONFIGS=(
    "small:16"
    "half"
    "large"
)

cleanup() {
    echo "Cleaning up processes and RocksDB directories..." >&2
    pkill -9 dbtest || true
    sleep 2
    rm -rf /tmp/mako_rocksdb_shard* || true
    rm -f test_1shard_replication_ycsb.sh_shard0-*.log || true
}

extract_metrics() {
    local log_file=$1
    local kdv_enabled=$2

    # Throughput (ops/sec)
    local throughput
    throughput=$(grep -oP 'agg_throughput:\s+\K[0-9.]+' "$log_file" 2>/dev/null | tail -1 || true)
    if [ -z "$throughput" ]; then
        throughput="N/A"
    fi

    # Average latency (ms)
    local latency
    latency=$(grep -oP 'avg_latency:\s+\K[0-9.]+' "$log_file" 2>/dev/null | tail -1 || true)
    if [ -z "$latency" ]; then
        latency="N/A"
    fi

    # Paxos network bytes
    local paxos_bytes
    paxos_bytes=$(grep -oP '\[Paxos Network\] Final statistics: total bytes sent:\s+\K[0-9]+' "$log_file" 2>/dev/null | tail -1 || true)
    if [ -z "$paxos_bytes" ]; then
        paxos_bytes="N/A"
    fi

    # KDV-specific stats (only when enabled and data exists)
    local original_bytes="N/A"
    local encoded_bytes="N/A"
    local compression_ratio="N/A"

    if [ "$kdv_enabled" = "true" ]; then
        local tmp

        tmp=$(grep -oP 'Total original bytes:\s+\K[0-9]+' "$log_file" 2>/dev/null | tail -1 || true)
        if [ -n "$tmp" ]; then
            original_bytes="$tmp"
        fi

        tmp=$(grep -oP 'Total encoded bytes:\s+\K[0-9]+' "$log_file" 2>/dev/null | tail -1 || true)
        if [ -n "$tmp" ]; then
            encoded_bytes="$tmp"
        fi

        tmp=$(grep -oP 'Compression ratio:\s+\K[-0-9.]+' "$log_file" 2>/dev/null | tail -1 || true)
        if [ -n "$tmp" ]; then
            # Stored as percentage value without the '%' sign
            compression_ratio="$tmp"
        fi
    fi

    # RocksDB total disk usage across shard directories (leader + partitions)
    local rocksdb_size="N/A"
    local rocksdb_bytes
    rocksdb_bytes=$(du -sb /tmp/mako_rocksdb_shard* 2>/dev/null | awk '{sum+=$1} END {print sum}' || true)
    if [ -n "$rocksdb_bytes" ] && [ "$rocksdb_bytes" != "0" ]; then
        rocksdb_size="$rocksdb_bytes"
    fi

    echo "$throughput,$latency,$paxos_bytes,$original_bytes,$encoded_bytes,$compression_ratio,$rocksdb_size"
}

run_experiment() {
    local workload=$1
    local record_size=$2
    local update_config=$3
    local kdv_enabled=$4
    
    echo "=========================================" >&2
    echo "Running experiment:" >&2
    echo "  Workload: $workload" >&2
    echo "  Record size: $record_size bytes" >&2
    echo "  Update config: $update_config" >&2
    echo "  KDV enabled: $kdv_enabled" >&2
    echo "=========================================" >&2
    
    local update_bytes=""
    local update_mode="middle"
    
    if [ "$update_config" = "small:16" ]; then
        update_bytes="16"
    elif [ "$update_config" = "half" ]; then
        update_bytes=$((record_size / 2))
    elif [ "$update_config" = "large" ]; then
        update_bytes=$((record_size * 9 / 10))
    else
        update_bytes="0"  # Full update
    fi
    
    cleanup
    
    if [ "$kdv_enabled" = "true" ]; then
        export MAKO_ENABLE_KDV_LOGS=1
    else
        unset MAKO_ENABLE_KDV_LOGS
    fi
    
    local temp_script="/tmp/run_ycsb_experiment.sh"
    cat > "$temp_script" << EOF
#!/bin/bash
cd "$PROJECT_ROOT"

echo "Starting 4 replicas (localhost, learner, p2, p1)..." >&2
nohup bash bash/shard.sh 1 0 $THREADS localhost 0 1 ycsb -w $workload -r $record_size -u $update_bytes -m $update_mode > test_1shard_replication_ycsb.sh_shard0-localhost-$THREADS.log 2>&1 &
nohup bash bash/shard.sh 1 0 $THREADS learner 0 1 ycsb -w $workload -r $record_size -u $update_bytes -m $update_mode > test_1shard_replication_ycsb.sh_shard0-learner-$THREADS.log 2>&1 &
nohup bash bash/shard.sh 1 0 $THREADS p2 0 1 ycsb -w $workload -r $record_size -u $update_bytes -m $update_mode > test_1shard_replication_ycsb.sh_shard0-p2-$THREADS.log 2>&1 &
sleep 1
nohup bash bash/shard.sh 1 0 $THREADS p1 0 1 ycsb -w $workload -r $record_size -u $update_bytes -m $update_mode > test_1shard_replication_ycsb.sh_shard0-p1-$THREADS.log 2>&1 &
SHARD0_PID=\$!

echo "Running experiment for $RUNTIME seconds..." >&2
sleep $RUNTIME

echo "Stopping leader (p1)..." >&2
kill "\$SHARD0_PID" 2>/dev/null || true
wait "\$SHARD0_PID" 2>/dev/null || true

echo "Experiment complete, extracting metrics..." >&2
EOF
    
    chmod +x "$temp_script"
    bash "$temp_script"

    local log_file="$PROJECT_ROOT/test_1shard_replication_ycsb.sh_shard0-localhost-$THREADS.log"
    if [ ! -f "$log_file" ]; then
        echo "ERROR: Log file not found: $log_file" >&2
        return 1
    fi

    # Give the benchmark a bit of time to flush final stats into the log
    local waited=0
    local max_wait=30
    while [ $waited -lt $max_wait ]; do
        if grep -q "agg_throughput:" "$log_file" 2>/dev/null; then
            break
        fi
        sleep 1
        waited=$((waited + 1))
    done

    local metrics
    metrics=$(extract_metrics "$log_file" "$kdv_enabled")

    # Split workload mix into separate columns for a clean CSV
    local read_pct write_pct rmw_pct scan_pct
    IFS=',' read -r read_pct write_pct rmw_pct scan_pct <<< "$workload"

    echo "$read_pct,$write_pct,$rmw_pct,$scan_pct,$record_size,$update_bytes,$kdv_enabled,$metrics"
}

main() {
    echo "Starting YCSB KDV Replicated Evaluation"
    echo "Output will be saved to: $OUTPUT_CSV"

    # CSV header: Split workload mix into explicit columns for readability
    local header="read_pct,write_pct,rmw_pct,scan_pct,record_size,update_bytes,kdv_enabled,throughput,latency,paxos_bytes,original_bytes,encoded_bytes,compression_ratio,rocksdb_size"
    echo "$header" > "$OUTPUT_CSV"
    echo "CSV columns: $header"
    
    for workload in "${WORKLOAD_MIXES[@]}"; do
        for record_size in "${RECORD_SIZES[@]}"; do
            for update_config in "${UPDATE_CONFIGS[@]}"; do
                echo ""
                echo "========================================="
                echo "Baseline run (KDV disabled)"
                echo "========================================="
                result=$(run_experiment "$workload" "$record_size" "$update_config" "false")
                echo "Baseline result: $result"
                echo "$result" >> "$OUTPUT_CSV"
                
                echo ""
                echo "========================================="
                echo "KDV run (KDV enabled)"
                echo "========================================="
                result=$(run_experiment "$workload" "$record_size" "$update_config" "true")
                echo "KDV result:      $result"
                echo "$result" >> "$OUTPUT_CSV"
                
                sleep 2
            done
        done
    done
    
    echo ""
    echo "========================================="
    echo "Evaluation complete!"
    echo "Results saved to: $OUTPUT_CSV"
    echo "========================================="
    
    cleanup
    
    echo ""
    echo "Summary of results:"
    column -t -s',' "$OUTPUT_CSV" | head -20
}

cd "$PROJECT_ROOT"
main
