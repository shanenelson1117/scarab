#!/usr/bin/env python3
"""
Build the per-marked-key "exceed / access" hashmap from the per-load memory-boundness
record CSVs, and optionally plot a CDF.

Input: the record files written by the td_load_track record pass, one row per retired
load instance:
    <trace>_mem_bound_loads_pc.csv     ->  pc,mem_bound_fraction
    <trace>_mem_bound_loads_addr.csv   ->  addr,mem_bound_fraction

For each record file (i.e. each simpoint) and a threshold p, we group rows by key
(PC or data address), then for every MARKED key -- one whose mean fraction > p -- emit:
    accesses = number of dynamic instances of that key
    exceeds  = number of those instances whose own fraction > p
    avg_ratio = mean fraction over the key's instances
This is identical to what the in-simulator --td_load_exceed_track mode produces, but
computed directly from the already-recorded per-instance data (no second simulation).

Output per input file (unless --no-write):
    <trace>_mem_bound_exceed_<key>_p<NN>.csv  ->  <key>,exceeds,accesses,avg_ratio

With --cdf PATH it also writes a CDF plot over all processed marked keys of the per-key
exceed rate (exceeds/accesses): both an unweighted CDF (fraction of keys) and an
access-weighted CDF (fraction of accesses).

Usage:
    # one simpoint's record file
    td_load_exceed_cdf.py /path/GemsFDTD_..._mem_bound_loads_addr.csv --key addr --thresh 0.5

    # a whole directory of record files (the shared --td_load_dir), + CDF
    td_load_exceed_cdf.py /home/shanen/samsung/td_load_traces --key pc --thresh 0.5 \\
        --cdf /home/shanen/samsung/td_load_traces/exceed_cdf_pc_p50.png
"""

import argparse
import csv
import glob
import math
import os
import sys
from collections import defaultdict


def record_glob(key):
    return f"*mem_bound_loads_{key}.csv"


def gather_inputs(paths, key):
    """Expand files/dirs into a list of record CSV paths for the requested key."""
    out = []
    for p in paths:
        if os.path.isdir(p):
            out.extend(sorted(glob.glob(os.path.join(p, record_glob(key)))))
        elif os.path.isfile(p):
            out.append(p)
        else:
            print(f"warning: {p} not found; skipping", file=sys.stderr)
    return out


def exceed_output_path(in_path, key, thresh, out_dir):
    """<trace>_mem_bound_loads_<key>.csv -> <trace>_mem_bound_exceed_<key>_p<NN>.csv"""
    base = os.path.basename(in_path)
    pnn = f"p{int(100.0 * thresh + 0.5):02d}"
    marker = f"mem_bound_loads_{key}"
    repl = f"mem_bound_exceed_{key}_{pnn}"
    name = base.replace(marker, repl) if marker in base else f"{base}.exceed_{key}_{pnn}.csv"
    return os.path.join(out_dir or os.path.dirname(os.path.abspath(in_path)), name)


def process_file(in_path, key, thresh):
    """Return {key_value: (exceeds, accesses, avg_ratio)} for MARKED keys in one record CSV."""
    fracs = defaultdict(list)
    with open(in_path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)  # 'pc,mem_bound_fraction' / 'addr,mem_bound_fraction'
        for row in reader:
            if len(row) < 2:
                continue
            try:
                frac = float(row[1])
            except ValueError:
                continue
            if math.isfinite(frac):
                fracs[row[0].strip()].append(frac)

    marked = {}
    for k, fs in fracs.items():
        mean = sum(fs) / len(fs)
        if mean > thresh:  # marked
            exceeds = sum(1 for x in fs if x > thresh)
            marked[k] = (exceeds, len(fs), mean)
    return marked


def write_exceed_csv(path, key, marked):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow([key, "exceeds", "accesses", "avg_ratio"])
        for k in sorted(marked):
            exceeds, accesses, avg = marked[k]
            w.writerow([k, exceeds, accesses, f"{avg:.6f}"])


def plot_cdf(all_keys, key, thresh, out_path):
    """all_keys: list of (exceed_rate, accesses). Plot unweighted + access-weighted CDF."""
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("(matplotlib not available; skipping CDF plot.)", file=sys.stderr)
        return
    if not all_keys:
        print("(no marked keys; nothing to plot)", file=sys.stderr)
        return

    rates = sorted(r for r, _a in all_keys)
    n = len(rates)
    y_keys = [(i + 1) / n for i in range(n)]

    weighted = sorted(all_keys, key=lambda t: t[0])
    total_acc = sum(a for _r, a in weighted) or 1
    cum, y_acc, x_acc = 0.0, [], []
    for r, a in weighted:
        cum += a
        x_acc.append(r)
        y_acc.append(cum / total_acc)

    fig, ax = plt.subplots(figsize=(7, 5))
    ax.plot(rates, y_keys, label="unweighted (fraction of keys)", color="#4c78a8")
    ax.plot(x_acc, y_acc, label="access-weighted (fraction of accesses)", color="#f58518")
    ax.set_xlabel(f"per-key exceed rate  (exceeds / accesses, p={thresh})")
    ax.set_ylabel("CDF")
    ax.set_title(f"td_load exceed-rate CDF over marked {key} keys  (n={n})")
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=9)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    print(f"CDF written to {out_path}")


def main():
    ap = argparse.ArgumentParser(description="Build exceed/access hashmap from record CSVs and optional CDF.")
    ap.add_argument("inputs", nargs="+", help="record CSV file(s) or directory(ies) containing them")
    ap.add_argument("--key", choices=["pc", "addr"], default="pc", help="key type (default: pc)")
    ap.add_argument("--thresh", type=float, default=0.5, help="threshold p (default: 0.5)")
    ap.add_argument("--out-dir", default=None, help="dir for per-file exceed CSVs (default: alongside input)")
    ap.add_argument("--no-write", action="store_true", help="do not write per-file exceed CSVs")
    ap.add_argument("--cdf", default=None, help="also write an aggregate CDF plot to this PNG path")
    args = ap.parse_args()

    files = gather_inputs(args.inputs, args.key)
    if not files:
        sys.exit(f"error: no record CSVs found (looking for {record_glob(args.key)}).")

    all_keys = []          # (exceed_rate, accesses) across all files, for the CDF
    n_files = n_marked = 0
    for in_path in files:
        marked = process_file(in_path, args.key, args.thresh)
        n_files += 1
        n_marked += len(marked)
        if not args.no_write:
            out = exceed_output_path(in_path, args.key, args.thresh, args.out_dir)
            write_exceed_csv(out, args.key, marked)
            print(f"{os.path.basename(in_path)}: {len(marked)} marked keys -> {os.path.basename(out)}")
        for exceeds, accesses, _avg in marked.values():
            if accesses > 0:
                all_keys.append((exceeds / accesses, accesses))

    print(f"\nprocessed {n_files} file(s), {n_marked} marked-key rows total (key={args.key}, p={args.thresh})")
    if args.cdf:
        plot_cdf(all_keys, args.key, args.thresh, args.cdf)


if __name__ == "__main__":
    main()
