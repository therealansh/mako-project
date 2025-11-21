#!/bin/bash

set -euo pipefail
shopt -s nullglob

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== KDV YCSB Replicated Evaluation ===${NC}"
echo "Runs YCSB in replicated (Paxos + RocksDB) mode to measure:"
echo "- Throughput and latency"
echo "- Paxos network bytes (cross-datacenter bandwidth proxy)"
echo "- RocksDB KDV disk savings (original vs encoded bytes)"
echo ""

cd "$(dirname "$0")/.."

RESULTS_DIR="results/kdv_ycsb_replicated"
mkdir -p "$RESULTS_DIR"

THREADS="${THREADS:-4}"
RUNTIME="${RUNTIME:-120}"

# YCSB workload parameters (can be overridden via env)
YCSB_WORKLOAD_MIX="${YCSB_WORKLOAD_MIX:-50,0,50,0}"   # R,W,RMW,Scan
YCSB_RECORD_SIZE="${YCSB_RECORD_SIZE:-1024}"          # bytes
YCSB_UPDATE_BYTES="${YCSB_UPDATE_BYTES:-16}"          # small updates
YCSB_UPDATE_MODE="${YCSB_UPDATE_MODE:-middle}"        # prefix|middle|suffix

export PATH="$HOME/.cargo/bin:$PATH"

