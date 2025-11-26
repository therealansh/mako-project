#!/bin/bash

set -euo pipefail
shopt -s nullglob

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== KDV YCSB-Style Evaluation Script ===${NC}"
echo "This script evaluates KDV using YCSB-style synthetic workloads"
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

echo -e "${YELLOW}Step 2: Run kdv_synthetic_bench for multiple YCSB-style modes${NC}"

# Canonical single-mode log (kept for backwards compatibility: 1KB, 16-byte delta)
primary_log="results/kdv_ycsb_experiments/kdv_synthetic_ycsb.log"
./build/kdv_synthetic_bench 1024 16 1000 2>&1 | tee "$primary_log"

# Additional modes for a simple YCSB-style grid.
# Each entry: label:payload_size:delta_region:num_updates
declare -a SYNTHETIC_MODES=(
    "small_1kb:1024:16:1000"    # 1KB record, 16B delta (small update)
    "medium_1kb:1024:512:1000"  # 1KB record, 512B delta (50% update)
    "large_1kb:1024:921:1000"   # 1KB record, ~90% update
)

table_file="results/kdv_ycsb_experiments/kdv_synthetic_ycsb_table.md"

echo "Generating summary table at ${table_file}"
{
    echo "| Mode       | Record Size (bytes) | Delta Bytes | Updates | Compression Ratio | Bandwidth Reduction |"
    echo "|------------|---------------------|------------:|--------:|-------------------|---------------------|"
} > "$table_file"

for cfg in "${SYNTHETIC_MODES[@]}"; do
    IFS=':' read -r label payload_size delta_region num_updates <<< "$cfg"
    mode_log="results/kdv_ycsb_experiments/kdv_synthetic_${label}.log"

    echo "Running synthetic mode: ${label} (payload=${payload_size}, delta=${delta_region}, updates=${num_updates})"
    ./build/kdv_synthetic_bench "$payload_size" "$delta_region" "$num_updates" > "$mode_log"

    # Parse summary metrics from the machine-readable line.
    summary_line=$(grep '^KDV_SYNTHETIC_SUMMARY' "$mode_log" || true)
    if [[ -z "$summary_line" ]]; then
        echo "  WARNING: No KDV_SYNTHETIC_SUMMARY line found in ${mode_log}, skipping"
        continue
    fi

    # Extract fields: compression_ratio and bandwidth_reduction_pct
    compression_ratio=$(echo "$summary_line" | sed -E 's/.*compression_ratio=([^,]+).*/\1/')
    bandwidth_reduction=$(echo "$summary_line" | sed -E 's/.*bandwidth_reduction_pct=([^,]+).*/\1/')

    echo "| ${label} | ${payload_size} | ${delta_region} | ${num_updates} | ${compression_ratio} | ${bandwidth_reduction}% |" >> "$table_file"
done

echo -e "${GREEN}✓ Synthetic YCSB-style KDV experiments complete${NC}"
echo ""

echo -e "${YELLOW}Step 3: Summarize synthetic results${NC}"
echo ""

echo "=== Canonical Synthetic YCSB-style Metrics (1KB, 16B delta) ==="
grep -E 'Payload size:|Updates:|Delta region:|Total original bytes:|Total encoded bytes:|Compression ratio:|Bandwidth reduction:' "$primary_log" || echo "No synthetic metrics found"
echo ""

echo "=== Synthetic YCSB-style Modes Table ==="
cat "$table_file"
echo ""

echo "=== Notes ==="
echo "These synthetic benchmarks use a single hot key with configurable record"
echo "size and update (delta) size to approximate YCSB-style small, medium, and"
echo "large in-place updates. Metrics are reported directly from the KDV layer."
echo ""

echo -e "${GREEN}=== YCSB-Style KDV Evaluation Complete ===${NC}"
echo "Results saved to results/kdv_ycsb_experiments/"
