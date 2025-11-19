#!/bin/bash

set -euo pipefail
shopt -s nullglob

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== KDV Evaluation Script ===${NC}"
echo "This script will evaluate KDV compression by running baseline and KDV-enabled tests"
echo ""

cd "$(dirname "$0")/.."
mkdir -p results/kdv_experiments

export PATH="$HOME/.cargo/bin:$PATH"

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

echo -e "${YELLOW}Step 1: Verify KDV toggle works${NC}"
echo "Testing with MAKO_ENABLE_KDV_LOGS=1..."
export MAKO_ENABLE_KDV_LOGS=1
if ./build/simpleTransaction 2>&1 | grep -q "KDV logs enabled"; then
    echo -e "${GREEN}✓ KDV toggle works!${NC}"
else
    echo -e "${YELLOW}⚠ KDV toggle message not found (may be normal for short tests)${NC}"
fi
echo ""

echo -e "${YELLOW}Step 2: Run unit tests${NC}"
if [ -f ./build/kdv_format_test ]; then
    ./build/kdv_format_test
    echo -e "${GREEN}✓ Unit tests passed!${NC}"
else
    echo -e "${RED}✗ kdv_format_test not found. Run 'make' first.${NC}"
    exit 1
fi
echo ""

echo -e "${YELLOW}Step 3: Baseline test (KDV disabled)${NC}"
unset MAKO_ENABLE_KDV_LOGS
rm -rf /tmp/mako_rocksdb_shard* 2>/dev/null || true
echo "Running shard1Replication test (baseline)..."
./ci/ci.sh shard1Replication 2>&1 | tee results/kdv_experiments/baseline.log
echo "Measuring disk usage..."
measure_disk results/kdv_experiments/baseline_disk.txt
echo "Archiving baseline log files..."
mkdir -p results/kdv_experiments/baseline_logs
cp -f test_1shard_replication.sh_*.log results/kdv_experiments/baseline_logs/ 2>/dev/null || true
echo -e "${GREEN}✓ Baseline test complete${NC}"
echo ""

echo -e "${YELLOW}Step 4: KDV test (KDV enabled)${NC}"
export MAKO_ENABLE_KDV_LOGS=1
rm -rf /tmp/mako_rocksdb_shard* 2>/dev/null || true
echo "Running shard1Replication test (KDV enabled)..."
./ci/ci.sh shard1Replication 2>&1 | tee results/kdv_experiments/kdv.log
echo "Measuring disk usage..."
measure_disk results/kdv_experiments/kdv_disk.txt
echo "Archiving KDV log files..."
mkdir -p results/kdv_experiments/kdv_logs
cp -f test_1shard_replication.sh_*.log results/kdv_experiments/kdv_logs/ 2>/dev/null || true
echo -e "${GREEN}✓ KDV test complete${NC}"
echo ""

echo -e "${YELLOW}Step 4b: RocksDB KDV compaction (offline)${NC}"
if [ -x ./build/kdv_compaction_tool ]; then
    echo "Running KDV compaction tool on RocksDB data..."
    set +e
    ./build/kdv_compaction_tool 2>&1 | tee results/kdv_experiments/kdv_compaction.log
    compaction_rc=$?
    set -e
    if [ "$compaction_rc" -eq 0 ]; then
        echo "Measuring disk usage after compaction..."
        measure_disk results/kdv_experiments/kdv_disk_compacted.txt
        echo -e "${GREEN}✓ KDV compaction complete${NC}"
    else
        echo -e "${YELLOW}⚠ KDV compaction tool failed (see results/kdv_experiments/kdv_compaction.log), skipping post-compaction measurement${NC}"
    fi
else
    echo -e "${YELLOW}⚠ kdv_compaction_tool not found (build ./build/kdv_compaction_tool to enable RocksDB KDV compaction)${NC}"
fi
echo ""

echo -e "${YELLOW}Step 5: Analyze results${NC}"
echo ""

