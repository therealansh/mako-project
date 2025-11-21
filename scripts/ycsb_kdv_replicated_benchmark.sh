#!/bin/bash
#
# YCSB KDV vs Baseline Evaluation in Replicated Mode
#
# This script runs the internal YCSB benchmark on a 1‑shard Paxos-replicated
# configuration and collects:
#   - Throughput (agg_throughput ops/sec)
#   - Average latency (avg_latency ms)
#   - RocksDB disk usage (bytes)
#   - KDV compression stats (original/encoded bytes, compression ratio)
#   - Paxos network bytes (total bytes sent across the Paxos network)
#
# It runs each configuration twice:
#   - baseline  (KDV disabled)
#   - kdv       (KDV enabled via MAKO_ENABLE_KDV_LOGS=1)
#
# Results are written to a CSV so you can directly compute:
#   - Throughput difference and overhead
#   - Latency overhead
#   - Storage reduction (RocksDB bytes)
#   - Network bandwidth savings (Paxos bytes)
#
# Usage:
#   ./scripts/ycsb_kdv_replicated_benchmark.sh [output_csv]
#
# Environment overrides (optional):
#   THREADS      - worker threads per replica (default: 6)
#   RUNTIME      - seconds per experiment (default: 60)
#   NUM_KEYS     - number of YCSB keys (default: 100000)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

OUTPUT_CSV="${1:-results/ycsb_kdv_replicated_benchmark.csv}"
RESULTS_DIR="${PROJECT_ROOT}/results/ycsb_kdv_replicated_benchmark"
mkdir -p "${RESULTS_DIR}"

THREADS="${THREADS:-6}"
RUNTIME="${RUNTIME:-60}"
NUM_KEYS="${NUM_KEYS:-100000}"

# Workload definitions: label:read,write,rmw,scan:description
# These roughly correspond to YCSB-style mixes:
#   - B_like: read-heavy
#   - A_like: balanced read/write
#   - F_like: write-heavy
WORKLOADS=(
  "B_like:95,0,5,0:Read-heavy (YCSB-B-like)"
  "A_like:50,0,50,0:Balanced read/update (YCSB-A-like)"
  "F_like:20,0,80,0:Write-heavy (YCSB-F-like)"
)

# Record sizes (bytes)
RECORD_SIZES=(100 1024 4096)

# Update size configs:
#   - small:<bytes>  -> fixed small delta
#   - half           -> half of record size
#   - large          -> 90% of record size
UPDATE_CONFIGS=(
  "small:16"
  "half"
  "large"
)

log_err() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >&2
}

