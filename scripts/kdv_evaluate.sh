#!/bin/bash

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== KDV Evaluation Script ===${NC}"
echo "This script will evaluate KDV compression by running baseline and KDV-enabled tests"
echo ""

mkdir -p results/kdv_experiments
cd "$(dirname "$0")/.."

export PATH="$HOME/.cargo/bin:$PATH"

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
rm -rf /tmp/*_mako_rocksdb_shard* 2>/dev/null || true
echo "Running shard1Replication test (baseline)..."
./ci/ci.sh shard1Replication 2>&1 | tee results/kdv_experiments/baseline.log
echo "Measuring disk usage..."
du -sb /tmp/*_mako_rocksdb_shard* 2>/dev/null | tee results/kdv_experiments/baseline_disk.txt || echo "No RocksDB data found"
echo -e "${GREEN}✓ Baseline test complete${NC}"
echo ""

echo -e "${YELLOW}Step 4: KDV test (KDV enabled)${NC}"
export MAKO_ENABLE_KDV_LOGS=1
rm -rf /tmp/*_mako_rocksdb_shard* 2>/dev/null || true
echo "Running shard1Replication test (KDV enabled)..."
./ci/ci.sh shard1Replication 2>&1 | tee results/kdv_experiments/kdv.log
echo "Measuring disk usage..."
du -sb /tmp/*_mako_rocksdb_shard* 2>/dev/null | tee results/kdv_experiments/kdv_disk.txt || echo "No RocksDB data found"
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
echo "Compression stats:"
grep "\[RocksDB KDV Stats\]" results/kdv_experiments/kdv.log 2>/dev/null || echo "No RocksDB KDV stats found"
echo ""
echo "Network stats:"
grep "\[Paxos Network\] Final" results/kdv_experiments/kdv.log 2>/dev/null || echo "No Paxos network stats found"
echo ""
echo "KDV encode/decode stats:"
grep "\[KDV Encode\]" results/kdv_experiments/kdv.log 2>/dev/null | tail -5 || echo "No KDV encode stats found"
grep "\[KDV Decode\]" results/kdv_experiments/kdv.log 2>/dev/null | tail -5 || echo "No KDV decode stats found"
echo ""

echo -e "${GREEN}=== Evaluation Complete ===${NC}"
echo "Results saved to results/kdv_experiments/"
echo ""
echo "To calculate compression ratio:"
echo "  baseline_bytes=\$(cat results/kdv_experiments/baseline_disk.txt | awk '{sum+=\$1} END {print sum}')"
echo "  kdv_bytes=\$(cat results/kdv_experiments/kdv_disk.txt | awk '{sum+=\$1} END {print sum}')"
echo "  echo \"Compression: \$(echo \"scale=2; (1 - \$kdv_bytes / \$baseline_bytes) * 100\" | bc)%\""
