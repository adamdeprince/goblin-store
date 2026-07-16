#!/usr/bin/env python3
"""Summarize raw CSV artifacts produced by goblin_store_rdma_benchmark."""

from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import os
import statistics
from collections import defaultdict
from typing import Callable


PERCENTILES = (0.50, 0.90, 0.99, 0.999, 0.9999)


def nearest_rank(sorted_values: list[int], probability: float) -> int:
    index = max(0, math.ceil(probability * len(sorted_values)) - 1)
    return sorted_values[min(index, len(sorted_values) - 1)]


def load_metric(paths: list[str], value: Callable[[dict[str, str]], int]) -> list[int]:
    values: list[int] = []
    for path in paths:
        with open(path, newline="", encoding="utf-8") as source:
            rows = csv.DictReader(source)
            values.extend(value(row) for row in rows)
    if not values:
        raise ValueError("latency CSVs contain no samples")
    return values


def summarize(values: list[int]) -> dict[str, float | int]:
    mean = statistics.fmean(values)
    stdev = statistics.stdev(values) if len(values) > 1 else 0.0
    values.sort()
    result: dict[str, float | int] = {
        "samples": len(values),
        "mean_ns": mean,
        "stdev_ns": stdev,
        "min_ns": values[0],
        "max_ns": values[-1],
    }
    for probability in PERCENTILES:
        label = str(probability * 100).rstrip("0").rstrip(".").replace(".", "_")
        result[f"p{label}_ns"] = nearest_rank(values, probability)
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact_dir")
    parser.add_argument("--output", help="JSON output (default: ARTIFACT_DIR/analysis.json)")
    args = parser.parse_args()

    latency_paths = sorted(glob.glob(os.path.join(args.artifact_dir, "latency-rep-*.csv")))
    if not latency_paths:
        raise SystemExit("no latency-rep-*.csv artifacts found")

    metrics: tuple[tuple[str, Callable[[dict[str, str]], int]], ...] = (
        ("ttfb", lambda row: int(row["ttfb_ns"])),
        ("payload_ready", lambda row: int(row["body_ready_ns"])),
        ("overall", lambda row: int(row["total_ns"])),
        ("header_to_payload", lambda row: int(row["body_ready_ns"]) - int(row["ttfb_ns"])),
    )
    latency = {name: summarize(load_metric(latency_paths, extractor))
               for name, extractor in metrics}

    throughput: list[dict[str, int | float]] = []
    for path in glob.glob(os.path.join(args.artifact_dir, "throughput-*-qp.csv")):
        with open(path, newline="", encoding="utf-8") as source:
            row = next(csv.DictReader(source))
        throughput.append({
            "qps": int(row["threads"]),
            "requests": int(row["requests"]),
            "ops_per_second": float(row["qps"]),
            "payload_gib_per_second": float(row["payload_gib_per_second"]),
            "payload_gigabits_per_second": float(row["payload_gigabits_per_second"]),
        })
    throughput.sort(key=lambda row: int(row["qps"]))

    confirmation_values: dict[int, list[dict[str, float]]] = defaultdict(list)
    for path in glob.glob(os.path.join(
            args.artifact_dir, "throughput-*-qp-confirm-*.csv")):
        with open(path, newline="", encoding="utf-8") as source:
            row = next(csv.DictReader(source))
        confirmation_values[int(row["threads"])].append({
            "ops_per_second": float(row["qps"]),
            "payload_gib_per_second": float(row["payload_gib_per_second"]),
            "payload_gigabits_per_second": float(row["payload_gigabits_per_second"]),
        })
    throughput_confirmation: list[dict[str, int | float]] = []
    for qps, rows in sorted(confirmation_values.items()):
        result_row: dict[str, int | float] = {"qps": qps, "repetitions": len(rows)}
        for field in ("ops_per_second", "payload_gib_per_second",
                      "payload_gigabits_per_second"):
            values = [row[field] for row in rows]
            result_row[f"mean_{field}"] = statistics.fmean(values)
            result_row[f"stdev_{field}"] = (
                statistics.stdev(values) if len(values) > 1 else 0.0)
            result_row[f"min_{field}"] = min(values)
            result_row[f"max_{field}"] = max(values)
        throughput_confirmation.append(result_row)

    result = {
        "latency_files": [os.path.basename(path) for path in latency_paths],
        "latency": latency,
        "throughput": throughput,
        "throughput_confirmation": throughput_confirmation,
    }
    output = args.output or os.path.join(args.artifact_dir, "analysis.json")
    with open(output, "w", encoding="utf-8") as destination:
        json.dump(result, destination, indent=2)
        destination.write("\n")

    print("metric       mean_us   p50_us   p90_us   p99_us  p99.9_us  p99.99_us    max_us")
    for name, _ in metrics:
        row = latency[name]
        print(f"{name:14} {row['mean_ns'] / 1000:8.3f} "
              f"{row['p50_ns'] / 1000:8.3f} {row['p90_ns'] / 1000:8.3f} "
              f"{row['p99_ns'] / 1000:8.3f} {row['p99_9_ns'] / 1000:10.3f} "
              f"{row['p99_99_ns'] / 1000:11.3f} {row['max_ns'] / 1000:9.3f}")
    print("\nqps  ops_per_s  payload_GiB_s  payload_Gbit_s")
    for row in throughput:
        print(f"{row['qps']:3d} {row['ops_per_second']:10.1f} "
              f"{row['payload_gib_per_second']:14.3f} "
              f"{row['payload_gigabits_per_second']:15.3f}")
    for row in throughput_confirmation:
        print(f"\n{row['qps']}-QP confirmation ({row['repetitions']} runs): "
              f"{row['mean_payload_gigabits_per_second']:.3f} +/- "
              f"{row['stdev_payload_gigabits_per_second']:.3f} Gbit/s "
              f"(range {row['min_payload_gigabits_per_second']:.3f}-"
              f"{row['max_payload_gigabits_per_second']:.3f})")


if __name__ == "__main__":
    main()
