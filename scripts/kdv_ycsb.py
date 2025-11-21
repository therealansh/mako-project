  #!/usr/bin/env python3
  import argparse
  import csv
  import os
  import re
  import subprocess
  import sys
  from pathlib import Path
  from typing import Dict, Any, List, Tuple

  # Read/write mixes: name -> "R,W,RMW,Scan" (100 total)
  RW_MIXES = {
      "read_heavy_95_5": "95,5,0,0",
      "balanced_50_50": "50,50,0,0",
      "write_heavy_20_80": "20,80,0,0",
  }

  # Update patterns: name -> extra args string appended to your command template.
  # Fill these with whatever flags your YCSB harness uses to select update-size pattern.
  UPDATE_PATTERNS = {
      "small_updates": "",
      "medium_updates": "",
      "large_updates": "",
  }


  def run_cmd(cmd: str, log_path: Path, env: Dict[str, str]) -> int:
      log_path.parent.mkdir(parents=True, exist_ok=True)
      print(f"Running: {cmd}")
      with log_path.open("w") as f:
          proc = subprocess.run(cmd, shell=True, stdout=f, stderr=subprocess.STDOUT, env=env)
      if proc.returncode != 0:
          print(f"[WARN] Command failed (rc={proc.returncode}) for {log_path}")
      return proc.returncode


  def measure_disk_bytes() -> int:
      # Sum size of all /tmp/mako_rocksdb_shard* directories
      try:
          proc = subprocess.run(
              ["bash", "-lc", "du -sb /tmp/mako_rocksdb_shard* 2>/dev/null || true"],
              check=False,
              capture_output=True,
              text=True,
          )
      except Exception:
          return 0

      total = 0
      for line in proc.stdout.strip().splitlines():
          parts = line.split()
          if not parts:
              continue
          try:
              total += int(parts[0])
          except ValueError:
              continue
      return total


  def parse_metrics_from_log(log_path: Path) -> Dict[str, Any]:
      metrics: Dict[str, Any] = {}
      if not log_path.exists():
          return metrics

      with log_path.open() as f:
          lines = f.readlines()

      # Generic bench stats
      for line in lines:
          m = re.search(r"n_commits:\s+([0-9]+)", line)
          if m:
              metrics["n_commits"] = int(m.group(1))

          m = re.search(r"agg_throughput:\s+([0-9.eE+\-]+)\s+ops/sec", line)
          if m:
              metrics["agg_throughput_ops"] = float(m.group(1))

          m = re.search(r"agg_persist_throughput:\s+([0-9.eE+\-]+)\s+ops/sec", line)
          if m:
              metrics["agg_persist_throughput_ops"] = float(m.group(1))

          m = re.search(r"avg_latency:\s+([0-9.eE+\-]+)\s+ms", line)
          if m:
              metrics["avg_latency_ms"] = float(m.group(1))

          m = re.search(r"avg_persist_latency:\s+([0-9.eE+\-]+)\s+ms", line)
          if m:
              metrics["avg_persist_latency_ms"] = float(m.group(1))

      # Paxos network stats (write bandwidth)
      for line in lines:
          m = re.search(
              r"\[Paxos Network\] Final statistics: total bytes sent:\s+([0-9]+)",
              line,
          )
          if m:
              metrics["network_bytes"] = int(m.group(1))

      # RocksDB KDV compression stats (present only when KDV is enabled)
      for i, line in enumerate(lines):
          if "=== RocksDB KDV Compression Statistics ===" in line:
              for j in range(i + 1, min(i + 10, len(lines))):
                  l2 = lines[j]
                  m = re.search(r"Total original bytes:\s+([0-9]+)", l2)
                  if m:
                      metrics["kdv_total_original_bytes"] = int(m.group(1))
                  m = re.search(r"Total encoded bytes:\s+([0-9]+)", l2)
                  if m:
                      metrics["kdv_total_encoded_bytes"] = int(m.group(1))
                  m = re.search(r"Compression ratio:\s+([0-9.\-eE+]+)%?", l2)
                  if m:
                      # May be negative or >100 if buggy; keep as-is
                      try:
                          metrics["kdv_compression_ratio_pct"] = float(m.group(1))
                      except ValueError:
                          pass
              break

      return metrics


  def build_row(
      rw_name: str,
      rw_mix: str,
      upd_name: str,
      mode: str,
      metrics: Dict[str, Any],
      disk_bytes: int,
  ) -> Dict[str, Any]:
      row: Dict[str, Any] = {
          "workload_label": f"{rw_name}__{upd_name}",
          "rw_mix": rw_mix,
          "update_pattern": upd_name,
          "mode": mode,  # "baseline" or "kdv"
          "avg_latency_ms": metrics.get("avg_latency_ms"),
          "avg_persist_latency_ms": metrics.get("avg_persist_latency_ms"),
          "agg_throughput_ops": metrics.get("agg_throughput_ops"),
          "agg_persist_throughput_ops": metrics.get("agg_persist_throughput_ops"),
          "n_commits": metrics.get("n_commits"),
          "network_bytes": metrics.get("network_bytes"),
          "disk_bytes": disk_bytes,
          "kdv_total_original_bytes": metrics.get("kdv_total_original_bytes"),
          "kdv_total_encoded_bytes": metrics.get("kdv_total_encoded_bytes"),
          "kdv_compression_ratio_pct": metrics.get("kdv_compression_ratio_pct"),
          # filled later
          "network_savings_pct_vs_baseline": None,
          "latency_overhead_pct_vs_baseline": None,
      }
      return row


  def compute_deltas(rows: List[Dict[str, Any]]) -> None:
      # Group rows by (rw_mix, update_pattern)
      group: Dict[Tuple[str, str], Dict[str, Dict[str, Any]]] = {}
      for r in rows:
          key = (r["rw_mix"], r["update_pattern"])
          group.setdefault(key, {})
          group[key][r["mode"]] = r

      for key, modes in group.items():
          base = modes.get("baseline")
          kdv = modes.get("kdv")
          if not base or not kdv:
              continue

          b_net = base.get("network_bytes")
          k_net = kdv.get("network_bytes")
          if isinstance(b_net, int) and isinstance(k_net, int) and b_net > 0:
              savings = (1.0 - (k_net / float(b_net))) * 100.0
              kdv["network_savings_pct_vs_baseline"] = savings

          b_lat = base.get("avg_latency_ms")
          k_lat = kdv.get("avg_latency_ms")
          if isinstance(b_lat, (int, float)) and b_lat > 0 and isinstance(
              k_lat, (int, float)
          ):
              overhead = ((k_lat - b_lat) / b_lat) * 100.0
              kdv["latency_overhead_pct_vs_baseline"] = overhead


  def write_summary_csv(rows: List[Dict[str, Any]], out_dir: Path) -> None:
      out_dir.mkdir(parents=True, exist_ok=True)
      out_path = out_dir / "summary.csv"
      fieldnames = [
          "workload_label",
          "rw_mix",
          "update_pattern",
          "mode",
          "avg_latency_ms",
          "avg_persist_latency_ms",
          "agg_throughput_ops",
          "agg_persist_throughput_ops",
          "n_commits",
          "network_bytes",
          "disk_bytes",
          "kdv_total_original_bytes",
          "kdv_total_encoded_bytes",
          "kdv_compression_ratio_pct",
          "network_savings_pct_vs_baseline",
          "latency_overhead_pct_vs_baseline",
      ]
      with out_path.open("w", newline="") as f:
          w = csv.DictWriter(f, fieldnames=fieldnames)
          w.writeheader()
          for r in rows:
              w.writerow(r)
      print(f"Wrote summary CSV to {out_path}")


  def main() -> None:
      parser = argparse.ArgumentParser(
          description=(
              "Sweep YCSB-style workloads over RW mixes and update sizes, "
              "comparing baseline vs KDV and extracting metrics."
          )
      )
      parser.add_argument(
          "--cmd-template",
          required=True,
          help=(
              "Shell command template for running your YCSB workload. "
              "It is formatted with: {rw_mix}, {update_args}, {threads}, {runtime}. "
              "Example (once YCSB is wired up): "
              "'./build/your_ycsb_driver --num-threads {threads} "
              "--runtime {runtime} --workload-mix={rw_mix} {update_args}'"
          ),
      )
      parser.add_argument(
          "--results-dir",
          default="results/kdv_ycsb_sweep",
          help="Directory to write logs and summary.csv (default: results/kdv_ycsb_sweep)",
      )
      parser.add_argument(
          "--threads",
          type=int,
          default=6,
          help="Threads to pass as {threads} in the command template.",
      )
      parser.add_argument(
          "--runtime",
          type=int,
          default=120,
          help="Runtime seconds to pass as {runtime} in the command template.",
      )
      parser.add_argument(
          "--skip-runs",
          action="store_true",
          help="Do not run experiments, only re-parse existing logs into summary.csv.",
      )

      args = parser.parse_args()
      results_dir = Path(args.results_dir)

      rows: List[Dict[str, Any]] = []

      if not args.skip_runs:
          base_env = os.environ.copy()

          for rw_name, rw_mix in RW_MIXES.items():
              for upd_name, update_args in UPDATE_PATTERNS.items():
                  label = f"{rw_name}__{upd_name}"
                  combo_dir = results_dir / label
                  combo_dir.mkdir(parents=True, exist_ok=True)

                  # Baseline run (KDV disabled)
                  print(f"\n=== {label}: baseline (KDV disabled) ===")
                  # Clean RocksDB data
                  subprocess.run(
                      ["bash", "-lc", "rm -rf /tmp/mako_rocksdb_shard*"],
                      check=False,
                  )
                  baseline_env = base_env.copy()
                  baseline_env.pop("MAKO_ENABLE_KDV_LOGS", None)
                  baseline_cmd = args.cmd_template.format(
                      rw_mix=rw_mix,
                      update_args=update_args,
                      threads=args.threads,
                      runtime=args.runtime,
                  )
                  baseline_log = combo_dir / "baseline.log"
                  run_cmd(baseline_cmd, baseline_log, baseline_env)
                  baseline_disk = measure_disk_bytes()
                  baseline_metrics = parse_metrics_from_log(baseline_log)
                  rows.append(
                      build_row(
                          rw_name, rw_mix, upd_name, "baseline", baseline_metrics, baseline_disk
                      )
                  )

                  # KDV run (KDV enabled)
                  print(f"\n=== {label}: KDV enabled ===")
                  subprocess.run(
                      ["bash", "-lc", "rm -rf /tmp/mako_rocksdb_shard*"],
                      check=False,
                  )
                  kdv_env = base_env.copy()
                  kdv_env["MAKO_ENABLE_KDV_LOGS"] = "1"
                  kdv_cmd = args.cmd_template.format(
                      rw_mix=rw_mix,
                      update_args=update_args,
                      threads=args.threads,
                      runtime=args.runtime,
                  )
                  kdv_log = combo_dir / "kdv.log"
                  run_cmd(kdv_cmd, kdv_log, kdv_env)
                  kdv_disk = measure_disk_bytes()
                  kdv_metrics = parse_metrics_from_log(kdv_log)
                  rows.append(
                      build_row(
                          rw_name, rw_mix, upd_name, "kdv", kdv_metrics, kdv_disk
                      )
                  )
      else:
          # Only parse existing logs
          for rw_name, rw_mix in RW_MIXES.items():
              for upd_name, _ in UPDATE_PATTERNS.items():
                  label = f"{rw_name}__{upd_name}"
                  combo_dir = results_dir / label
                  baseline_log = combo_dir / "baseline.log"
                  kdv_log = combo_dir / "kdv.log"

                  if baseline_log.exists():
                      baseline_metrics = parse_metrics_from_log(baseline_log)
                      baseline_disk = 0
                      rows.append(
                          build_row(
                              rw_name,
                              rw_mix,
                              upd_name,
                              "baseline",
                              baseline_metrics,
                              baseline_disk,
                          )
                      )
                  if kdv_log.exists():
                      kdv_metrics = parse_metrics_from_log(kdv_log)
                      kdv_disk = 0
                      rows.append(
                          build_row(
                              rw_name, rw_mix, upd_name, "kdv", kdv_metrics, kdv_disk
                          )
                      )

      if not rows:
          print("No rows collected; nothing to summarize.", file=sys.stderr)
          return

      compute_deltas(rows)
      write_summary_csv(rows, results_dir)


  if __name__ == "__main__":
      main()