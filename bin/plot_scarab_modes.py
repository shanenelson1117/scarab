#!/usr/bin/env python3
"""Plot Scarab stat differences across simulation modes.

Reads pre-collected stats from ~/simulations/<experiment>/collected_stats.csv
(produced by scarab-infra's stat_collector) and visualizes how stats differ
across Scarab modes (configs), normalized to a baseline mode.

Example:
  python3 bin/plot_scarab_modes.py new_method_replay \\
      --stat BR_EXEC_RESOLVE_TOTAL --field cycles \\
      --modes all_hit depth_2 depth_5 depth_10 \\
      --baseline baseline \\
      --output /tmp/br_resolve.png

If system python3 has a NumPy/matplotlib mismatch, the script automatically
re-runs itself with ~/miniconda3/envs/scarabinfra/bin/python when available.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

_REEXEC_ENV = "SCARAB_PLOT_PYTHON_OK"
_CONDA_ENVS = ("scarabinfra", "scarab")


def _python_can_plot(python: str) -> bool:
    result = subprocess.run(
        [python, "-c", "import matplotlib.pyplot"],
        capture_output=True,
    )
    return result.returncode == 0


def _conda_python_candidates() -> List[Path]:
    home = Path.home()
    candidates = [
        home / "miniconda3" / "envs" / env / "bin" / "python" for env in _CONDA_ENVS
    ]
    anaconda = home / "anaconda3"
    if anaconda.is_dir():
        for env in _CONDA_ENVS:
            candidates.append(anaconda / "envs" / env / "bin" / "python")
    return candidates


def _maybe_reexec_with_working_python() -> None:
    """Re-exec with a conda python when system matplotlib is broken."""
    if os.environ.get(_REEXEC_ENV):
        return

    current = sys.executable
    if _python_can_plot(current):
        os.environ[_REEXEC_ENV] = "1"
        return

    for candidate in _conda_python_candidates():
        if not candidate.is_file():
            continue
        candidate_str = str(candidate)
        if candidate_str == current:
            continue
        if _python_can_plot(candidate_str):
            os.environ[_REEXEC_ENV] = "1"
            os.execv(candidate_str, [candidate_str] + sys.argv)


def _needs_matplotlib(argv: Sequence[str]) -> bool:
    if any(flag in argv for flag in ("-h", "--help")):
        return False
    return not any(flag in argv for flag in ("--list-modes", "--list-stats"))


if _needs_matplotlib(sys.argv):
    _maybe_reexec_with_working_python()

import numpy as np
import pandas as pd

plt = None


def _import_matplotlib():
    global plt
    if plt is not None:
        return plt
    try:
        import matplotlib.pyplot as pyplot
    except ImportError as exc:
        raise SystemExit(
            "matplotlib failed to import (usually a NumPy 1.x vs 2.x mismatch).\n"
            "Fix options:\n"
            "  1) conda run -n scarabinfra python3 bin/plot_scarab_modes.py ...\n"
            "  2) pip3 install --user --upgrade 'matplotlib>=3.9'\n"
            "  3) pip3 install --user 'numpy<2'  (downgrade NumPy to match old matplotlib)\n"
            f"Original error: {exc}"
        ) from exc
    plt = pyplot
    return plt

SIMULATIONS_ROOT = Path.home() / "simulations"
METADATA_ROWS = {"Configuration", "Workload", "Weight", "Experiment"}

FIELD_SUFFIX = {
    "cycles": "_total_count",
    "cumulative": "_total_count",
    "total": "_total_count",
    "total_count": "_total_count",
    "periodic": "_count",
    "count": "_count",
    "pct": "_pct",
    "percent": "_pct",
    "percentage": "_pct",
}


def expand_user_path(path: str) -> Path:
    return Path(path).expanduser().resolve()


def experiment_csv_path(experiment: str, simulations_root: Path) -> Path:
    return simulations_root / experiment / "collected_stats.csv"


def parse_column_name(column: str) -> Tuple[str, str, str]:
    """Return (mode, workload, simpoint) from a CSV column header."""
    parts = column.rsplit(" ", 1)
    if len(parts) != 2:
        raise ValueError(f"Unexpected column format: {column!r}")
    prefix, simpoint = parts
    mode, workload = prefix.split(" ", 1)
    return mode, workload, simpoint


def resolve_stat_name(
    stat: str,
    field: Optional[str],
    available_stats: Iterable[str],
) -> str:
    """Map a user stat + field to the row name in collected_stats.csv."""
    available = set(available_stats)
    if stat in available:
        return stat

    if field:
        suffix = FIELD_SUFFIX.get(field.lower())
        if suffix is None:
            suffix = f"_{field}"
        candidate = f"{stat}{suffix}"
        if candidate in available:
            return candidate

    # Try common suffixes when field is omitted.
    for suffix in ("_total_count", "_count", "_pct"):
        candidate = f"{stat}{suffix}"
        if candidate in available:
            return candidate

    matches = sorted(s for s in available if s.startswith(stat))
    if len(matches) == 1:
        return matches[0]
    if matches:
        preview = ", ".join(matches[:8])
        more = "" if len(matches) <= 8 else f", ... (+{len(matches) - 8} more)"
        raise SystemExit(
            f"Ambiguous stat {stat!r}. Matching rows include: {preview}{more}\n"
            f"Pass --field ({', '.join(FIELD_SUFFIX)}) or the full stat row name."
        )
    raise SystemExit(f"Stat {stat!r} not found in collected_stats.csv")


def load_experiment(csv_path: Path) -> pd.DataFrame:
    if not csv_path.is_file():
        raise SystemExit(f"Missing collected stats file: {csv_path}")
    return pd.read_csv(csv_path, low_memory=False)


def discover_modes(df: pd.DataFrame) -> List[str]:
    conf_row = df.loc[df["stats"] == "Configuration"].iloc[0]
    modes = []
    seen = set()
    for column in df.columns[3:]:
        mode = str(conf_row[column])
        if mode not in seen:
            seen.add(mode)
            modes.append(mode)
    return modes


def discover_workloads(df: pd.DataFrame, modes: Sequence[str]) -> List[str]:
    wl_row = df.loc[df["stats"] == "Workload"].iloc[0]
    conf_row = df.loc[df["stats"] == "Configuration"].iloc[0]
    workloads = []
    seen = set()
    for column in df.columns[3:]:
        if str(conf_row[column]) not in modes:
            continue
        workload = str(wl_row[column])
        if workload not in seen:
            seen.add(workload)
            workloads.append(workload)
    return workloads


def weighted_sum(values: Sequence[float], weights: Sequence[float]) -> float:
    vals = [float(v) for v in values]
    wts = [float(w) for w in weights]
    if all(math.isnan(v) for v in vals):
        return float("nan")
    total = 0.0
    weight_total = 0.0
    for value, weight in zip(vals, wts):
        if math.isnan(value):
            continue
        total += value * weight
        weight_total += weight
    if weight_total == 0.0:
        return float("nan")
    return total / weight_total


def aggregate_workload_values(
    df: pd.DataFrame,
    stat_name: str,
    mode: str,
    workloads: Sequence[str],
) -> Dict[str, float]:
    stat_row = df.loc[df["stats"] == stat_name]
    weight_row = df.loc[df["stats"] == "Weight"]
    if stat_row.empty:
        raise SystemExit(f"Stat row {stat_name!r} not found.")
    if weight_row.empty:
        raise SystemExit("Weight row missing from collected_stats.csv")

    stat_row = stat_row.iloc[0]
    weight_row = weight_row.iloc[0]
    conf_row = df.loc[df["stats"] == "Configuration"].iloc[0]
    wl_row = df.loc[df["stats"] == "Workload"].iloc[0]

    results: Dict[str, float] = {}
    for workload in workloads:
        columns = [
            column
            for column in df.columns[3:]
            if str(conf_row[column]) == mode and str(wl_row[column]) == workload
        ]
        if not columns:
            continue
        values = [stat_row[column] for column in columns]
        weights = [weight_row[column] for column in columns]
        results[workload] = weighted_sum(values, weights)
    return results


def normalize_values(
    mode_values: Dict[str, float],
    baseline_values: Dict[str, float],
    method: str,
    lower_is_better: bool,
) -> Dict[str, float]:
    normalized: Dict[str, float] = {}
    for workload, value in mode_values.items():
        baseline = baseline_values.get(workload)
        if baseline is None or math.isnan(value) or math.isnan(baseline):
            normalized[workload] = float("nan")
            continue
        if baseline == 0.0:
            normalized[workload] = float("nan")
            continue

        ratio = value / baseline
        if method == "ratio":
            normalized[workload] = ratio
        elif method == "pct_change":
            normalized[workload] = 100.0 * (ratio - 1.0)
        elif method == "delta":
            normalized[workload] = value - baseline
        elif method == "improvement":
            if lower_is_better:
                normalized[workload] = 100.0 * (baseline - value) / baseline
            else:
                normalized[workload] = 100.0 * (value - baseline) / baseline
        else:
            raise ValueError(f"Unknown normalize method: {method}")
    return normalized


def geomean(values: Sequence[float]) -> float:
    clean = [v for v in values if not math.isnan(v) and v > 0]
    if not clean:
        return float("nan")
    return float(np.exp(np.mean(np.log(clean))))


def arithmean(values: Sequence[float]) -> float:
    clean = [v for v in values if not math.isnan(v)]
    if not clean:
        return float("nan")
    return float(np.mean(clean))


def choose_average(values: Sequence[float], stat_name: str) -> float:
    if stat_name.endswith("_pct"):
        return arithmean(values)
    return geomean(values)


def y_label_for(method: str, stat_name: str) -> str:
    if method == "ratio":
        return f"{stat_name} / baseline"
    if method == "pct_change":
        return f"% change in {stat_name} vs baseline"
    if method == "delta":
        return f"Δ {stat_name} vs baseline"
    return f"% improvement in {stat_name} vs baseline"


def plot_grouped_bars(
    workloads: List[str],
    modes: List[str],
    data: Dict[str, Dict[str, float]],
    title: str,
    y_label: str,
    output: Optional[Path],
    show: bool,
    average: bool,
    baseline: str,
) -> None:
    pyplot = _import_matplotlib()
    if average:
        workloads_to_plot = workloads + ["GeoMean"]
        for mode in modes:
            avg = choose_average([data[mode].get(wl, float("nan")) for wl in workloads], title)
            data[mode]["GeoMean"] = avg
    else:
        workloads_to_plot = workloads

    num_modes = len(modes)
    num_workloads = len(workloads_to_plot)
    bar_width = 0.8 / max(num_modes, 1)
    offsets = (np.arange(num_modes) - (num_modes - 1) / 2.0) * bar_width
    x = np.arange(num_workloads)

    fig, ax = pyplot.subplots(figsize=(max(8, num_workloads * 0.45), 5))
    colors = pyplot.cm.Set2(np.linspace(0, 1, max(num_modes, 3)))

    for idx, mode in enumerate(modes):
        heights = [data[mode].get(wl, float("nan")) for wl in workloads_to_plot]
        ax.bar(
            x + offsets[idx],
            heights,
            width=bar_width,
            label=mode,
            color=colors[idx],
            edgecolor="black",
            linewidth=0.5,
        )

    ax.axhline(0.0 if "improvement" in y_label.lower() or "change" in y_label.lower() else 1.0,
               color="gray", linestyle="--", linewidth=0.8)
    ax.set_xticks(x)
    ax.set_xticklabels(workloads_to_plot, rotation=35, ha="right")
    ax.set_ylabel(y_label)
    ax.set_title(title)
    ax.grid(axis="y", alpha=0.3)
    ax.legend(loc="best", title=f"normalized to {baseline}")
    fig.tight_layout()

    if output:
        fig.savefig(output, dpi=150, bbox_inches="tight")
        print(f"Wrote {output}")
    if show or not output:
        pyplot.show()
    pyplot.close(fig)


def plot_heatmap(
    workloads: List[str],
    modes: List[str],
    data: Dict[str, Dict[str, float]],
    title: str,
    y_label: str,
    output: Optional[Path],
    show: bool,
) -> None:
    pyplot = _import_matplotlib()
    matrix = np.array([[data[mode].get(wl, float("nan")) for wl in workloads] for mode in modes])
    fig, ax = pyplot.subplots(figsize=(max(8, len(workloads) * 0.35), max(3, len(modes) * 0.6)))
    im = ax.imshow(matrix, aspect="auto", cmap="RdYlGn_r")
    ax.set_xticks(np.arange(len(workloads)))
    ax.set_xticklabels(workloads, rotation=45, ha="right")
    ax.set_yticks(np.arange(len(modes)))
    ax.set_yticklabels(modes)
    ax.set_title(title)
    fig.colorbar(im, ax=ax, label=y_label)
    fig.tight_layout()

    if output:
        fig.savefig(output, dpi=150, bbox_inches="tight")
        print(f"Wrote {output}")
    if show or not output:
        pyplot.show()
    pyplot.close(fig)


def list_matching_stats(df: pd.DataFrame, pattern: str) -> None:
    regex = re.compile(pattern)
    rows = [name for name in df["stats"] if isinstance(name, str) and regex.search(name)]
    for name in rows[:200]:
        print(name)
    if len(rows) > 200:
        print(f"... ({len(rows) - 200} more)")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plot Scarab stat differences across modes, normalized to a baseline.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Stat fields:
  cycles / cumulative -> <stat>_total_count
  periodic / count    -> <stat>_count
  pct                 -> <stat>_pct

Normalization:
  ratio       mode / baseline (1.0 means identical to baseline)
  pct_change  100 * (mode / baseline - 1)
  improvement % better than baseline (uses lower-is-better for cycle/count stats)
  delta       mode - baseline
""",
    )
    parser.add_argument(
        "experiment",
        nargs="?",
        help="Experiment directory name under ~/simulations (e.g. new_method_replay).",
    )
    parser.add_argument(
        "--simulations-root",
        type=expand_user_path,
        default=SIMULATIONS_ROOT,
        help=f"Root directory containing experiments (default: {SIMULATIONS_ROOT}).",
    )
    parser.add_argument("--stat", help="Stat name, e.g. BR_EXEC_RESOLVE_TOTAL")
    parser.add_argument(
        "--field",
        choices=sorted(FIELD_SUFFIX),
        help="Stat field suffix (cycles, count, pct, ...).",
    )
    parser.add_argument(
        "--modes",
        nargs="+",
        help="Modes/configs to plot. Defaults to all modes except baseline.",
    )
    parser.add_argument(
        "--baseline",
        default="baseline",
        help="Baseline mode for normalization (default: baseline).",
    )
    parser.add_argument(
        "--workloads",
        nargs="+",
        help="Subset of workloads to plot (default: all workloads in selected modes).",
    )
    parser.add_argument(
        "--normalize",
        choices=("ratio", "pct_change", "improvement", "delta"),
        default="ratio",
        help="How to express differences vs baseline (default: ratio).",
    )
    parser.add_argument(
        "--higher-is-better",
        action="store_true",
        help="Treat larger stat values as improvements for --normalize improvement.",
    )
    parser.add_argument(
        "--plot",
        choices=("bars", "heatmap"),
        default="bars",
        help="Plot style (default: grouped bars by workload).",
    )
    parser.add_argument(
        "--average",
        action="store_true",
        help="Add a geomean bar across workloads (arithmean for pct stats).",
    )
    parser.add_argument("--title", help="Plot title.")
    parser.add_argument("--output", "-o", type=expand_user_path, help="Save figure to this path.")
    parser.add_argument("--show", action="store_true", help="Show the plot interactively.")
    parser.add_argument(
        "--list-modes",
        action="store_true",
        help="List modes in the experiment CSV and exit.",
    )
    parser.add_argument(
        "--list-stats",
        metavar="REGEX",
        help="List stat row names matching REGEX and exit.",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if not args.experiment:
        parser.error("experiment name is required unless using --list-stats with a custom workflow")

    csv_path = experiment_csv_path(args.experiment, args.simulations_root)
    df = load_experiment(csv_path)

    if args.list_stats:
        list_matching_stats(df, args.list_stats)
        return 0

    modes = discover_modes(df)
    if args.list_modes:
        print("\n".join(modes))
        return 0

    if not args.stat:
        parser.error("--stat is required")

    stat_name = resolve_stat_name(args.stat, args.field, df["stats"])
    baseline = args.baseline

    if baseline not in modes:
        raise SystemExit(
            f"Baseline mode {baseline!r} not found. Available modes: {', '.join(modes)}"
        )

    compare_modes = args.modes
    if not compare_modes:
        compare_modes = [mode for mode in modes if mode != baseline]
    else:
        missing = [mode for mode in compare_modes if mode not in modes]
        if missing:
            raise SystemExit(f"Unknown modes: {', '.join(missing)}. Available: {', '.join(modes)}")

    workloads = args.workloads or discover_workloads(df, [baseline] + compare_modes)
    if not workloads:
        raise SystemExit("No workloads found for the selected modes.")

    baseline_values = aggregate_workload_values(df, stat_name, baseline, workloads)
    lower_is_better = not args.higher_is_better and (
        stat_name.endswith("_count")
        or stat_name.endswith("_total_count")
        or "cycle" in stat_name.lower()
    )

    normalized: Dict[str, Dict[str, float]] = {}
    for mode in compare_modes:
        mode_values = aggregate_workload_values(df, stat_name, mode, workloads)
        normalized[mode] = normalize_values(
            mode_values,
            baseline_values,
            args.normalize,
            lower_is_better=lower_is_better,
        )

    title = args.title or (
        f"{stat_name} by workload ({args.normalize} vs {baseline}) — {args.experiment}"
    )
    y_label = y_label_for(args.normalize, stat_name)

    if args.plot == "heatmap":
        plot_heatmap(workloads, compare_modes, normalized, title, y_label, args.output, args.show)
    else:
        plot_grouped_bars(
            workloads,
            compare_modes,
            normalized,
            title,
            y_label,
            args.output,
            args.show,
            args.average,
            baseline,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