baseline_logs=(results/kdv_experiments/baseline.log results/kdv_experiments/baseline_logs/*.log)
kdv_logs=(results/kdv_experiments/kdv.log results/kdv_experiments/kdv_logs/*.log)

baseline_network_bytes=$(grep -h "\[Paxos Network\] Final statistics: total bytes sent:" "${baseline_logs[@]}" 2>/dev/null | grep -oP 'total bytes sent: \K[0-9]+' | head -1 || echo "0")
kdv_network_bytes=$(grep -h "\[Paxos Network\] Final statistics: total bytes sent:" "${kdv_logs[@]}" 2>/dev/null | grep -oP 'total bytes sent: \K[0-9]+' | head -1 || echo "0")

echo "=== Network Compression Analysis ==="
if [[ "$baseline_network_bytes" -gt 0 && "$kdv_network_bytes" -gt 0 ]]; then
    baseline_mb=$(echo "scale=2; $baseline_network_bytes / 1024 / 1024" | bc)
    kdv_mb=$(echo "scale=2; $kdv_network_bytes / 1024 / 1024" | bc)
    compression_pct=$(echo "scale=2; (1 - $kdv_network_bytes / $baseline_network_bytes) * 100" | bc)
    
    echo "Baseline network bytes: $baseline_network_bytes ($baseline_mb MB)"
    echo "KDV network bytes: $kdv_network_bytes ($kdv_mb MB)"
    if (( $(echo "$compression_pct > 0" | bc -l) )); then
        echo -e "${GREEN}Network compression: ${compression_pct}%${NC}"
    else
        echo -e "${YELLOW}Network compression: ${compression_pct}% (KDV increased network usage)${NC}"
    fi
else
    echo -e "${YELLOW}⚠ Could not calculate network compression (missing Paxos network stats)${NC}"
    echo "Baseline bytes: $baseline_network_bytes"
    echo "KDV bytes: $kdv_network_bytes"
fi
echo ""

echo "=== Disk Usage Comparison ==="
echo "Baseline:"
cat results/kdv_experiments/baseline_disk.txt 2>/dev/null || echo "No baseline disk data"
echo ""
echo "KDV:"
cat results/kdv_experiments/kdv_disk.txt 2>/dev/null || echo "No KDV disk data"
if [ -f results/kdv_experiments/kdv_disk_compacted.txt ]; then
    echo ""
    echo "KDV (after KDV compaction):"
    cat results/kdv_experiments/kdv_disk_compacted.txt 2>/dev/null || echo "No KDV compaction disk data"
fi
echo ""

echo "=== KDV Statistics ==="
if ((${#kdv_logs[@]} > 0)); then
    echo "Compression stats:"
    grep -H "RocksDB KDV Compression Statistics" "${kdv_logs[@]}" 2>/dev/null || echo "No RocksDB KDV stats found"
    grep -H "Total original bytes:" "${kdv_logs[@]}" 2>/dev/null || true
    grep -H "Total encoded bytes:" "${kdv_logs[@]}" 2>/dev/null || true
    grep -H "Compression ratio:" "${kdv_logs[@]}" 2>/dev/null || true
    grep -H "Disk savings:" "${kdv_logs[@]}" 2>/dev/null || true
    echo ""
    echo "Network stats:"
    grep -H "\[Paxos Network\] Final" "${kdv_logs[@]}" 2>/dev/null || echo "No Paxos network stats found"
    echo ""
    echo "KDV encode/decode stats:"
    grep -H "\[KDV Encode\]" "${kdv_logs[@]}" 2>/dev/null | tail -5 || echo "No KDV encode stats found"
    grep -H "\[KDV Decode\]" "${kdv_logs[@]}" 2>/dev/null | tail -5 || echo "No KDV decode stats found"
else
    echo "No log files found to analyze"
fi
echo ""

echo -e "${GREEN}=== Evaluation Complete ===${NC}"
echo "Results saved to results/kdv_experiments/"
echo ""
echo "To calculate compression ratio:"
echo "  baseline_bytes=\$(cat results/kdv_experiments/baseline_disk.txt | awk '{sum+=\$1} END {print sum}')"
echo "  kdv_bytes=\$(cat results/kdv_experiments/kdv_disk.txt | awk '{sum+=\$1} END {print sum}')"
echo "  echo \"Compression: \$(echo \"scale=2; (1 - \$kdv_bytes / \$baseline_bytes) * 100\" | bc)%\""
