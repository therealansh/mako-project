#!/bin/bash
#
# Quick KDV Test - Run single experiment to verify metrics collection
#
# Usage: ./kdv_quick_test.sh [baseline|kdv]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MODE="${1:-kdv}"
RUNTIME=120  # Shorter runtime for quick test
THREADS=4
NUM_KEYS=10000

# Test workload: 90% RMW with small updates
WORKLOAD="10,0,90,0"
RECORD_SIZE=1024
UPDATE_BYTES=16

echo "========================================="
echo "Quick KDV Test"
echo "========================================="
echo "Mode: $MODE"
echo "Workload: $WORKLOAD (10% read, 90% RMW)"
echo "Record size: $RECORD_SIZE bytes"
echo "Update size: $UPDATE_BYTES bytes"
echo "Runtime: ${RUNTIME}s"
echo "========================================="

cleanup() {
    echo "Cleaning up..." >&2
    pkill -9 dbtest || true
    sleep 2
    rm -rf /tmp/mako_rocksdb_shard* || true
}

cleanup

# Set KDV mode
if [ "$MODE" = "kdv" ]; then
    export MAKO_ENABLE_KDV_LOGS=1
    echo "KDV: ENABLED"
else
    unset MAKO_ENABLE_KDV_LOGS
    echo "KDV: DISABLED (baseline)"
fi

cd "$PROJECT_ROOT"

echo ""
echo "Starting 4 replicas..."
nohup bash bash/shard.sh 1 0 $THREADS localhost 0 1 ycsb -w $WORKLOAD -r $RECORD_SIZE -u $UPDATE_BYTES -m middle -k $NUM_KEYS > test_localhost.log 2>&1 &
nohup bash bash/shard.sh 1 0 $THREADS learner 0 1 ycsb -w $WORKLOAD -r $RECORD_SIZE -u $UPDATE_BYTES -m middle -k $NUM_KEYS > test_learner.log 2>&1 &
nohup bash bash/shard.sh 1 0 $THREADS p2 0 1 ycsb -w $WORKLOAD -r $RECORD_SIZE -u $UPDATE_BYTES -m middle -k $NUM_KEYS > test_p2.log 2>&1 &
sleep 2
nohup bash bash/shard.sh 1 0 $THREADS p1 0 1 ycsb -w $WORKLOAD -r $RECORD_SIZE -u $UPDATE_BYTES -m middle -k $NUM_KEYS > test_p1.log 2>&1 &

echo "Running for ${RUNTIME}s..."
sleep $RUNTIME

echo "Stopping processes gracefully..."
pkill -TERM dbtest 2>/dev/null || true

# Wait for graceful shutdown
waited=0
while pgrep dbtest >/dev/null 2>&1 && [ $waited -lt 10 ]; do
    echo "  Waiting for shutdown... ${waited}s"
    sleep 1
    waited=$((waited + 1))
done

# Force kill if needed
pkill -9 dbtest 2>/dev/null || true
sleep 2

echo ""
echo "========================================="
echo "Checking for metrics in log files..."
echo "========================================="

for log in test_*.log; do
    [ ! -f "$log" ] && continue
    echo ""
    echo "=== $log ==="

    # Check for throughput
    if grep -q "agg_throughput" "$log" 2>/dev/null; then
        echo "✓ Throughput found:"
        grep "agg_throughput" "$log" | tail -3
    else
        echo "✗ No throughput metrics"
    fi

    # Check for Paxos network
    if grep -q "\[Paxos Network\]" "$log" 2>/dev/null; then
        echo "✓ Paxos Network found:"
        grep "\[Paxos Network\]" "$log" | tail -3
    else
        echo "✗ No Paxos Network metrics"
    fi

    # Check for KDV stats
    if grep -q "Total original bytes\|Total encoded bytes\|Compression ratio" "$log" 2>/dev/null; then
        echo "✓ KDV stats found:"
        grep -E "Total original bytes|Total encoded bytes|Compression ratio" "$log" | tail -3
    else
        echo "✗ No KDV stats"
    fi

    echo ""
done

# Check RocksDB size
echo "========================================="
echo "RocksDB disk usage:"
du -sh /tmp/mako_rocksdb_shard* 2>/dev/null || echo "No RocksDB directories found"
echo "========================================="

echo ""
echo "Log files saved as test_*.log"
echo "To view full log: less test_localhost.log"
echo ""
echo "To run again:"
echo "  Baseline: ./scripts/kdv_quick_test.sh baseline"
echo "  KDV:      ./scripts/kdv_quick_test.sh kdv"