cleanup_processes() {
  log_err "Cleaning up dbtest processes and RocksDB directories..."
  pkill -9 dbtest 2>/dev/null || true
  sleep 2
  rm -rf /tmp/mako_rocksdb_shard* /tmp/*_mako_rocksdb_shard* 2>/dev/null || true
}

measure_rocksdb_bytes() {
  # Sum size of all RocksDB shard directories
  local total
  total=$(du -sb /tmp/mako_rocksdb_shard* 2>/dev/null | awk '{sum+=$1} END {print sum}' || true)
  if [ -z "$total" ]; then
    total="0"
  fi
  echo "$total"
}

extract_metrics_from_logs() {
  local run_dir="$1"
  local kdv_enabled="$2"

  # Use the localhost log as the primary source for throughput/latency.
  local localhost_log
  localhost_log=$(ls "${run_dir}"/*_localhost-*.log 2>/dev/null | head -1 || true)

  local throughput="N/A"
  local latency="N/A"

  if [ -n "$localhost_log" ] && [ -f "$localhost_log" ]; then
    throughput=$(grep -oP 'agg_throughput:\s+\K[0-9.]+' "$localhost_log" 2>/dev/null | tail -1 || true)
    latency=$(grep -oP 'avg_latency:\s+\K[0-9.]+' "$localhost_log" 2>/dev/null | tail -1 || true)
    [ -z "$throughput" ] && throughput="N/A"
    [ -z "$latency" ] && latency="N/A"
  fi

  # Paxos network bytes: sum across all logs that report them.
  local network_bytes=0
  local found_network=false
  local log
  for log in "${run_dir}"/*.log; do
    [ ! -f "$log" ] && continue
    local nb
    nb=$(grep '\[Paxos Network\]' "$log" 2>/dev/null | grep -oP 'total bytes sent:\s+\K[0-9]+' | tail -1 || true)
    if [ -n "$nb" ] && [ "$nb" -gt 0 ] 2>/dev/null; then
      network_bytes=$((network_bytes + nb))
      found_network=true
    fi
  done
  if [ "$found_network" = false ]; then
    network_bytes="N/A"
  fi

  # KDV-specific stats from any log that has them (usually leader).
  local original_bytes="N/A"
  local encoded_bytes="N/A"
  local compression_ratio="N/A"

  if [ "$kdv_enabled" = "true" ]; then
    for log in "${run_dir}"/*.log; do
      [ ! -f "$log" ] && continue
      local ob eb cr
      ob=$(grep -oP 'Total original bytes:\s+\K[0-9]+' "$log" 2>/dev/null | tail -1 || true)
      eb=$(grep -oP 'Total encoded bytes:\s+\K[0-9]+' "$log" 2>/dev/null | tail -1 || true)
      cr=$(grep -oP 'Compression ratio:\s+\K[-0-9.]+' "$log" 2>/dev/null | tail -1 || true)
      if [ -n "$ob" ]; then
        original_bytes="$ob"
        [ -n "$eb" ] && encoded_bytes="$eb"
        [ -n "$cr" ] && compression_ratio="$cr"
        break
      fi
    done
  fi

  # RocksDB disk usage (bytes) across all shard dirs.
  local rocksdb_bytes
  rocksdb_bytes=$(measure_rocksdb_bytes)

  echo "$throughput,$latency,$network_bytes,$original_bytes,$encoded_bytes,$compression_ratio,$rocksdb_bytes"
}

run_one_experiment() {
  local exp_label="$1"      # e.g., B_like
  local workload_mix="$2"   # e.g., 95,0,5,0
  local record_size="$3"    # bytes
  local update_cfg="$4"     # small:16 | half | large
  local mode="$5"           # baseline | kdv

  local kdv_enabled="false"
  if [ "$mode" = "kdv" ]; then
    kdv_enabled="true"
  fi

  # Compute concrete update_bytes from config and record_size.
  local update_bytes
  case "$update_cfg" in
    small:*)
      update_bytes="${update_cfg#small:}"
      ;;
    half)
      update_bytes=$((record_size / 2))
      ;;
    large)
      update_bytes=$((record_size * 9 / 10))
      ;;
    *)
      update_bytes="$record_size"
      ;;
  esac

  log_err "============================================================"
  log_err "Experiment: ${exp_label} | mode=${mode}"
  log_err "  workload_mix = ${workload_mix}"
  log_err "  record_size  = ${record_size} bytes"
  log_err "  update_cfg   = ${update_cfg} (update_bytes=${update_bytes})"
  log_err "  num_keys     = ${NUM_KEYS}"
  log_err "  threads      = ${THREADS}"
  log_err "  runtime      = ${RUNTIME}s"
  log_err "  kdv_enabled  = ${kdv_enabled}"
  log_err "============================================================"

  cleanup_processes

  if [ "$kdv_enabled" = "true" ]; then
    export MAKO_ENABLE_KDV_LOGS=1
  else
    unset MAKO_ENABLE_KDV_LOGS
  fi

  local run_dir="${RESULTS_DIR}/${exp_label}_r${record_size}_u${update_bytes}_${mode}"
  mkdir -p "$run_dir"

  cd "$PROJECT_ROOT"

  # Start 4 replicas: localhost, learner, p2, p1 (leader last).
  log_err "Starting Paxos-replicated YCSB cluster..."

  nohup bash bash/shard.sh 1 0 "$THREADS" localhost 0 1 ycsb \
    -w "$workload_mix" -r "$record_size" -u "$update_bytes" -m middle -k "$NUM_KEYS" \
    > "${run_dir}/shard0-localhost-${THREADS}.log" 2>&1 &

  nohup bash bash/shard.sh 1 0 "$THREADS" learner 0 1 ycsb \
    -w "$workload_mix" -r "$record_size" -u "$update_bytes" -m middle -k "$NUM_KEYS" \
    > "${run_dir}/shard0-learner-${THREADS}.log" 2>&1 &

  nohup bash bash/shard.sh 1 0 "$THREADS" p2 0 1 ycsb \
    -w "$workload_mix" -r "$record_size" -u "$update_bytes" -m middle -k "$NUM_KEYS" \
    > "${run_dir}/shard0-p2-${THREADS}.log" 2>&1 &

  sleep 1

  nohup bash bash/shard.sh 1 0 "$THREADS" p1 0 1 ycsb \
    -w "$workload_mix" -r "$record_size" -u "$update_bytes" -m middle -k "$NUM_KEYS" \
    > "${run_dir}/shard0-p1-${THREADS}.log" 2>&1 &
  local leader_pid=$!

  log_err "Cluster started (leader PID=${leader_pid}), running for ${RUNTIME}s..."
  sleep "$RUNTIME"

  log_err "Stopping leader..."
  kill "$leader_pid" 2>/dev/null || true
  wait "$leader_pid" 2>/dev/null || true

  # Give processes a bit of time to flush final statistics.
  sleep 5

  # Extract metrics from logs.
  local metrics
  metrics=$(extract_metrics_from_logs "$run_dir" "$kdv_enabled")

  # Split workload mix for CSV.
  local read_pct write_pct rmw_pct scan_pct
  IFS=',' read -r read_pct write_pct rmw_pct scan_pct <<< "$workload_mix"

  # CSV row: one line per (config, mode).
  echo "${exp_label},${mode},${read_pct},${write_pct},${rmw_pct},${scan_pct},${record_size},${update_bytes},${NUM_KEYS},${THREADS},${RUNTIME},${kdv_enabled},${metrics}"
}

main() {
  cd "$PROJECT_ROOT"

  log_err "YCSB KDV vs Baseline Replicated Evaluation"
  log_err "Results directory: ${RESULTS_DIR}"
  log_err "Output CSV: ${OUTPUT_CSV}"
  log_err "THREADS=${THREADS}, RUNTIME=${RUNTIME}, NUM_KEYS=${NUM_KEYS}"

  # CSV header
  local header="experiment_label,mode,read_pct,write_pct,rmw_pct,scan_pct,record_size,update_bytes,num_keys,threads,runtime_sec,kdv_enabled,throughput,avg_latency_ms,paxos_bytes,original_bytes,encoded_bytes,compression_ratio_pct,rocksdb_bytes"
  echo "$header" > "$OUTPUT_CSV"

  for workload_spec in "${WORKLOADS[@]}"; do
    IFS=':' read -r label mix desc <<< "$workload_spec"

    log_err "------------------------------------------------------------"
    log_err "Workload ${label}: ${mix}  (${desc})"
    log_err "------------------------------------------------------------"

    for record_size in "${RECORD_SIZES[@]}"; do
      for update_cfg in "${UPDATE_CONFIGS[@]}"; do
        # Baseline
        local row
        row=$(run_one_experiment "$label" "$mix" "$record_size" "$update_cfg" "baseline")
        echo "$row" >> "$OUTPUT_CSV"

        # KDV
        row=$(run_one_experiment "$label" "$mix" "$record_size" "$update_cfg" "kdv")
        echo "$row" >> "$OUTPUT_CSV"

        # Short pause between experiments.
        sleep 3
      done
    done
  done

  cleanup_processes

  log_err "============================================================"
  log_err "Evaluation complete. CSV results: ${OUTPUT_CSV}"
  log_err "You can now compute:"
  log_err "  - throughput diff: by (mode=kdv vs baseline)"
  log_err "  - latency overhead: avg_latency_ms (kdv vs baseline)"
  log_err "  - storage reduction: rocksdb_bytes (kdv vs baseline)"
  log_err "  - network savings: paxos_bytes (kdv vs baseline)"
  log_err "============================================================"
}

main "$@"

