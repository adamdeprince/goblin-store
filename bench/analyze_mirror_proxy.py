#!/usr/bin/env python3
"""Analyze matched Vinyl/Goblin warm-cache HTTP benchmark traces.

The fixed-seed request sequence is validated before any comparison.  Per-request throughput is
received payload bytes divided by the request-to-last-byte interval, in decimal MB/s.  The reported
Vargha--Delaney A is oriented so values above 0.5 always favor Goblin Store: lower is better for
latency and higher is better for throughput.
"""

from __future__ import annotations

import argparse
from bisect import bisect_left, bisect_right
import csv
import json
import math
from pathlib import Path
from typing import Any, Iterable


CONCURRENCY = (1, 2, 3, 5, 8, 11)
PERCENTILES = (50.0, 75.0, 90.0, 95.0, 99.0, 99.9, 99.99)
METRICS = (
    ("response_ttfb_ns", "response TTFB"),
    ("body_ttfb_ns", "payload TTFB"),
    ("ttlb_ns", "total request / TTLB"),
    ("body_transfer_ns", "body transfer"),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vinyl-dir", required=True, type=Path)
    parser.add_argument("--goblin-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--concurrency",
        nargs="+",
        type=int,
        default=list(CONCURRENCY),
        help="concurrency cases [1 2 3 5 8 11]",
    )
    return parser.parse_args()


def load(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        raise FileNotFoundError(path)
    rows: list[dict[str, Any]] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for raw in csv.DictReader(stream):
            row: dict[str, Any] = dict(raw)
            for key in (
                "sequence",
                "repetition",
                "object_index",
                "worker",
                "connection",
                "new_connection",
                "wall_start_ns",
                "connect_ns",
                "query_start_ns",
                "response_first_byte_ns",
                "body_first_byte_ns",
                "complete_ns",
                "response_ttfb_ns",
                "body_ttfb_ns",
                "ttlb_ns",
                "body_transfer_ns",
                "expected_bytes",
                "received_bytes",
                "status",
            ):
                row[key] = int(row[key])
            rows.append(row)
    if not rows:
        raise ValueError(f"empty trace: {path}")
    errors = [row for row in rows if row["error"]]
    if errors:
        sample = "; ".join(f"{row['sequence']}:{row['error']}" for row in errors[:5])
        raise ValueError(f"{path} contains {len(errors)} failed requests: {sample}")
    for row in rows:
        if row["status"] != 200 or row["expected_bytes"] != row["received_bytes"]:
            raise ValueError(f"invalid successful row in {path}: sequence {row['sequence']}")
    rows.sort(key=lambda row: row["sequence"])
    if [row["sequence"] for row in rows] != list(range(len(rows))):
        raise ValueError(f"sequence is not contiguous in {path}")
    return rows


def nearest_rank(values: Iterable[int | float], percentile: float) -> int | float:
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile / 100.0 * len(ordered)))
    return ordered[rank - 1]


def degradation_rank(values: Iterable[int | float], percentile: float) -> int | float:
    """Return a higher-is-worse percentile for a higher-is-better measurement.

    For example, P99.9 throughput is the lower 0.1% rate. This keeps the percentile direction
    consistent with latency: increasing percentile always moves toward the worse tail.
    """
    ordered = sorted(values, reverse=True)
    rank = max(1, math.ceil(percentile / 100.0 * len(ordered)))
    return ordered[rank - 1]


def probability_goblin_better(
    goblin: list[int | float], vinyl: list[int | float], *, higher_is_better: bool
) -> float:
    ordered_vinyl = sorted(vinyl)
    twice_wins = 0
    for value in goblin:
        left = bisect_left(ordered_vinyl, value)
        right = bisect_right(ordered_vinyl, value)
        wins = left if higher_is_better else len(ordered_vinyl) - right
        twice_wins += 2 * wins + (right - left)
    return twice_wins / (2.0 * len(goblin) * len(vinyl))


def magnitude(a_value: float) -> str:
    distance = abs(a_value - 0.5)
    if distance < 0.06:
        return "negligible"
    if distance < 0.14:
        return "small"
    if distance < 0.21:
        return "medium"
    return "large"


def paired_probability(
    goblin: list[int | float], vinyl: list[int | float], *, higher_is_better: bool
) -> float:
    if higher_is_better:
        wins = sum(left > right for left, right in zip(goblin, vinyl, strict=True))
    else:
        wins = sum(left < right for left, right in zip(goblin, vinyl, strict=True))
    ties = sum(left == right for left, right in zip(goblin, vinyl, strict=True))
    return (wins + 0.5 * ties) / len(goblin)


