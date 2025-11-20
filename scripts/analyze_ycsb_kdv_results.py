#!/usr/bin/env python3
"""
YCSB KDV Results Analysis Script (Phase 5)

This script analyzes the CSV output from ycsb_kdv_replicated.sh and computes:
- Network savings (%)
- Disk savings (%)
- Throughput overhead (%)
- Latency overhead (%)

Usage: python3 scripts/analyze_ycsb_kdv_results.py <input_csv> [output_csv]
"""

import sys
import csv
import os
from collections import defaultdict

def parse_csv(input_file):
    """Parse the input CSV file and group results by configuration."""
    results = defaultdict(lambda: {'baseline': None, 'kdv': None})
    
    with open(input_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            key = (row['workload_mix'], row['record_size'], row['update_bytes'])
            
            if row['kdv_enabled'] == 'false':
                results[key]['baseline'] = row
            else:
                results[key]['kdv'] = row
    
    return results

def safe_float(value, default=0.0):
    """Safely convert a value to float, returning default if conversion fails."""
    try:
        if value == 'N/A' or value == '' or value is None:
            return default
        return float(value)
    except (ValueError, TypeError):
        return default

def compute_metrics(baseline, kdv):
    """Compute savings and overhead metrics."""
    metrics = {}
    
    baseline_paxos = safe_float(baseline.get('paxos_bytes', 0))
    kdv_paxos = safe_float(kdv.get('paxos_bytes', 0))
    
    baseline_rocksdb = safe_float(baseline.get('rocksdb_size', 0))
    kdv_rocksdb = safe_float(kdv.get('rocksdb_size', 0))
    
    baseline_throughput = safe_float(baseline.get('throughput', 0))
    kdv_throughput = safe_float(kdv.get('throughput', 0))
    
    baseline_latency = safe_float(baseline.get('latency', 0))
    kdv_latency = safe_float(kdv.get('latency', 0))
    
    if baseline_paxos > 0:
        network_savings = ((baseline_paxos - kdv_paxos) / baseline_paxos) * 100
    else:
        network_savings = 0.0
    
    if baseline_rocksdb > 0:
        disk_savings = ((baseline_rocksdb - kdv_rocksdb) / baseline_rocksdb) * 100
    else:
        disk_savings = 0.0
    
    if baseline_throughput > 0:
        throughput_overhead = ((baseline_throughput - kdv_throughput) / baseline_throughput) * 100
    else:
        throughput_overhead = 0.0
    
    if baseline_latency > 0 and kdv_latency > 0:
        latency_overhead = ((kdv_latency - baseline_latency) / baseline_latency) * 100
    else:
        latency_overhead = 0.0
    
    compression_ratio = safe_float(kdv.get('compression_ratio', 0))
    
    metrics['workload_mix'] = baseline.get('workload_mix', 'N/A')
    metrics['record_size'] = baseline.get('record_size', 'N/A')
    metrics['update_bytes'] = baseline.get('update_bytes', 'N/A')
    metrics['baseline_paxos_bytes'] = baseline_paxos
    metrics['kdv_paxos_bytes'] = kdv_paxos
    metrics['network_savings_pct'] = network_savings
    metrics['baseline_rocksdb_size'] = baseline_rocksdb
    metrics['kdv_rocksdb_size'] = kdv_rocksdb
    metrics['disk_savings_pct'] = disk_savings
    metrics['baseline_throughput'] = baseline_throughput
    metrics['kdv_throughput'] = kdv_throughput
    metrics['throughput_overhead_pct'] = throughput_overhead
    metrics['baseline_latency'] = baseline_latency
    metrics['kdv_latency'] = kdv_latency
    metrics['latency_overhead_pct'] = latency_overhead
    metrics['compression_ratio_pct'] = compression_ratio
    
    return metrics

def format_bytes(bytes_value):
    """Format bytes into human-readable format."""
    if bytes_value == 0:
        return "0 B"
    
    units = ['B', 'KB', 'MB', 'GB', 'TB']
    unit_index = 0
    value = float(bytes_value)
    
    while value >= 1024 and unit_index < len(units) - 1:
        value /= 1024
        unit_index += 1
    
    return f"{value:.2f} {units[unit_index]}"

def print_summary(all_metrics):
    """Print a summary of the results."""
    print("\n" + "="*80)
    print("YCSB KDV Evaluation Results Summary")
    print("="*80)
    
    for metrics in all_metrics:
        print(f"\nConfiguration:")
        print(f"  Workload: {metrics['workload_mix']}")
        print(f"  Record size: {metrics['record_size']} bytes")
        print(f"  Update bytes: {metrics['update_bytes']} bytes")
        print(f"\nNetwork (Paxos):")
        print(f"  Baseline: {format_bytes(metrics['baseline_paxos_bytes'])}")
        print(f"  KDV:      {format_bytes(metrics['kdv_paxos_bytes'])}")
        print(f"  Savings:  {metrics['network_savings_pct']:.2f}%")
        print(f"\nDisk (RocksDB):")
        print(f"  Baseline: {format_bytes(metrics['baseline_rocksdb_size'])}")
        print(f"  KDV:      {format_bytes(metrics['kdv_rocksdb_size'])}")
        print(f"  Savings:  {metrics['disk_savings_pct']:.2f}%")
        print(f"\nThroughput:")
        print(f"  Baseline: {metrics['baseline_throughput']:.2f} ops/sec")
        print(f"  KDV:      {metrics['kdv_throughput']:.2f} ops/sec")
        print(f"  Overhead: {metrics['throughput_overhead_pct']:.2f}%")
        
        if metrics['baseline_latency'] > 0 and metrics['kdv_latency'] > 0:
            print(f"\nLatency:")
            print(f"  Baseline: {metrics['baseline_latency']:.2f} ms")
            print(f"  KDV:      {metrics['kdv_latency']:.2f} ms")
            print(f"  Overhead: {metrics['latency_overhead_pct']:.2f}%")
        
        print(f"\nKDV Compression Ratio: {metrics['compression_ratio_pct']:.2f}%")
        print("-" * 80)
    
    print("\n" + "="*80)
    print("Overall Statistics")
    print("="*80)
    
    avg_network_savings = sum(m['network_savings_pct'] for m in all_metrics) / len(all_metrics)
    avg_disk_savings = sum(m['disk_savings_pct'] for m in all_metrics) / len(all_metrics)
    avg_throughput_overhead = sum(m['throughput_overhead_pct'] for m in all_metrics) / len(all_metrics)
    
    print(f"Average network savings: {avg_network_savings:.2f}%")
    print(f"Average disk savings: {avg_disk_savings:.2f}%")
    print(f"Average throughput overhead: {avg_throughput_overhead:.2f}%")
    
    best_network = max(all_metrics, key=lambda m: m['network_savings_pct'])
    worst_network = min(all_metrics, key=lambda m: m['network_savings_pct'])
    
    print(f"\nBest network savings: {best_network['network_savings_pct']:.2f}% "
          f"(workload={best_network['workload_mix']}, record={best_network['record_size']}, "
          f"update={best_network['update_bytes']})")
    print(f"Worst network savings: {worst_network['network_savings_pct']:.2f}% "
          f"(workload={worst_network['workload_mix']}, record={worst_network['record_size']}, "
          f"update={worst_network['update_bytes']})")
    
    print("="*80)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_ycsb_kdv_results.py <input_csv> [output_csv]")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else "ycsb_kdv_analysis.csv"
    
    if not os.path.exists(input_file):
        print(f"Error: Input file not found: {input_file}")
        sys.exit(1)
    
    print(f"Analyzing results from: {input_file}")
    
    results = parse_csv(input_file)
    
    all_metrics = []
    for key, data in results.items():
        if data['baseline'] and data['kdv']:
            metrics = compute_metrics(data['baseline'], data['kdv'])
            all_metrics.append(metrics)
        else:
            print(f"Warning: Missing baseline or KDV data for configuration: {key}")
    
    if not all_metrics:
        print("Error: No valid data pairs found in input file")
        sys.exit(1)
    
    fieldnames = [
        'workload_mix', 'record_size', 'update_bytes',
        'baseline_paxos_bytes', 'kdv_paxos_bytes', 'network_savings_pct',
        'baseline_rocksdb_size', 'kdv_rocksdb_size', 'disk_savings_pct',
        'baseline_throughput', 'kdv_throughput', 'throughput_overhead_pct',
        'baseline_latency', 'kdv_latency', 'latency_overhead_pct',
        'compression_ratio_pct'
    ]
    
    with open(output_file, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(all_metrics)
    
    print(f"Analysis saved to: {output_file}")
    
    print_summary(all_metrics)

if __name__ == '__main__':
    main()