cleanup_processes() {
    echo -e "${YELLOW}Cleaning up any lingering test processes...${NC}"
    pkill -9 -f dbtest 2>/dev/null || true
    pkill -9 -f simplePaxos 2>/dev/null || true
    sleep 3
    rm -rf /tmp/mako_rocksdb_shard* /tmp/*_mako_rocksdb_shard* 2>/dev/null || true
    echo "Cleanup complete."
}

measure_disk() {
    local out="$1"
    local dirs=(/tmp/mako_rocksdb_shard*)
    if ((${#dirs[@]} == 0)); then
        mapfile -t dirs < <(find /tmp -maxdepth 1 -type d -iname 'mako_rocksdb_shard*' 2>/dev/null || true)
    fi
    if ((${#dirs[@]} > 0)); then
        du -sb "${dirs[@]}" | tee "$out"
    else
        echo "No RocksDB data found" | tee "$out"
    fi
}

run_ycsb_shard1_replication() {
    local mode="$1"         # baseline | kdv
    local trd="$THREADS"
    local script_tag="ycsb_${mode}"

    echo -e "${YELLOW}Running YCSB shard1Replication (${mode})...${NC}"
    cleanup_processes

    if [ "$mode" = "kdv" ]; then
        export MAKO_ENABLE_KDV_LOGS=1
    else
        unset MAKO_ENABLE_KDV_LOGS
    fi

    rm -rf /tmp/mako_rocksdb_shard* /tmp/*_mako_rocksdb_shard* 2>/dev/null || true

    local path
    path="$(pwd)/src/mako"

    # Common YCSB args
    local ycsb_args=(
        --bench ycsb
        --num-threads "$trd"
        --is-replicated
        -w "$YCSB_WORKLOAD_MIX"
        -r "$YCSB_RECORD_SIZE"
        -u "$YCSB_UPDATE_BYTES"
        -m "$YCSB_UPDATE_MODE"
    )

    echo "Starting shard 0 (leader + followers) with YCSB..."

    # localhost: leader
    nohup ./dbtest \
        --shard-index 0 \
        --shard-config "$path/config/local-shards1-warehouses${trd}.yml" \
        -F config/1leader_2followers/paxos${trd}_shardidx0.yml \
        -F config/occ_paxos.yml \
        -P localhost \
        "${ycsb_args[@]}" \
        > "${RESULTS_DIR}/${script_tag}_shard0-localhost-${trd}.log" 2>&1 &

    # learner
    nohup ./dbtest \
        --shard-index 0 \
        --shard-config "$path/config/local-shards1-warehouses${trd}.yml" \
        -F config/1leader_2followers/paxos${trd}_shardidx0.yml \
        -F config/occ_paxos.yml \
        -P learner \
        "${ycsb_args[@]}" \
        > "${RESULTS_DIR}/${script_tag}_shard0-learner-${trd}.log" 2>&1 &

    # p2
    nohup ./dbtest \
        --shard-index 0 \
        --shard-config "$path/config/local-shards1-warehouses${trd}.yml" \
        -F config/1leader_2followers/paxos${trd}_shardidx0.yml \
        -F config/occ_paxos.yml \
        -P p2 \
        "${ycsb_args[@]}" \
        > "${RESULTS_DIR}/${script_tag}_shard0-p2-${trd}.log" 2>&1 &

    sleep 1

    # p1 (start last; use its PID as leader handle)
    nohup ./dbtest \
        --shard-index 0 \
        --shard-config "$path/config/local-shards1-warehouses${trd}.yml" \
        -F config/1leader_2followers/paxos${trd}_shardidx0.yml \
        -F config/occ_paxos.yml \
        -P p1 \
        "${ycsb_args[@]}" \
        > "${RESULTS_DIR}/${script_tag}_shard0-p1-${trd}.log" 2>&1 &
    local leader_pid=$!

    sleep 2

    echo "Running YCSB workload for ${RUNTIME}s..."
    sleep "$RUNTIME"

    echo "Stopping leader (p1)..."
    kill "$leader_pid" 2>/dev/null || true
    wait "$leader_pid" 2>/dev/null || true

    echo "Measuring disk usage..."
    measure_disk "${RESULTS_DIR}/${mode}_disk.txt"

    echo -e "${GREEN}YCSB ${mode} run complete${NC}"
}

echo -e "${YELLOW}Step 1: Baseline run (KDV disabled)${NC}"
run_ycsb_shard1_replication baseline
echo ""

echo -e "${YELLOW}Step 2: KDV run (KDV enabled)${NC}"
run_ycsb_shard1_replication kdv
echo ""

echo -e "${YELLOW}Step 3: Analyze results${NC}"

baseline_logs=( "${RESULTS_DIR}/ycsb_baseline_shard0-localhost-${THREADS}.log" )
kdv_logs=( "${RESULTS_DIR}/ycsb_kdv_shard0-localhost-${THREADS}.log" )

baseline_network_bytes=$(grep -h "\[Paxos Network\] Final statistics: total bytes sent:" "${baseline_logs[@]}" 2>/dev/null | grep -oP 'total bytes sent: \K[0-9]+' | head -1 || echo "0")
kdv_network_bytes=$(grep -h "\[Paxos Network\] Final statistics: total bytes sent:" "${kdv_logs[@]}" 2>/dev/null | grep -oP 'total bytes sent: \K[0-9]+' | head -1 || echo "0")

echo "=== YCSB Network Compression (Paxos) ==="
if [[ "$baseline_network_bytes" -gt 0 && "$kdv_network_bytes" -gt 0 ]]; then
    baseline_mb=$(echo "scale=2; $baseline_network_bytes / 1024 / 1024" | bc)
    kdv_mb=$(echo "scale=2; $kdv_network_bytes / 1024 / 1024" | bc)
    compression_pct=$(echo "scale=2; (1 - $kdv_network_bytes / $baseline_network_bytes) * 100" | bc)
    
    echo "Baseline network bytes: $baseline_network_bytes ($baseline_mb MB)"
    echo "KDV network bytes:      $kdv_network_bytes ($kdv_mb MB)"
    echo "Network savings:        ${compression_pct}%"
else
    echo "Could not compute network savings (missing Paxos stats)"
    echo "Baseline bytes: $baseline_network_bytes"
    echo "KDV bytes:      $kdv_network_bytes"
fi
echo ""

echo "=== YCSB Disk Usage Comparison ==="
echo "Baseline RocksDB:"
cat "${RESULTS_DIR}/baseline_disk.txt" 2>/dev/null || echo "No baseline disk data"
echo ""
echo "KDV RocksDB:"
cat "${RESULTS_DIR}/kdv_disk.txt" 2>/dev/null || echo "No KDV disk data"
echo ""

echo "=== YCSB RocksDB KDV Statistics (leader log) ==="
grep -H "RocksDB KDV Compression Statistics" "${kdv_logs[@]}" 2>/dev/null || echo "No RocksDB KDV stats found"
grep -H "Total original bytes:" "${kdv_logs[@]}" 2>/dev/null || true
grep -H "Total encoded bytes:" "${kdv_logs[@]}" 2>/dev/null || true
grep -H "Compression ratio:" "${kdv_logs[@]}" 2>/dev/null || true
grep -H "Disk savings:" "${kdv_logs[@]}" 2>/dev/null || true
echo ""

echo -e "${GREEN}=== YCSB Replicated KDV Evaluation Complete ===${NC}"
echo "Results saved under: $RESULTS_DIR"