def percent_improvement(goblin: int | float, vinyl: int | float) -> float:
    return 100.0 * (vinyl - goblin) / vinyl if vinyl else 0.0


def request_megabytes_per_second(row: dict[str, Any]) -> float:
    """Return payload MB/s from request send start through receipt of the last byte."""
    if row["ttlb_ns"] <= 0:
        raise ValueError(f"non-positive request-to-last-byte interval at sequence {row['sequence']}")
    # bytes / 1,000,000 divided by nanoseconds / 1,000,000,000
    return row["received_bytes"] * 1_000.0 / row["ttlb_ns"]


def summarize_proxy(rows: list[dict[str, Any]]) -> dict[str, Any]:
    start = min(row["query_start_ns"] for row in rows)
    end = max(row["complete_ns"] for row in rows)
    elapsed = end - start
    total_bytes = sum(row["received_bytes"] for row in rows)
    return {
        "requests": len(rows),
        "bytes": total_bytes,
        "elapsed_ns": elapsed,
        "aggregate_gbit_s": total_bytes * 8.0 / elapsed if elapsed else 0.0,
        "new_connections": sum(row["new_connection"] for row in rows),
        "metrics": {
            key: {f"p{percentile:g}": nearest_rank((row[key] for row in rows), percentile)
                  for percentile in PERCENTILES}
            for key, _ in METRICS
        },
        "request_throughput_MB_s": {
            f"p{percentile:g}": degradation_rank(
                (request_megabytes_per_second(row) for row in rows), percentile
            )
            for percentile in PERCENTILES
        },
    }


def analyze_case(vinyl: list[dict[str, Any]], goblin: list[dict[str, Any]]) -> dict[str, Any]:
    if len(vinyl) != len(goblin):
        raise ValueError("matched traces have different row counts")
    for left, right in zip(vinyl, goblin, strict=True):
        identity = ("sequence", "repetition", "object_index", "expected_bytes", "path")
        if any(left[key] != right[key] for key in identity):
            raise ValueError(f"fixed-seed trace mismatch at sequence {left['sequence']}")

    result: dict[str, Any] = {
        "vinyl": summarize_proxy(vinyl),
        "goblin": summarize_proxy(goblin),
        "comparison": {},
    }
    for key, label in METRICS:
        vinyl_values = [row[key] for row in vinyl]
        goblin_values = [row[key] for row in goblin]
        percentiles: dict[str, Any] = {}
        for percentile in PERCENTILES:
            vinyl_value = nearest_rank(vinyl_values, percentile)
            goblin_value = nearest_rank(goblin_values, percentile)
            percentiles[f"p{percentile:g}"] = {
                "vinyl_ns": vinyl_value,
                "goblin_ns": goblin_value,
                "goblin_improvement_percent": percent_improvement(goblin_value, vinyl_value),
            }
        a_value = probability_goblin_better(
            goblin_values, vinyl_values, higher_is_better=False
        )
        result["comparison"][key] = {
            "label": label,
            "percentiles": percentiles,
            "vargha_delaney_a_goblin_faster": a_value,
            "magnitude": magnitude(a_value),
            "paired_probability_goblin_faster": paired_probability(
                goblin_values, vinyl_values, higher_is_better=False
            ),
        }

    vinyl_request_rates = [request_megabytes_per_second(row) for row in vinyl]
    goblin_request_rates = [request_megabytes_per_second(row) for row in goblin]
    rate_percentiles: dict[str, Any] = {}
    for percentile in PERCENTILES:
        vinyl_value = degradation_rank(vinyl_request_rates, percentile)
        goblin_value = degradation_rank(goblin_request_rates, percentile)
        rate_percentiles[f"p{percentile:g}"] = {
            "vinyl_MB_s": vinyl_value,
            "goblin_MB_s": goblin_value,
            "goblin_improvement_percent": (
                100.0 * (goblin_value - vinyl_value) / vinyl_value if vinyl_value else 0.0
            ),
        }
    rate_a_value = probability_goblin_better(
        goblin_request_rates, vinyl_request_rates, higher_is_better=True
    )
    result["comparison"]["request_throughput_MB_s"] = {
        "label": "per-request throughput",
        "percentiles": rate_percentiles,
        "vargha_delaney_a_goblin_higher": rate_a_value,
        "magnitude": magnitude(rate_a_value),
        "paired_probability_goblin_higher": paired_probability(
            goblin_request_rates, vinyl_request_rates, higher_is_better=True
        ),
    }
    vinyl_rate = result["vinyl"]["aggregate_gbit_s"]
    goblin_rate = result["goblin"]["aggregate_gbit_s"]
    result["throughput_improvement_percent"] = (
        100.0 * (goblin_rate - vinyl_rate) / vinyl_rate if vinyl_rate else 0.0
    )
    return result


