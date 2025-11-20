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
    echo "Cleaning up processes and RocksDB directories..."
    pkill -9 dbtest || true
    sleep 2
    rm -rf /tmp/mako_rocksdb_shard* || true
    rm -f test_1shard_replication_ycsb.sh_shard0-*.log || true
}

extract_metrics() {
    local log_file=$1
    local kdv_enabled=$2
    
    local throughput=$(grep "agg_throughput:" "$log_file" | tail -1 | awk '{print $2}')
    
    local latency=$(grep "avg_latency:" "$log_file" | tail -1 | awk '{print $2}' || echo "N/A")
    
    local paxos_bytes=$(grep "\[Paxos Network\] Final statistics: total bytes sent:" "$log_file" | tail -1 | awk '{print $7}')
    
    local original_bytes="N/A"
    local encoded_bytes="N/A"
    local compression_ratio="N/A"
    
    if [ "$kdv_enabled" = "true" ]; then
        original_bytes=$(grep "Total original bytes:" "$log_file" | tail -1 | awk '{print $4}')
        encoded_bytes=$(grep "Total encoded bytes:" "$log_file" | tail -1 | awk '{print $4}')
        compression_ratio=$(grep "Compression ratio:" "$log_file" | tail -1 | awk '{print $3}' | tr -d '%')
    fi
    
    local rocksdb_size="N/A"
    if [ -d "/tmp/mako_rocksdb_shard0_leader_pid"* ]; then
        local rocksdb_dir=$(ls -d /tmp/mako_rocksdb_shard0_leader_pid* 2>/dev/null | head -1)
        if [ -n "$rocksdb_dir" ]; then
            rocksdb_size=$(du -sb "$rocksdb_dir" 2>/dev/null | awk '{print $1}' || echo "N/A")
        fi
    fi
    
    echo "$throughput,$latency,$paxos_bytes,$original_bytes,$encoded_bytes,$compression_ratio,$rocksdb_size"
}

run_experiment() {
    local workload=$1
    local record_size=$2
    local update_config=$3
    local kdv_enabled=$4
    
    echo "========================================="
    echo "Running experiment:"
    echo "  Workload: $workload"
    echo "  Record size: $record_size bytes"
    echo "  Update config: $update_config"
    echo "  KDV enabled: $kdv_enabled"
    echo "========================================="
    
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

nohup bash bash/shard.sh 1 0 $THREADS localhost 0 1 ycsb -w $workload -r $record_size -u $update_bytes -m $update_mode > test_1shard_replication_ycsb.sh_shard0-localhost-$THREADS.log 2>&1 &
nohup bash bash/shard.sh 1 0 $THREADS learner 0 1 ycsb -w $workload -r $record_size -u $update_bytes -m $update_mode > test_1shard_replication_ycsb.sh_shard0-learner-$THREADS.log 2>&1 &
nohup bash bash/shard.sh 1 0 $THREADS p2 0 1 ycsb -w $workload -r $record_size -u $update_bytes -m $update_mode > test_1shard_replication_ycsb.sh_shard0-p2-$THREADS.log 2>&1 &
sleep 1
nohup bash bash/shard.sh 1 0 $THREADS p1 0 1 ycsb -w $workload -r $record_size -u $update_bytes -m $update_mode > test_1shard_replication_ycsb.sh_shard0-p1-$THREADS.log 2>&1 &

echo "Running experiment for $RUNTIME seconds..."
sleep $RUNTIME

echo "Stopping processes..."
pkill -9 dbtest || true
sleep 2
EOF
    
    chmod +x "$temp_script"
    bash "$temp_script"
    
    local log_file="$PROJECT_ROOT/test_1shard_replication_ycsb.sh_shard0-localhost-$THREADS.log"
    if [ ! -f "$log_file" ]; then
        echo "ERROR: Log file not found: $log_file"
        return 1
    fi
    
    local metrics=$(extract_metrics "$log_file" "$kdv_enabled")
    echo "$workload,$record_size,$update_bytes,$kdv_enabled,$metrics"
}

main() {
    echo "Starting YCSB KDV Replicated Evaluation"
    echo "Output will be saved to: $OUTPUT_CSV"
    
    echo "workload_mix,record_size,update_bytes,kdv_enabled,throughput,latency,paxos_bytes,original_bytes,encoded_bytes,compression_ratio,rocksdb_size" > "$OUTPUT_CSV"
    
    for workload in "${WORKLOAD_MIXES[@]}"; do
        for record_size in "${RECORD_SIZES[@]}"; do
            for update_config in "${UPDATE_CONFIGS[@]}"; do
                echo ""
                echo "========================================="
                echo "Baseline run (KDV disabled)"
                echo "========================================="
                result=$(run_experiment "$workload" "$record_size" "$update_config" "false")
                echo "$result" >> "$OUTPUT_CSV"
                
                echo ""
                echo "========================================="
                echo "KDV run (KDV enabled)"
                echo "========================================="
                result=$(run_experiment "$workload" "$record_size" "$update_config" "true")
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
