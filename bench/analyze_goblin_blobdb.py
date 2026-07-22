#!/usr/bin/env python3
"""Summarize Goblin Store/BlobDB per-request latency CSV output."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from array import array
from pathlib import Path


PERCENTILES = (50.0, 75.0, 90.0, 95.0, 99.0, 99.9, 99.99)


def percentile_name(value: float) -> str:
    return "p" + f"{value:g}".replace(".", "_") + "_ns"


def nearest_rank(sorted_values: list[int], percentile: float) -> int:
    rank = max(1, math.ceil((percentile / 100.0) * len(sorted_values)))
    return sorted_values[rank - 1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reads", type=Path, help="reads.csv from bench_goblin_and_blobdb")
    parser.add_argument("--output", type=Path, help="write summary CSV here (default: stdout)")
    parser.add_argument(
        "--split-size",
        type=int,
        default=16 * 1024,
        help="split results below/at-or-above this object size [16384]",
    )
    args = parser.parse_args()

    series: dict[tuple[str, str, str], dict[str, array[int]]] = {}
    with args.reads.open(newline="") as source:
        reader = csv.DictReader(source)
        required = {
            "engine",
            "expected_bytes",
            "io_mode",
            "time_to_first_byte_ns",
            "time_to_end_ns",
            "status",
        }
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            parser.error(f"{args.reads} does not have the expected columns")
        for row in reader:
            if row["status"] != "ok":
                continue
            expected_bytes = int(row["expected_bytes"])
            size_class = (
                f"lt_{args.split_size}"
                if expected_bytes < args.split_size
                else f"ge_{args.split_size}"
            )
            for grouping in ("all", size_class):
                key = (row["engine"], row["io_mode"], grouping)
                values = series.setdefault(key, {"ttfb": array("Q"), "tte": array("Q")})
                values["ttfb"].append(int(row["time_to_first_byte_ns"]))
                values["tte"].append(int(row["time_to_end_ns"]))

    fieldnames = ["engine", "io_mode", "size_class", "metric", "count", "mean_ns"]
    fieldnames.extend(percentile_name(value) for value in PERCENTILES)
    fieldnames.append("max_ns")

    destination = args.output.open("w", newline="") if args.output else None
    output = destination if destination is not None else sys.stdout
    try:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        for (engine, io_mode, size_class), metrics in sorted(series.items()):
            for metric, compact_values in metrics.items():
                ordered = sorted(compact_values)
                if not ordered:
                    continue
                row: dict[str, int | str] = {
                    "engine": engine,
                    "io_mode": io_mode,
                    "size_class": size_class,
                    "metric": metric,
                    "count": len(ordered),
                    "mean_ns": round(sum(ordered) / len(ordered)),
                    "max_ns": ordered[-1],
                }
                for value in PERCENTILES:
                    row[percentile_name(value)] = nearest_rank(ordered, value)
                writer.writerow(row)
    finally:
        if destination is not None:
            destination.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