def human_duration(ns: int) -> str:
    if ns < 1_000:
        return f"{ns} ns"
    if ns < 1_000_000:
        return f"{ns / 1_000:.3f} us"
    if ns < 1_000_000_000:
        return f"{ns / 1_000_000:.3f} ms"
    return f"{ns / 1_000_000_000:.3f} s"


def markdown(analysis: dict[str, Any], concurrencies: list[int]) -> str:
    lines = [
        "# Mirror proxy benchmark analysis",
        "",
        "All measurements are ordinary HTTP over TCP/IPoIB. Native Goblin RDMA is not used. "
        "Latency A values are `P(Goblin latency < Vinyl latency) + 0.5*P(tie)`. "
        "Throughput A values reverse the comparison so that values above 0.5 always favor Goblin. "
        "Percentiles use the nearest-rank definition.",
        "",
        "## Throughput",
        "",
        "| Concurrency | Requests | Vinyl Gbit/s | Goblin Gbit/s | Goblin improvement |",
        "|---:|---:|---:|---:|---:|",
    ]
    for concurrency in concurrencies:
        case = analysis[str(concurrency)]
        vinyl = case["vinyl"]["aggregate_gbit_s"]
        goblin = case["goblin"]["aggregate_gbit_s"]
        improvement = 100.0 * (goblin - vinyl) / vinyl if vinyl else 0.0
        lines.append(
            f"| {concurrency} | {case['vinyl']['requests']:,} | {vinyl:.3f} | "
            f"{goblin:.3f} | {improvement:+.2f}% |"
        )
    for key, label in METRICS:
        lines.extend(
            [
                "",
                f"## {label}",
                "",
                "| Concurrency | Percentile | Vinyl | Goblin | Goblin improvement | "
                "A (Goblin faster) | Paired P(Goblin faster) |",
                "|---:|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for concurrency in concurrencies:
            comparison = analysis[str(concurrency)]["comparison"][key]
            for percentile in PERCENTILES:
                row = comparison["percentiles"][f"p{percentile:g}"]
                lines.append(
                    f"| {concurrency} | p{percentile:g} | {human_duration(row['vinyl_ns'])} | "
                    f"{human_duration(row['goblin_ns'])} | "
                    f"{row['goblin_improvement_percent']:+.2f}% | "
                    f"{comparison['vargha_delaney_a_goblin_faster']:.4f} "
                    f"({comparison['magnitude']}) | "
                    f"{comparison['paired_probability_goblin_faster']:.4f} |"
                )
    lines.extend(
        [
            "",
            "## Per-request throughput",
            "",
            "Each request rate is payload bytes divided by `complete_ns - query_start_ns` "
            "(`ttlb_ns`). MB/s is decimal: 1 MB = 1,000,000 bytes. Throughput uses a "
            "latency-style degradation percentile: P99.9 is the lower 0.1% rate, so increasing "
            "percentiles move toward the slow tail.",
            "",
            "| Concurrency | Percentile | Vinyl MB/s | Goblin MB/s | Goblin improvement | "
            "A (Goblin higher) | Paired P(Goblin higher) |",
            "|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for concurrency in concurrencies:
        comparison = analysis[str(concurrency)]["comparison"]["request_throughput_MB_s"]
        for percentile in PERCENTILES:
            row = comparison["percentiles"][f"p{percentile:g}"]
            lines.append(
                f"| {concurrency} | p{percentile:g} | {row['vinyl_MB_s']:.3f} | "
                f"{row['goblin_MB_s']:.3f} | "
                f"{row['goblin_improvement_percent']:+.2f}% | "
                f"{comparison['vargha_delaney_a_goblin_higher']:.4f} "
                f"({comparison['magnitude']}) | "
                f"{comparison['paired_probability_goblin_higher']:.4f} |"
            )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    analysis: dict[str, Any] = {}
    for concurrency in args.concurrency:
        vinyl = load(args.vinyl_dir / f"c{concurrency}.csv")
        goblin = load(args.goblin_dir / f"c{concurrency}.csv")
        analysis[str(concurrency)] = analyze_case(vinyl, goblin)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "analysis.json").write_text(
        json.dumps(analysis, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.output_dir / "analysis.md").write_text(
        markdown(analysis, args.concurrency), encoding="utf-8"
    )
    print(args.output_dir / "analysis.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
