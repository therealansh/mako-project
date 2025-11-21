#!/usr/bin/env python3
"""
KDV Network & Storage Analysis Script

Analyzes CSV output from kdv_network_storage_eval.sh and computes:
- Network bandwidth reduction (primary metric for geo-replication)
- Storage space reduction
- KDV compression effectiveness
- Performance overhead

Usage: python3 analyze_kdv_network_storage.py <input_csv>
"""

import sys
import csv
import os
from collections import defaultdict

def safe_float(value, default=0.0):
    """Safely convert value to float."""
    try:
        if value == 'N/A' or value == '' or value is None:
            return default
        return float(value)
    except (ValueError, TypeError):
        return default

def format_bytes(bytes_value):
    """Format bytes into human-readable format."""
    if bytes_value == 0:
        return "0 B"

    units = ['B', 'KB', 'MB', 'GB']
    unit_index = 0
    value = float(bytes_value)

    while value >= 1024 and unit_index < len(units) - 1:
        value /= 1024
        unit_index += 1

    return f"{value:.2f} {units[unit_index]}"

def parse_csv(input_file):
    """Parse CSV and group by experiment."""
    results = defaultdict(lambda: {'baseline': None, 'kdv': None})

    with open(input_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            exp_name = row['experiment']
            mode = row['mode']
            results[exp_name][mode] = row

    return results

def compute_savings(baseline, kdv):
    """Compute bandwidth and storage savings."""

    # Network bandwidth (most important for geo-replication)
    baseline_network = safe_float(baseline.get('network_bytes', 0))
    kdv_network = safe_float(kdv.get('network_bytes', 0))

    if baseline_network > 0:
        network_saving = ((baseline_network - kdv_network) / baseline_network) * 100
        network_reduction_bytes = baseline_network - kdv_network
    else:
        network_saving = 0.0
        network_reduction_bytes = 0

    # Storage (RocksDB disk usage)
    baseline_storage = safe_float(baseline.get('rocksdb_bytes', 0))
    kdv_storage = safe_float(kdv.get('rocksdb_bytes', 0))

    if baseline_storage > 0:
        storage_saving = ((baseline_storage - kdv_storage) / baseline_storage) * 100
        storage_reduction_bytes = baseline_storage - kdv_storage
    else:
        storage_saving = 0.0
        storage_reduction_bytes = 0

    # KDV compression ratio
    compression_ratio = safe_float(kdv.get('compression_ratio_pct', 0))

    # Performance overhead
    baseline_tps = safe_float(baseline.get('throughput', 0))
    kdv_tps = safe_float(kdv.get('throughput', 0))

    if baseline_tps > 0:
        throughput_overhead = ((baseline_tps - kdv_tps) / baseline_tps) * 100
    else:
        throughput_overhead = 0.0

    return {
        'experiment': baseline.get('experiment', 'N/A'),
        'description': baseline.get('description', 'N/A'),
        'record_size': baseline.get('record_size', 'N/A'),
        'update_bytes': baseline.get('update_bytes', 'N/A'),
        'workload': f"{baseline.get('read_pct', 'N/A')}% read, {baseline.get('rmw_pct', 'N/A')}% RMW",

        # Network metrics (PRIMARY)
        'baseline_network_bytes': baseline_network,
        'kdv_network_bytes': kdv_network,
        'network_saving_pct': network_saving,
        'network_reduction_bytes': network_reduction_bytes,

        # Storage metrics
        'baseline_storage_bytes': baseline_storage,
        'kdv_storage_bytes': kdv_storage,
        'storage_saving_pct': storage_saving,
        'storage_reduction_bytes': storage_reduction_bytes,

        # Compression
        'kdv_compression_ratio_pct': compression_ratio,

        # Performance
        'baseline_tps': baseline_tps,
        'kdv_tps': kdv_tps,
        'throughput_overhead_pct': throughput_overhead,
    }

def print_summary(all_results):
    """Print detailed summary report."""

    print("\n" + "=" * 100)
    print("KDV NETWORK & STORAGE EVALUATION RESULTS")
    print("=" * 100)
    print("\nFocus: Cross-Datacenter Replication Bandwidth and Storage Savings")
    print("=" * 100)

    # Sort by network savings (descending)
    sorted_results = sorted(all_results, key=lambda x: x['network_saving_pct'], reverse=True)

    print("\n" + "-" * 100)
    print("DETAILED RESULTS (sorted by network savings)")
    print("-" * 100)

    for i, result in enumerate(sorted_results, 1):
        print(f"\n[{i}] {result['experiment']}")
        print(f"    Description: {result['description']}")
        print(f"    Workload: {result['workload']}, Record: {result['record_size']}B, Update: {result['update_bytes']}B")
        print(f"")
        print(f"    📡 NETWORK (Paxos Replication Traffic):")
        print(f"       Baseline:  {format_bytes(result['baseline_network_bytes'])}")
        print(f"       KDV:       {format_bytes(result['kdv_network_bytes'])}")
        print(f"       Savings:   {result['network_saving_pct']:.1f}% ({format_bytes(result['network_reduction_bytes'])} saved)")
        print(f"")
        print(f"    💾 STORAGE (RocksDB Disk Usage):")
        print(f"       Baseline:  {format_bytes(result['baseline_storage_bytes'])}")
        print(f"       KDV:       {format_bytes(result['kdv_storage_bytes'])}")
        print(f"       Savings:   {result['storage_saving_pct']:.1f}% ({format_bytes(result['storage_reduction_bytes'])} saved)")
        print(f"")
        print(f"    🗜️  KDV COMPRESSION: {result['kdv_compression_ratio_pct']:.1f}%")
        print(f"")
        print(f"    ⚡ PERFORMANCE:")
        print(f"       Baseline TPS: {result['baseline_tps']:.0f}")
        print(f"       KDV TPS:      {result['kdv_tps']:.0f}")
        print(f"       Overhead:     {result['throughput_overhead_pct']:.1f}%")
        print("-" * 100)

    # Summary statistics
    print("\n" + "=" * 100)
    print("SUMMARY STATISTICS")
    print("=" * 100)

    avg_network_saving = sum(r['network_saving_pct'] for r in all_results) / len(all_results)
    avg_storage_saving = sum(r['storage_saving_pct'] for r in all_results) / len(all_results)
    avg_overhead = sum(r['throughput_overhead_pct'] for r in all_results) / len(all_results)

    total_network_saved = sum(r['network_reduction_bytes'] for r in all_results)
    total_storage_saved = sum(r['storage_reduction_bytes'] for r in all_results)

    print(f"\n📊 AVERAGES:")
    print(f"   Network bandwidth savings:  {avg_network_saving:.1f}%")
    print(f"   Storage space savings:      {avg_storage_saving:.1f}%")
    print(f"   Performance overhead:       {avg_overhead:.1f}%")

    print(f"\n💰 TOTAL SAVINGS (across all experiments):")
    print(f"   Network bandwidth saved:    {format_bytes(total_network_saved)}")
    print(f"   Storage space saved:        {format_bytes(total_storage_saved)}")

    # Best and worst cases
    best_network = max(all_results, key=lambda x: x['network_saving_pct'])
    worst_network = min(all_results, key=lambda x: x['network_saving_pct'])

    print(f"\n🏆 BEST NETWORK SAVINGS:")
    print(f"   Experiment: {best_network['experiment']}")
    print(f"   Savings: {best_network['network_saving_pct']:.1f}%")
    print(f"   Config: {best_network['record_size']}B records, {best_network['update_bytes']}B updates")

    print(f"\n⚠️  WORST NETWORK SAVINGS:")
    print(f"   Experiment: {worst_network['experiment']}")
    print(f"   Savings: {worst_network['network_saving_pct']:.1f}%")
    print(f"   Config: {best_network['record_size']}B records, {worst_network['update_bytes']}B updates")

    # Recommendations
    print("\n" + "=" * 100)
    print("💡 RECOMMENDATIONS")
    print("=" * 100)

    if avg_network_saving >= 50:
        print("\n✅ KDV is achieving target bandwidth reduction (50-70%)")
        print("   → Ready for geo-replication deployment")
    elif avg_network_saving >= 30:
        print("\n⚠️  KDV achieving moderate bandwidth reduction (30-50%)")
        print("   → Consider tuning policies (MaxChainLen, MaxDeltaSizeRatio)")
        print("   → Analyze workload patterns for optimization opportunities")
    else:
        print("\n❌ KDV not achieving target bandwidth reduction (<30%)")
        print("   → Check key identification (ensure repeated updates to same keys)")
        print("   → Verify update patterns (small deltas work best)")
        print("   → Review KDV policy settings")

    if avg_overhead <= 5:
        print("\n✅ Performance overhead is acceptable (<5%)")
    elif avg_overhead <= 10:
        print("\n⚠️  Performance overhead is moderate (5-10%)")
        print("   → Consider optimizing encode/decode paths")
    else:
        print("\n❌ Performance overhead is high (>10%)")
        print("   → Profile encoding/decoding performance")
        print("   → Consider async encoding for non-critical path")

    # Workload-specific insights
    print("\n" + "=" * 100)
    print("📈 WORKLOAD-SPECIFIC INSIGHTS")
    print("=" * 100)

    small_update_results = [r for r in all_results if safe_float(r['update_bytes']) <= 64]
    large_update_results = [r for r in all_results if safe_float(r['update_bytes']) >= 512]

    if small_update_results:
        avg_small = sum(r['network_saving_pct'] for r in small_update_results) / len(small_update_results)
        print(f"\n• Small updates (≤64 bytes): {avg_small:.1f}% average network savings")
        print(f"  → {'✅ Excellent compression' if avg_small >= 70 else '⚠️  Review delta encoding'}")

    if large_update_results:
        avg_large = sum(r['network_saving_pct'] for r in large_update_results) / len(large_update_results)
        print(f"\n• Large updates (≥512 bytes): {avg_large:.1f}% average network savings")
        print(f"  → {'✅ KDV correctly falling back to base encoding' if avg_large < 20 else '⚠️  Unexpected compression on large updates'}")

    print("\n" + "=" * 100)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_kdv_network_storage.py <input_csv>")
        sys.exit(1)

    input_file = sys.argv[1]

    if not os.path.exists(input_file):
        print(f"Error: Input file not found: {input_file}")
        sys.exit(1)

    print(f"Analyzing results from: {input_file}")

    results_dict = parse_csv(input_file)

    all_results = []
    for exp_name, modes in results_dict.items():
        if modes['baseline'] and modes['kdv']:
            result = compute_savings(modes['baseline'], modes['kdv'])
            all_results.append(result)
        else:
            print(f"Warning: Missing baseline or KDV data for experiment: {exp_name}")

    if not all_results:
        print("Error: No valid experiment pairs found")
        sys.exit(1)

    # Save detailed results to CSV
    output_csv = input_file.replace('.csv', '_analysis.csv')
    fieldnames = [
        'experiment', 'description', 'record_size', 'update_bytes', 'workload',
        'baseline_network_bytes', 'kdv_network_bytes', 'network_saving_pct', 'network_reduction_bytes',
        'baseline_storage_bytes', 'kdv_storage_bytes', 'storage_saving_pct', 'storage_reduction_bytes',
        'kdv_compression_ratio_pct', 'baseline_tps', 'kdv_tps', 'throughput_overhead_pct'
    ]

    with open(output_csv, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(all_results)

    print(f"Detailed analysis saved to: {output_csv}")

    # Print summary
    print_summary(all_results)

if __name__ == '__main__':
    main()
