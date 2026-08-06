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

Parsing is parallelized across a process pool (--jobs). Each worker returns compact
partial aggregates -- a Counter of (S,W) pairs and a per-line (evicted,total) dict -- which
are merged in the parent, so large row data never crosses the process boundary.

Usage:
    td_load_evict_cdf.py <file-or-dir> [...] --thresh 0.5 --out evict.png --jobs 16
    td_load_evict_cdf.py /home/shanen/samsung/td_load_traces --thresh 0.5
"""

import argparse
import csv
import glob
import os
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor


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


def parse_file(path):
    """Worker: parse one CSV into compact partials.

    Returns (sw_counter, per_line) where
      sw_counter[(S,W)] = number of intervals with those integer S,W
      per_line[line]    = [evicted_count, total_count]
    """
    sw = Counter()
    per_line = {}
    try:
        with open(path, newline="") as f:
            r = csv.reader(f)
            next(r, None)  # header: line,distinct_conflicts,ways
            for row in r:
                if len(row) < 3:
                    continue
                try:
                    line = row[0]
                    S = int(row[1])
                    W = int(row[2])
                except ValueError:
                    continue
                if W <= 0:
                    continue
                sw[(S, W)] += 1
                rec = per_line.get(line)
                if rec is None:
                    per_line[line] = [1 if S >= W else 0, 1]
                else:
                    rec[0] += 1 if S >= W else 0
                    rec[1] += 1
    except OSError as e:
        print(f"warning: cannot read {path}: {e}", file=sys.stderr)
    return sw, per_line


def load_parallel(files, jobs):
    sw_total = Counter()
    per_line = defaultdict(lambda: [0, 0])
    jobs = max(1, min(jobs, len(files)))
    if jobs == 1:
        results = (parse_file(f) for f in files)
    else:
        ex = ProcessPoolExecutor(max_workers=jobs)
        results = ex.map(parse_file, files)
    for sw, pl in results:
        sw_total.update(sw)
        for line, (ev, tot) in pl.items():
            rec = per_line[line]
            rec[0] += ev
            rec[1] += tot
    if jobs != 1:
        ex.shutdown()
    return sw_total, per_line


def weighted_cdf(ratio_counts):
    """ratio_counts: dict value->weight. Return (xs, ys) step CDF (weighted)."""
    items = sorted(ratio_counts.items())
    total = sum(w for _, w in items) or 1
    xs, ys, cum = [], [], 0
    for v, w in items:
        cum += w
        xs.append(v)
        ys.append(cum / total)
    return xs, ys


def plain_cdf(values):
    vs = sorted(values)
    n = len(vs) or 1
    return vs, [(i + 1) / n for i in range(len(vs))]


def decimate(xs, ys, max_points=2000):
    """Evenly subsample a monotone CDF to at most max_points (keeps first & last)."""
    n = len(xs)
    if n <= max_points:
        return xs, ys
    step = n / max_points
    idx = sorted({int(i * step) for i in range(max_points)} | {n - 1})
    return [xs[i] for i in idx], [ys[i] for i in idx]


def plot(ratio_counts, p_evict, n_intervals, line_rates, thresh, min_intervals, out_path):
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("(matplotlib not available; skipping plot.)", file=sys.stderr)
        return

    fig, axes = plt.subplots(1, 3, figsize=(18, 5))

    # 1. pooled S/W CDF (event-weighted)
    x, y = weighted_cdf(ratio_counts)
    ax = axes[0]
    ax.plot(x, y, color="#4c78a8")
    ax.axvline(1.0, color="#e45756", linestyle="--", label="S = W (eviction)")
    ax.set_xlim(0, min(4.0, x[-1] if x else 4.0))
    ax.set_ylim(0, 1)
    ax.set_xlabel("S / W  (distinct set-fills per way)")
    ax.set_ylabel("CDF (event-weighted)")
    ax.set_title(f"Pooled S/W  |  P(S≥W)={p_evict:.1%}  (p={thresh}, n={n_intervals})")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=9)

    # 2. per-line eviction-rate CDF
    ax = axes[1]
    if line_rates:
        x2, y2 = decimate(*plain_cdf(line_rates))
        ax.plot(x2, y2, color="#54a24b")
        ax.set_xlim(0, 1)
        ax.set_ylim(0, 1)
    ax.set_xlabel(f"per-line eviction rate  (≥{min_intervals} intervals/line)")
    ax.set_ylabel("CDF over lines")
    ax.set_title(f"Per-line eviction rate  (lines={len(line_rates)})")
    ax.grid(True, alpha=0.3)

    # 3. CCDF of S/W (log-log tail)
    ax = axes[2]
    x, y = weighted_cdf(ratio_counts)
    xt = [v for v in x if v > 0]
    yt = [1.0 - y[i] for i, v in enumerate(x) if v > 0]
    if xt:
        ax.plot(xt, yt, color="#4c78a8")
        ax.set_xscale("log")
        ax.set_yscale("log")
    ax.axvline(1.0, color="#e45756", linestyle="--")
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
    ap.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1, help="parallel parse workers (default: #CPUs)")
    ap.add_argument("--out", default=None, help="output PNG (default: evict_cdf_p<NN>.png next to first input)")
    args = ap.parse_args()

    files = gather_inputs(args.inputs, args.thresh)
    if not files:
        sys.exit(f"error: no line_evict CSVs found for p={args.thresh}.")

    sw_total, per_line = load_parallel(files, args.jobs)
    n_intervals = sum(sw_total.values())
    if n_intervals == 0:
        sys.exit("error: no valid interval rows parsed.")

    # weighted S/W distribution and eviction probability
    ratio_counts = defaultdict(int)
    n_evict = 0
    for (S, W), c in sw_total.items():
        ratio_counts[S / W] += c
        if S >= W:
            n_evict += c
    p_evict = n_evict / n_intervals

    line_rates = [ev / tot for (ev, tot) in per_line.values() if tot >= args.min_intervals]

    print(f"files={len(files)} jobs={min(args.jobs, len(files))}  intervals={n_intervals}  "
          f"distinct lines={len(per_line)}  P(S>=W)={p_evict:.3f}  (p={args.thresh})")

    pnn = f"p{int(100.0 * args.thresh + 0.5):02d}"
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(files[0])), f"evict_cdf_{pnn}.png")
    plot(ratio_counts, p_evict, n_intervals, line_rates, args.thresh, args.min_intervals, out)


if __name__ == "__main__":
    main()
