#!/usr/bin/env python3
"""Phase 2: compare CSV feeder uids vs BLD_REPLAY_MARK_LOADS from a sim run."""

import argparse
import csv
import re
import sys
from pathlib import Path


def load_csv_feeder_uids(csv_path: Path, max_depth: int) -> set[int]:
    uids: set[int] = set()
    with csv_path.open(newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        legacy = "branch_inst_uid" not in header
        for row in reader:
            if not row:
                continue
            if legacy:
                feeder_uid = int(row[1])
                depth = int(row[2])
            else:
                feeder_uid = int(row[2])
                depth = int(row[3])
            if max_depth == 0 or depth <= max_depth:
                uids.add(feeder_uid)
    return uids


def parse_stat(path: Path, name: str) -> int | None:
    if not path.exists():
        return None
    pat = re.compile(rf"^\s*{re.escape(name)}\s+(\d+)")
    with path.open() as f:
        for line in f:
            m = pat.match(line)
            if m:
                return int(m.group(1))
    return None


def parse_bld_stderr(sim_log: Path) -> dict[str, int | str]:
    out: dict[str, int | str] = {}
    if not sim_log.exists():
        return out
    pat = re.compile(
        r"\[BLD\] Loaded (\d+) replay pairs from (.+) \((\d+) unique feeders on proc 0\)"
    )
    for line in sim_log.read_text().splitlines():
        m = pat.search(line)
        if m:
            out["pairs"] = int(m.group(1))
            out["trace_path"] = m.group(2)
            out["unique_feeders_stderr"] = int(m.group(3))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--csv",
        type=Path,
        default=Path("/home/mgiordan/samsung/bld_traces/astar_34395.csv"),
    )
    ap.add_argument("--max-depth", type=int, default=2)
    ap.add_argument(
        "--statdir",
        type=Path,
        help="Sim output dir containing memory.stat.0.out and sim.log",
    )
    args = ap.parse_args()

    csv_uids = load_csv_feeder_uids(args.csv, args.max_depth)
    print(f"CSV unique feeder uids (depth <= {args.max_depth}): {len(csv_uids):,}")

    if args.statdir:
        mem = args.statdir / "memory.stat.0.out"
        marks = parse_stat(mem, "BLD_REPLAY_MARK_LOADS")
        mark_ops = parse_stat(mem, "BLD_REPLAY_MARK_OPS")
        hit_lat = parse_stat(mem, "BLD_FEEDER_HIT_LATENCY")
        miss_wait = parse_stat(mem, "BLD_FEEDER_MISS_WAITMEM")
        br_exec = parse_stat(args.statdir / "bp.stat.0.out", "BR_EXEC_RESOLVE_TOTAL")

        stderr_info = parse_bld_stderr(args.statdir / "sim.log")
        if stderr_info:
            print(f"stderr pairs: {stderr_info.get('pairs', '?'):,}")
            print(f"stderr unique feeders: {stderr_info.get('unique_feeders_stderr', '?'):,}")
            print(f"stderr trace: {stderr_info.get('trace_path', '?')}")

        for label, val in [
            ("BLD_REPLAY_MARK_OPS", mark_ops),
            ("BLD_REPLAY_MARK_LOADS", marks),
            ("BLD_FEEDER_HIT_LATENCY", hit_lat),
            ("BLD_FEEDER_MISS_WAITMEM", miss_wait),
            ("BR_EXEC_RESOLVE_TOTAL", br_exec),
        ]:
            if val is None:
                print(f"{label}: (missing — sim may not have finished)")
            else:
                print(f"{label}: {val:,}")

        if marks is not None:
            ratio = marks / len(csv_uids) if csv_uids else 0.0
            print(f"mark/csv ratio: {ratio:.4f}")
            if marks < len(csv_uids) * 0.99:
                print("H1a LIKELY: marks << CSV unique feeders")
            elif hit_lat is not None and hit_lat < marks * 0.01:
                print("H1c LIKELY: feeders hit L1 (hit_latency rare)")
            else:
                print("Marks look complete; check hit_latency vs BR_EXEC")

    return 0


if __name__ == "__main__":
    sys.exit(main())
