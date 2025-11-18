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

echo -e "${YELLOW}Step 5: Analyze results${NC}"
echo ""
echo "=== Disk Usage Comparison ==="
echo "Baseline:"
cat results/kdv_experiments/baseline_disk.txt 2>/dev/null || echo "No baseline disk data"
echo ""
echo "KDV:"
cat results/kdv_experiments/kdv_disk.txt 2>/dev/null || echo "No KDV disk data"
echo ""

echo "=== KDV Statistics ==="
logs=(results/kdv_experiments/kdv.log results/kdv_experiments/kdv_logs/*.log)
if ((${#logs[@]} > 0)); then
    echo "Compression stats:"
    grep -H "RocksDB KDV Compression Statistics" "${logs[@]}" 2>/dev/null || echo "No RocksDB KDV stats found"
    grep -H "Total original bytes:" "${logs[@]}" 2>/dev/null || true
    grep -H "Total encoded bytes:" "${logs[@]}" 2>/dev/null || true
    grep -H "Compression ratio:" "${logs[@]}" 2>/dev/null || true
    grep -H "Disk savings:" "${logs[@]}" 2>/dev/null || true
    echo ""
    echo "Network stats:"
    grep -H "\[Paxos Network\] Final" "${logs[@]}" 2>/dev/null || echo "No Paxos network stats found"
    echo ""
    echo "KDV encode/decode stats:"
    grep -H "\[KDV Encode\]" "${logs[@]}" 2>/dev/null | tail -5 || echo "No KDV encode stats found"
    grep -H "\[KDV Decode\]" "${logs[@]}" 2>/dev/null | tail -5 || echo "No KDV decode stats found"
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
