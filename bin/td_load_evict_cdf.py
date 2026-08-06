#!/usr/bin/env python3
"""
Visualize the set-conflict / eviction data produced by the in-sim td_load_evict_track mode.

Input: one row per closed reuse interval (between two consecutive exceeding accesses to the
same L1D line l), written to <trace>_line_evict_p<NN>.csv:
    line,distinct_conflicts,ways
where distinct_conflicts = S = number of distinct lines filled into set(l) during the
interval, and ways = W = associativity. Under LRU, l is evicted between the two memory-bound
touches iff S >= W.

Produces two CDFs (plus a CCDF tail companion):
  1. Pooled, event-weighted CDF of S/W across all intervals, with a reference line at 1.0.
     Mass at/right of 1.0 = P(S >= W) = the eviction rate.
  2. Per-line eviction-rate CDF: for each line, rate = (#intervals with S>=W)/(#intervals)
     (lines with < --min-intervals intervals dropped), CDF over lines.

Usage:
    td_load_evict_cdf.py <file-or-dir> [...] --thresh 0.5 --out evict.png
    td_load_evict_cdf.py /home/shanen/samsung/td_load_traces --thresh 0.5   # dir glob
"""

import argparse
import csv
import glob
import math
import os
import sys
from collections import defaultdict


def gather_inputs(paths, thresh):
    pnn = f"p{int(100.0 * thresh + 0.5):02d}"
    pat = f"*line_evict_{pnn}.csv"
    out = []
    for p in paths:
        if os.path.isdir(p):
            out.extend(sorted(glob.glob(os.path.join(p, pat))))
        elif os.path.isfile(p):
            out.append(p)
        else:
            print(f"warning: {p} not found; skipping", file=sys.stderr)
    return out


def load_rows(files):
    """Return (sw_ratios, per_line[line] -> [evicted_bool,...])."""
    sw = []
    per_line = defaultdict(list)
    for path in files:
        with open(path, newline="") as f:
            r = csv.reader(f)
            next(r, None)  # header: line,distinct_conflicts,ways
            for row in r:
                if len(row) < 3:
                    continue
                try:
                    line = row[0].strip()
                    S = float(row[1])
                    W = float(row[2])
                except ValueError:
                    continue
                if W <= 0:
                    continue
                sw.append(S / W)
                per_line[line].append(S >= W)
    return sw, per_line


def cdf_xy(values):
    vs = sorted(values)
    n = len(vs)
    return vs, [(i + 1) / n for i in range(n)]


def plot(sw, per_line, thresh, min_intervals, out_path):
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("(matplotlib not available; skipping plot.)", file=sys.stderr)
        return
    if not sw:
        print("(no intervals; nothing to plot)", file=sys.stderr)
        return

    p_evict = sum(1 for r in sw if r >= 1.0) / len(sw)

    line_rates = [sum(b) / len(b) for b in per_line.values() if len(b) >= min_intervals]

    fig, axes = plt.subplots(1, 3, figsize=(18, 5))

    # 1. pooled S/W CDF
    x, y = cdf_xy(sw)
    ax = axes[0]
    ax.plot(x, y, color="#4c78a8")
    ax.axvline(1.0, color="#e45756", linestyle="--", label="S = W (eviction)")
    ax.set_xlim(0, min(4.0, max(x) if x else 4.0))
    ax.set_ylim(0, 1)
    ax.set_xlabel("S / W  (distinct set-fills per way)")
    ax.set_ylabel("CDF (event-weighted)")
    ax.set_title(f"Pooled S/W  |  P(S≥W)={p_evict:.1%}  (p={thresh}, n={len(sw)})")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=9)

    # 2. per-line eviction-rate CDF
    ax = axes[1]
    if line_rates:
        x2, y2 = cdf_xy(line_rates)
        ax.plot(x2, y2, color="#54a24b")
        ax.set_xlim(0, 1)
        ax.set_ylim(0, 1)
    ax.set_xlabel(f"per-line eviction rate  (≥{min_intervals} intervals/line)")
    ax.set_ylabel("CDF over lines")
    ax.set_title(f"Per-line eviction rate  (lines={len(line_rates)})")
    ax.grid(True, alpha=0.3)

    # 3. CCDF of S/W (log-log tail)
    ax = axes[2]
    x3, y3 = cdf_xy(sw)
    ccdf = [1.0 - v for v in y3]
    ax.plot(x3, ccdf, color="#4c78a8")
    ax.axvline(1.0, color="#e45756", linestyle="--")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("S / W (log)")
    ax.set_ylabel("CCDF  P(X > x)")
    ax.set_title("S/W tail (CCDF)")
    ax.grid(True, alpha=0.3, which="both")

    fig.suptitle(f"td_load_evict — set-conflict between memory-bound line reuses (p={thresh})")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    print(f"chart written to {out_path}")


def main():
    ap = argparse.ArgumentParser(description="CDF of distinct set-fills vs ways between memory-bound line reuses.")
    ap.add_argument("inputs", nargs="+", help="line_evict CSV file(s) or directory(ies)")
    ap.add_argument("--thresh", type=float, default=0.5, help="threshold p used at record time (default 0.5)")
    ap.add_argument("--min-intervals", type=int, default=5, help="min intervals per line for per-line CDF (default 5)")
    ap.add_argument("--out", default=None, help="output PNG (default: evict_cdf_p<NN>.png next to first input)")
    args = ap.parse_args()

    files = gather_inputs(args.inputs, args.thresh)
    if not files:
        sys.exit(f"error: no line_evict CSVs found for p={args.thresh}.")

    sw, per_line = load_rows(files)
    if not sw:
        sys.exit("error: no valid interval rows parsed.")

    p_evict = sum(1 for r in sw if r >= 1.0) / len(sw)
    print(f"intervals={len(sw)}  distinct lines={len(per_line)}  P(S>=W)={p_evict:.3f}  (p={args.thresh})")

    pnn = f"p{int(100.0 * args.thresh + 0.5):02d}"
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(files[0])), f"evict_cdf_{pnn}.png")
    plot(sw, per_line, args.thresh, args.min_intervals, out)


if __name__ == "__main__":
    main()
