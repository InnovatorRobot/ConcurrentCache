#!/usr/bin/env python3
"""Visualize LruTtlCacheThreadSafe benchmark results.

Reads the CSV files produced by ``cache_benchmark`` and renders PNG charts for
throughput scaling, latency percentiles, memory footprint, and hit rate.

Usage:
    python3 scripts/visualize.py [results_dir]   # default: bench_results

Requires: matplotlib  (pip install matplotlib)
"""
from __future__ import annotations

import csv
import os
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")  # headless / no display required
import matplotlib.pyplot as plt


def read_csv(path):
    if not os.path.exists(path):
        print(f"  skip (missing): {path}")
        return None
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def plot_throughput(results_dir):
    rows = read_csv(os.path.join(results_dir, "throughput.csv"))
    if not rows:
        return

    series = defaultdict(list)  # read_ratio -> [(threads, ops_per_sec), ...]
    for r in rows:
        series[float(r["read_ratio"])].append((int(r["threads"]), float(r["ops_per_sec"])))

    fig, (ax_tp, ax_sp) = plt.subplots(1, 2, figsize=(13, 5))

    for ratio, pts in sorted(series.items()):
        pts.sort()
        threads = [p[0] for p in pts]
        ops_m = [p[1] / 1e6 for p in pts]
        ax_tp.plot(threads, ops_m, marker="o", label=f"{int(ratio * 100)}% reads")

        base = pts[0][1] if pts else 1.0
        speedup = [p[1] / base for p in pts]
        ax_sp.plot(threads, speedup, marker="o", label=f"{int(ratio * 100)}% reads")

    ax_tp.set_title("Throughput scaling")
    ax_tp.set_xlabel("threads")
    ax_tp.set_ylabel("million ops / sec")
    ax_tp.grid(True, alpha=0.3)
    ax_tp.legend()

    # Ideal linear-scaling reference line.
    all_threads = sorted({t for pts in series.values() for t, _ in pts})
    if all_threads:
        ax_sp.plot(all_threads, all_threads, "k--", alpha=0.4, label="ideal linear")
    ax_sp.set_title("Scalability (speedup vs 1 thread)")
    ax_sp.set_xlabel("threads")
    ax_sp.set_ylabel("speedup x")
    ax_sp.grid(True, alpha=0.3)
    ax_sp.legend()

    fig.tight_layout()
    out = os.path.join(results_dir, "throughput.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"  wrote {out}")


def plot_latency(results_dir):
    rows = read_csv(os.path.join(results_dir, "latency.csv"))
    if not rows:
        return

    data = defaultdict(dict)  # operation -> {percentile: ns}
    order = []
    for r in rows:
        op = r["operation"]
        pct = r["percentile"]
        data[op][pct] = float(r["nanoseconds"])
        if pct not in order:
            order.append(pct)

    fig, ax = plt.subplots(figsize=(10, 5))
    x = range(len(order))
    width = 0.38
    for i, (op, vals) in enumerate(sorted(data.items())):
        ys = [vals.get(p, 0) for p in order]
        offset = (i - (len(data) - 1) / 2) * width
        ax.bar([xi + offset for xi in x], ys, width=width, label=op)

    ax.set_title("Operation latency percentiles (single thread)")
    ax.set_xlabel("percentile")
    ax.set_ylabel("nanoseconds (log)")
    ax.set_yscale("log")
    ax.set_xticks(list(x))
    ax.set_xticklabels(order)
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend()

    fig.tight_layout()
    out = os.path.join(results_dir, "latency.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"  wrote {out}")


def plot_memory(results_dir):
    rows = read_csv(os.path.join(results_dir, "memory.csv"))
    if not rows:
        return

    entries = [int(r["entries"]) for r in rows]
    total_mb = [float(r["rss_bytes"]) / (1024 * 1024) for r in rows]
    bpe = [float(r["bytes_per_entry"]) for r in rows]

    fig, (ax_total, ax_bpe) = plt.subplots(1, 2, figsize=(13, 5))

    ax_total.plot(entries, total_mb, marker="o", color="tab:blue")
    ax_total.set_title("Resident memory vs entries")
    ax_total.set_xlabel("entries")
    ax_total.set_ylabel("RSS growth (MB)")
    ax_total.grid(True, alpha=0.3)

    ax_bpe.plot(entries, bpe, marker="o", color="tab:green")
    ax_bpe.set_title("Bytes per entry")
    ax_bpe.set_xlabel("entries")
    ax_bpe.set_ylabel("bytes / entry (approx.)")
    ax_bpe.grid(True, alpha=0.3)

    fig.tight_layout()
    out = os.path.join(results_dir, "memory.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"  wrote {out}")


def plot_hitrate(results_dir):
    rows = read_csv(os.path.join(results_dir, "hitrate.csv"))
    if not rows:
        return

    fracs = [float(r["capacity_fraction"]) * 100 for r in rows]
    hit = [float(r["hit_rate"]) * 100 for r in rows]

    fig, ax = plt.subplots(figsize=(9, 5))
    ax.plot(fracs, hit, marker="o", color="tab:purple")
    ax.set_title("Hit rate vs capacity (Zipf s=1.0 workload)")
    ax.set_xlabel("capacity (% of key space)")
    ax.set_ylabel("hit rate (%)")
    ax.set_ylim(0, 100)
    ax.grid(True, alpha=0.3)

    fig.tight_layout()
    out = os.path.join(results_dir, "hitrate.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"  wrote {out}")


def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else "bench_results"
    if not os.path.isdir(results_dir):
        print(f"error: results directory not found: {results_dir}")
        print("Run ./build/cache_benchmark first.")
        sys.exit(1)

    print(f"Reading benchmark CSVs from: {results_dir}")
    plot_throughput(results_dir)
    plot_latency(results_dir)
    plot_memory(results_dir)
    plot_hitrate(results_dir)
    print("Done.")


if __name__ == "__main__":
    main()
