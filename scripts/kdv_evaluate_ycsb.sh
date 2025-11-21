#!/bin/bash

set -euo pipefail
shopt -s nullglob

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== KDV YCSB-Style Evaluation Script ===${NC}"
echo "This script evaluates KDV using a YCSB-style small-update workload"
echo "based on the kdv_synthetic_bench microbenchmark."
echo ""

cd "$(dirname "$0")/.."
mkdir -p results/kdv_ycsb_experiments

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

echo -e "${YELLOW}Step 1: Build kdv_synthetic_bench${NC}"
if [ ! -x ./build/kdv_synthetic_bench ]; then
    echo "Building kdv_synthetic_bench..."
    cmake -S . -B build
    cmake --build build --target kdv_synthetic_bench -j$(nproc)
fi
echo -e "${GREEN}✓ kdv_synthetic_bench built${NC}"
echo ""

echo -e "${YELLOW}Step 2: Run kdv_synthetic_bench (YCSB-style small updates)${NC}"
log_file="results/kdv_ycsb_experiments/kdv_synthetic_ycsb.log"
./build/kdv_synthetic_bench 2>&1 | tee "$log_file"
echo -e "${GREEN}✓ Synthetic YCSB-style KDV experiment complete${NC}"
echo ""

echo -e "${YELLOW}Step 3: Summarize synthetic results${NC}"
echo ""

echo "=== Synthetic YCSB-style KDV Metrics ==="
grep -E 'Payload size:|Updates:|Delta region:|Total original bytes:|Total encoded bytes:|Compression ratio:|Bandwidth reduction:' "$log_file" || echo "No synthetic metrics found"
echo ""

echo "=== Notes ==="
echo "This synthetic benchmark uses 1KB records with a 16-byte delta region"
echo "to approximate a YCSB small-update workload and reports logical"
echo "compression metrics (original vs encoded bytes) directly from the KDV layer."
echo ""

echo -e "${GREEN}=== YCSB-Style KDV Evaluation Complete ===${NC}"
echo "Results saved to results/kdv_ycsb_experiments/"
