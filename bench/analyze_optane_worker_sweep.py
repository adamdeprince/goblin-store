#!/usr/bin/env python3
"""Summarize one or more Goblin Optane worker-count sweeps.

The controller and per-worker directories are produced by
``bench/run_optane_worker_sweep.sh``.  The default pass reads only the small run logs and is suitable
for choosing confirmation candidates while a large experiment is in progress.  ``--latency`` also
reads every nanosecond trace with NumPy, computes exact nearest-rank percentiles, and restricts the
resource-monitor samples to each measured request window.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import re
from statistics import median
from typing import Any, Iterable


PERCENTILES = (50.0, 90.0, 99.0, 99.9)
COMPLETION_FIELD = re.compile(r"([A-Za-z_]+)=(\"[^\"]*\"|\S+)")


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--series-dir",
        action="append",
        required=True,
        type=Path,
        help="controller directory; repeat for confirmation replicates",
    )
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--latency",
        action="store_true",
        help="read the large CSV traces and compute latency and exact-window CPU statistics",
    )
    parser.add_argument(
        "--plateau-percent",
        type=float,
        default=2.0,
        help="choose the smallest worker count within this percentage of the best score",
    )
    return parser.parse_args()


def read_config(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if "=" in line and not line.startswith(("sha256", "/")):
                key, value = line.split("=", 1)
                if key and " " not in key:
                    result[key] = value
    return result


def completion(path: Path, expected_requests: int) -> dict[str, int | float | str]:
    selected: dict[str, str] | None = None
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if f"completed={expected_requests}" not in line:
                continue
            selected = {
                match.group(1): match.group(2).strip('"')
                for match in COMPLETION_FIELD.finditer(line)
            }
    if selected is None:
        raise ValueError(f"no completed={expected_requests} record in {path}")

    converted: dict[str, int | float | str] = {}
    for key, value in selected.items():
        try:
            converted[key] = int(value)
            continue
        except ValueError:
            pass
        try:
            converted[key] = float(value)
        except ValueError:
            converted[key] = value
    return converted


def nearest_rank_indices(size: int) -> list[int]:
    return [max(0, math.ceil(percentile / 100.0 * size) - 1) for percentile in PERCENTILES]


def latency_trace(path: Path) -> tuple[dict[str, Any], int, int]:
    try:
        import numpy as np
    except ImportError as error:
        raise RuntimeError("--latency requires NumPy") from error

    # wall_start, connect duration, response TTFB, and TTLB. Loading only integer columns keeps a
    # 6.14-million-request trace below 200 MiB and avoids Python per-row objects.
    data = np.loadtxt(
        path,
        delimiter=",",
        skiprows=1,
        usecols=(6, 7, 12, 14),
        dtype=np.uint64,
        ndmin=2,
    )
    if data.shape[0] == 0:
        raise ValueError(f"empty trace: {path}")
    wall_start = data[:, 0]
    connect = data[:, 1]
    start_ns = int(wall_start.min())
    end_ns = int((wall_start + connect + data[:, 3]).max())
    indices = nearest_rank_indices(data.shape[0])

    result: dict[str, Any] = {"requests": int(data.shape[0])}
    for column, name in ((2, "ttfb_ns"), (3, "ttlb_ns")):
        values = data[:, column].copy()
        mean = float(values.mean())
        values.partition(indices)
        result[name] = {
            "mean": mean,
            **{
                f"p{percentile:g}": int(values[index])
                for percentile, index in zip(PERCENTILES, indices, strict=True)
            },
        }
    return result, start_ns, end_ns


def monitor_window(path: Path, start_ns: int, end_ns: int) -> dict[str, float | int]:
    cpu: list[float] = []
    rss: list[int] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            if row["comm"] != "goblin-store" or not row["cpu_pct"]:
                continue
            timestamp = int(row["timestamp_ns"])
            if start_ns <= timestamp <= end_ns:
                cpu.append(float(row["cpu_pct"]))
                rss.append(int(row["rss_bytes"]))
    if not cpu:
        return {"samples": 0, "cpu_mean": 0.0, "cpu_peak": 0.0, "rss_peak": 0}
    return {
        "samples": len(cpu),
        "cpu_mean": sum(cpu) / len(cpu),
        "cpu_peak": max(cpu),
        "rss_peak": max(rss),
    }


def dispatch_distribution(path: Path) -> dict[str, Any]:
    accepted: list[int] = []
    pattern = re.compile(r"HTTP dispatch worker=\d+ cpu=-?\d+ accepted=(\d+) current=(\d+)")
    if path.is_file():
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if match := pattern.search(line):
                    accepted.append(int(match.group(1)))
                    if int(match.group(2)) != 0:
                        raise ValueError(f"nonzero live connection count at shutdown in {path}")
    return {
        "workers_reported": len(accepted),
        "accepted_min": min(accepted) if accepted else 0,
        "accepted_max": max(accepted) if accepted else 0,
        "accepted_total": sum(accepted),
    }


def analyze_series(series_dir: Path, include_latency: bool) -> list[dict[str, Any]]:
    config = read_config(series_dir / "config.txt")
    series_id = config["series_id"]
    workers = [int(value) for value in config["worker_counts"].split()]
    concurrencies = [int(value) for value in config["concurrencies"].split()]
    result_root = series_dir.parent
    rows: list[dict[str, Any]] = []
    for worker_count in workers:
        run_dir = result_root / f"goblin-workers-{worker_count}-{series_id}"
        prewarm = completion(run_dir / "prewarm.log", 373_165)
        distribution = dispatch_distribution(run_dir / "server.stderr")
        for concurrency in concurrencies:
            row: dict[str, Any] = {
                "series_id": series_id,
                "workers": worker_count,
                "concurrency": concurrency,
                "run_dir": str(run_dir),
                "prewarm": prewarm,
                "dispatch": distribution,
                "measured": completion(run_dir / f"one-pass-c{concurrency}.log", 6_141_630),
            }
            if include_latency:
                trace, start_ns, end_ns = latency_trace(
                    run_dir / f"one-pass-c{concurrency}.csv"
                )
                row["latency"] = trace
                row["monitor"] = monitor_window(
                    run_dir / "monitor/processes.csv", start_ns, end_ns
                )
            rows.append(row)
    return rows


def group_rows(rows: Iterable[dict[str, Any]]) -> dict[tuple[int, int], list[dict[str, Any]]]:
    grouped: dict[tuple[int, int], list[dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault((row["workers"], row["concurrency"]), []).append(row)
    return grouped


def summarized(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for (workers, concurrency), trials in sorted(group_rows(rows).items()):
        rates = [float(trial["measured"]["aggregate_Gbit_s"]) for trial in trials]
        record: dict[str, Any] = {
            "workers": workers,
            "concurrency": concurrency,
            "trials": len(trials),
            "throughput_median_gbit_s": median(rates),
            "throughput_min_gbit_s": min(rates),
            "throughput_max_gbit_s": max(rates),
        }
        if "latency" in trials[0]:
            for metric in ("ttfb_ns", "ttlb_ns"):
                for statistic in ("mean", "p50", "p90", "p99", "p99.9"):
                    record[f"{metric}_{statistic}"] = median(
                        float(trial["latency"][metric][statistic]) for trial in trials
                    )
            record["cpu_mean"] = median(
                float(trial["monitor"]["cpu_mean"]) for trial in trials
            )
            record["cpu_peak"] = max(float(trial["monitor"]["cpu_peak"]) for trial in trials)
        output.append(record)
    return output


def worker_scores(summary: list[dict[str, Any]]) -> list[dict[str, float | int]]:
    best_by_concurrency: dict[int, float] = {}
    for row in summary:
        concurrency = int(row["concurrency"])
        rate = float(row["throughput_median_gbit_s"])
        best_by_concurrency[concurrency] = max(best_by_concurrency.get(concurrency, 0.0), rate)
    normalized: dict[int, list[float]] = {}
    for row in summary:
        normalized.setdefault(int(row["workers"]), []).append(
            float(row["throughput_median_gbit_s"]) / best_by_concurrency[int(row["concurrency"])]
        )
    scores = [
        {
            "workers": workers,
            "geometric_mean_relative_throughput": math.prod(values) ** (1.0 / len(values)),
        }
        for workers, values in normalized.items()
    ]
    return sorted(scores, key=lambda row: (-float(row["geometric_mean_relative_throughput"]),
                                            int(row["workers"])))


def human_us(ns: float) -> str:
    return f"{ns / 1_000.0:.1f}"


def markdown(summary: list[dict[str, Any]], scores: list[dict[str, Any]],
             plateau_percent: float) -> str:
    latency = bool(summary and "ttfb_ns_mean" in summary[0])
    lines = [
        "# Goblin Store worker-count sweep",
        "",
        "Throughput is the median when confirmation series contain repeated worker counts. "
        "The combined score is the geometric mean of throughput relative to the best worker "
        "count at each measured concurrency.",
        "",
        "| Workers | Concurrency | Trials | Median Gbit/s | Range Gbit/s |"
        + (" Mean TTFB us | p99 TTFB us | Mean TTLB us | p99 TTLB us | Mean CPU |" if latency else ""),
        "|---:|---:|---:|---:|---:|" + ("---:|---:|---:|---:|---:|" if latency else ""),
    ]
    for row in summary:
        line = (
            f"| {row['workers']} | {row['concurrency']} | {row['trials']} | "
            f"{row['throughput_median_gbit_s']:.3f} | "
            f"{row['throughput_min_gbit_s']:.3f}-{row['throughput_max_gbit_s']:.3f} |"
        )
        if latency:
            line += (
                f" {human_us(row['ttfb_ns_mean'])} | {human_us(row['ttfb_ns_p99'])} |"
                f" {human_us(row['ttlb_ns_mean'])} | {human_us(row['ttlb_ns_p99'])} |"
                f" {row['cpu_mean']:.1f}% |"
            )
        lines.append(line)

    best = float(scores[0]["geometric_mean_relative_throughput"])
    cutoff = best * (1.0 - plateau_percent / 100.0)
    plateau = sorted(
        int(row["workers"])
        for row in scores
        if float(row["geometric_mean_relative_throughput"]) >= cutoff
    )
    lines.extend(
        [
            "",
            "## Combined throughput score",
            "",
            "| Rank | Workers | Relative score |",
            "|---:|---:|---:|",
        ]
    )
    for rank, row in enumerate(scores, 1):
        lines.append(
            f"| {rank} | {row['workers']} | "
            f"{100.0 * float(row['geometric_mean_relative_throughput']):.2f}% |"
        )
    lines.extend(
        [
            "",
            f"Smallest worker count within {plateau_percent:g}% of the best combined score: "
            f"**{plateau[0]}**.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    args = arguments()
    rows: list[dict[str, Any]] = []
    for series_dir in args.series_dir:
        rows.extend(analyze_series(series_dir, args.latency))
    summary = summarized(rows)
    scores = worker_scores(summary)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    result = {"trials": rows, "summary": summary, "worker_scores": scores}
    (args.output_dir / "analysis.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.output_dir / "analysis.md").write_text(
        markdown(summary, scores, args.plateau_percent), encoding="utf-8"
    )
    (args.output_dir / "top-workers.txt").write_text(
        " ".join(str(row["workers"]) for row in scores[:3]) + "\n", encoding="utf-8"
    )
    print(args.output_dir / "analysis.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
