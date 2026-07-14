#!/usr/bin/env python3
"""Analyze matched normal/perverse Goblin Store latency traces.

The benchmark uses the same fixed-seed key sequence for normal (NIC-local head RAM) and perverse
(farthest-node head RAM) runs.  This program computes exact full-trace percentiles and an exact
Vargha--Delaney probability of superiority, oriented as the probability that a randomly selected
local latency is lower than a randomly selected remote latency (ties count one half).

Because millions of serial requests are not millions of independent experiments, the program also
splits the matched traces into contiguous blocks (96 by default, approximately one minute each) and
bootstraps the mean within-block A.  That interval is a guard against short-range autocorrelation;
it cannot remove placement-order, thermal, or other whole-run confounding.

Outputs:
  latency_percentiles.csv  exact p50/p90/p99/p99.9/p99.99 and local improvement
  latency_effects.csv      full-trace A, block-bootstrap interval, paired win probability
  latency_analysis.json    machine-readable complete result
  latency_analysis.md      ready-to-include Markdown tables and methodology

Requires NumPy.  SciPy is deliberately not required.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any

import numpy as np


PERCENTILES = (50.0, 90.0, 99.0, 99.9, 99.99)
METRICS = (
    ("response_ttfb", "response TTFB", 2),
    ("payload_ttfb", "payload TTFB", 3),
    ("complete", "total download", 4),
)
CASES = {
    "memcache": ("01-normal-memcache", "03-perverse-memcache"),
    "http": ("02-normal-http", "04-perverse-http"),
}
CSV_COLUMNS = (
    "worker",
    "sample",
    "key_index",
    "query_start_ns",
    "response_first_byte_ns",
    "response_ttfb_ns",
    "payload_first_byte_ns",
    "payload_ttfb_ns",
    "complete_ns",
)
# Loaded matrix columns: key_index, query_start_ns, then the three metrics above.
USECOLS = (2, 3, 5, 7, 8)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path, help="completed first-byte latency run directory")
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="output directory (default: RUN_DIR/analysis)",
    )
    parser.add_argument(
        "--blocks",
        type=int,
        default=96,
        help="contiguous matched blocks for autocorrelation-aware summaries (default: 96)",
    )
    parser.add_argument(
        "--bootstrap-reps",
        type=int,
        default=20_000,
        help="block-bootstrap repetitions (default: 20000)",
    )
    parser.add_argument(
        "--seed",
        type=lambda value: int(value, 0),
        default=0x9E3779B97F4A7C15,
        help="bootstrap PRNG seed, decimal or 0x-prefixed",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=1_000_000,
        help="search chunk for exact A calculation (default: 1000000)",
    )
    return parser.parse_args()


def read_header(path: Path) -> tuple[str, ...]:
    import gzip

    with gzip.open(path, "rt", encoding="ascii", newline="") as stream:
        return tuple(next(csv.reader(stream)))


def load_trace(path: Path) -> np.ndarray:
    if not path.is_file():
        raise FileNotFoundError(path)
    header = read_header(path)
    if header != CSV_COLUMNS:
        raise ValueError(f"unexpected CSV header in {path}: {header!r}")
    print(f"loading {path}", flush=True)
    data = np.loadtxt(
        path,
        delimiter=",",
        dtype=np.int64,
        skiprows=1,
        usecols=USECOLS,
        ndmin=2,
    )
    if data.shape[0] == 0:
        raise ValueError(f"empty trace: {path}")
    return data


def nearest_rank(sorted_values: np.ndarray, percentile: float) -> int:
    """Return the nearest-rank percentile (rank ceil(p*N), one-indexed)."""

    rank = max(1, math.ceil(percentile / 100.0 * sorted_values.size))
    return int(sorted_values[rank - 1])


def probability_local_faster_sorted(
    local_sorted: np.ndarray, remote_sorted: np.ndarray, chunk_size: int
) -> float:
    """Exact P(local < remote) + 0.5 P(tie), with both inputs already sorted."""

    remote_count = int(remote_sorted.size)
    twice_wins = 0
    for start in range(0, int(local_sorted.size), chunk_size):
        chunk = local_sorted[start : start + chunk_size]
        left = np.searchsorted(remote_sorted, chunk, side="left")
        right = np.searchsorted(remote_sorted, chunk, side="right")
        # Each remote value greater than local is one win; a tie is half a win.  Accumulating
        # doubled wins retains exact integer arithmetic for this 17-million-sample experiment.
        doubled = 2 * (remote_count - right) + (right - left)
        twice_wins += int(np.sum(doubled, dtype=np.int64))
    denominator = 2.0 * float(local_sorted.size) * float(remote_count)
    return twice_wins / denominator


def probability_local_faster(
    local: np.ndarray, remote: np.ndarray, chunk_size: int
) -> float:
    return probability_local_faster_sorted(np.sort(local), np.sort(remote), chunk_size)


def vd_magnitude(a_value: float) -> str:
    """Conventional VDA thresholds derived from Cohen's d = 0.2, 0.5, and 0.8."""

    distance = abs(a_value - 0.5)
    if distance < 0.06:
        return "negligible"
    if distance < 0.14:
        return "small"
    if distance < 0.21:
        return "medium"
    return "large"


def bootstrap_mean_ci(
    values: np.ndarray, repetitions: int, rng: np.random.Generator
) -> tuple[float, float, float]:
    if values.size == 0:
        raise ValueError("cannot bootstrap an empty vector")
    means = np.empty(repetitions, dtype=np.float64)
    batch_size = 512
    for start in range(0, repetitions, batch_size):
        count = min(batch_size, repetitions - start)
        indices = rng.integers(0, values.size, size=(count, values.size))
        means[start : start + count] = values[indices].mean(axis=1)
    low, high = np.percentile(means, (2.5, 97.5))
    return float(values.mean()), float(low), float(high)


def contiguous_block_effects(
    local: np.ndarray,
    remote: np.ndarray,
    blocks: int,
    chunk_size: int,
) -> tuple[np.ndarray, np.ndarray]:
    if local.size != remote.size:
        raise ValueError("matched block analysis requires equal trace lengths")
    if blocks < 2 or blocks > local.size:
        raise ValueError(f"invalid block count {blocks} for {local.size} observations")
    edges = np.linspace(0, local.size, blocks + 1, dtype=np.int64)
    block_a = np.empty(blocks, dtype=np.float64)
    paired = np.empty(blocks, dtype=np.float64)
    for block in range(blocks):
        start, end = int(edges[block]), int(edges[block + 1])
        local_block = local[start:end]
        remote_block = remote[start:end]
        block_a[block] = probability_local_faster(local_block, remote_block, chunk_size)
        less = int(np.count_nonzero(local_block < remote_block))
        equal = int(np.count_nonzero(local_block == remote_block))
        paired[block] = (less + 0.5 * equal) / (end - start)
        if (block + 1) % 16 == 0 or block + 1 == blocks:
            print(f"  analyzed temporal block {block + 1}/{blocks}", flush=True)
    return block_a, paired


def percent_improvement(local_ns: int, remote_ns: int) -> float:
    return 100.0 * (remote_ns - local_ns) / remote_ns if remote_ns else 0.0


def analyze_metric(
    local: np.ndarray,
    remote: np.ndarray,
    blocks: int,
    bootstrap_reps: int,
    chunk_size: int,
    rng: np.random.Generator,
) -> dict[str, Any]:
    local_sorted = np.sort(local)
    remote_sorted = np.sort(remote)
    percentile_rows = []
    for percentile in PERCENTILES:
        local_ns = nearest_rank(local_sorted, percentile)
        remote_ns = nearest_rank(remote_sorted, percentile)
        percentile_rows.append(
            {
                "percentile": percentile,
                "local_ns": local_ns,
                "remote_ns": remote_ns,
                "local_improvement_percent": percent_improvement(local_ns, remote_ns),
            }
        )
    full_a = probability_local_faster_sorted(local_sorted, remote_sorted, chunk_size)
    del local_sorted, remote_sorted

    block_a, paired = contiguous_block_effects(local, remote, blocks, chunk_size)
    blocked_mean, blocked_low, blocked_high = bootstrap_mean_ci(block_a, bootstrap_reps, rng)
    _, paired_low, paired_high = bootstrap_mean_ci(paired, bootstrap_reps, rng)
    paired_less = int(np.count_nonzero(local < remote))
    paired_equal = int(np.count_nonzero(local == remote))
    paired_full = (paired_less + 0.5 * paired_equal) / local.size
    return {
        "sample_count_local": int(local.size),
        "sample_count_remote": int(remote.size),
        "percentiles": percentile_rows,
        "vargha_delaney_A_local_faster": full_a,
        "vargha_delaney_magnitude": vd_magnitude(full_a),
        "blocked_A_mean": blocked_mean,
        "blocked_A_bootstrap_95_ci": [blocked_low, blocked_high],
        "paired_local_faster_probability": paired_full,
        "paired_block_bootstrap_95_ci": [paired_low, paired_high],
        "block_A_min": float(block_a.min()),
        "block_A_max": float(block_a.max()),
    }


def analyze_protocol(
    protocol: str,
    local_path: Path,
    remote_path: Path,
    blocks: int,
    bootstrap_reps: int,
    chunk_size: int,
    rng: np.random.Generator,
) -> dict[str, Any]:
    local = load_trace(local_path)
    remote = load_trace(remote_path)
    if local.shape != remote.shape:
        raise ValueError(
            f"{protocol}: trace shapes differ: local {local.shape}, remote {remote.shape}"
        )
    if not np.array_equal(local[:, 0], remote[:, 0]):
        mismatch = int(np.flatnonzero(local[:, 0] != remote[:, 0])[0])
        raise ValueError(f"{protocol}: key sequence first differs at sample {mismatch}")

    result: dict[str, Any] = {
        "local_trace": str(local_path),
        "remote_trace": str(remote_path),
        "sample_count": int(local.shape[0]),
        "key_sequence_matched": True,
        "local_elapsed_seconds": float((local[-1, 1] - local[0, 1]) / 1e9),
        "remote_elapsed_seconds": float((remote[-1, 1] - remote[0, 1]) / 1e9),
        "metrics": {},
    }
    for key, label, column in METRICS:
        print(f"analyzing {protocol} {label}", flush=True)
        metric = analyze_metric(
            local[:, column],
            remote[:, column],
            blocks,
            bootstrap_reps,
            chunk_size,
            rng,
        )
        metric["label"] = label
        result["metrics"][key] = metric
    return result


def percentile_label(value: float) -> str:
    return f"p{value:g}"


def format_percent(value: float) -> str:
    sign = "+" if value > 0 else ""
    return f"{sign}{value:.3f}%"


def write_percentiles_csv(result: dict[str, Any], output: Path) -> None:
    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            (
                "protocol",
                "metric",
                "percentile",
                "local_ns",
                "remote_ns",
                "local_us",
                "remote_us",
                "local_improvement_percent",
            )
        )
        for protocol, protocol_result in result["protocols"].items():
            for metric, metric_result in protocol_result["metrics"].items():
                for row in metric_result["percentiles"]:
                    writer.writerow(
                        (
                            protocol,
                            metric,
                            percentile_label(row["percentile"]),
                            row["local_ns"],
                            row["remote_ns"],
                            f'{row["local_ns"] / 1000.0:.3f}',
                            f'{row["remote_ns"] / 1000.0:.3f}',
                            f'{row["local_improvement_percent"]:.9f}',
                        )
                    )


def write_effects_csv(result: dict[str, Any], output: Path) -> None:
    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            (
                "protocol",
                "metric",
                "samples_per_placement",
                "A_local_faster",
                "magnitude",
                "blocked_A_mean",
                "blocked_A_ci_low",
                "blocked_A_ci_high",
                "paired_local_faster_probability",
                "paired_ci_low",
                "paired_ci_high",
            )
        )
        for protocol, protocol_result in result["protocols"].items():
            for metric, metric_result in protocol_result["metrics"].items():
                a_low, a_high = metric_result["blocked_A_bootstrap_95_ci"]
                p_low, p_high = metric_result["paired_block_bootstrap_95_ci"]
                writer.writerow(
                    (
                        protocol,
                        metric,
                        metric_result["sample_count_local"],
                        f'{metric_result["vargha_delaney_A_local_faster"]:.9f}',
                        metric_result["vargha_delaney_magnitude"],
                        f'{metric_result["blocked_A_mean"]:.9f}',
                        f"{a_low:.9f}",
                        f"{a_high:.9f}",
                        f'{metric_result["paired_local_faster_probability"]:.9f}',
                        f"{p_low:.9f}",
                        f"{p_high:.9f}",
                    )
                )


def markdown_percentile_table(protocol: str, protocol_result: dict[str, Any]) -> str:
    lines = [
        f"### {protocol}",
        "",
        "| metric | percentile | local | remote | local improvement |",
        "|---|---:|---:|---:|---:|",
    ]
    for _, metric_result in protocol_result["metrics"].items():
        for row in metric_result["percentiles"]:
            lines.append(
                "| {label} | {percentile} | {local:.3f} us | {remote:.3f} us | {improvement} |".format(
                    label=metric_result["label"],
                    percentile=percentile_label(row["percentile"]),
                    local=row["local_ns"] / 1000.0,
                    remote=row["remote_ns"] / 1000.0,
                    improvement=format_percent(row["local_improvement_percent"]),
                )
            )
    return "\n".join(lines)


def markdown_effect_table(result: dict[str, Any]) -> str:
    lines = [
        "| protocol | metric | full-trace A: local faster | magnitude | blocked A (95% CI) | paired fixed-sequence probability (95% CI) |",
        "|---|---|---:|---|---:|---:|",
    ]
    for protocol, protocol_result in result["protocols"].items():
        for _, metric_result in protocol_result["metrics"].items():
            a_low, a_high = metric_result["blocked_A_bootstrap_95_ci"]
            p_low, p_high = metric_result["paired_block_bootstrap_95_ci"]
            lines.append(
                "| {protocol} | {label} | {a:.6f} | {magnitude} | {blocked:.6f} [{a_low:.6f}, {a_high:.6f}] | {paired:.6f} [{p_low:.6f}, {p_high:.6f}] |".format(
                    protocol=protocol,
                    label=metric_result["label"],
                    a=metric_result["vargha_delaney_A_local_faster"],
                    magnitude=metric_result["vargha_delaney_magnitude"],
                    blocked=metric_result["blocked_A_mean"],
                    a_low=a_low,
                    a_high=a_high,
                    paired=metric_result["paired_local_faster_probability"],
                    p_low=p_low,
                    p_high=p_high,
                )
            )
    return "\n".join(lines)


def write_markdown(result: dict[str, Any], output: Path) -> None:
    sections = [
        "# Normal versus perverse NUMA latency analysis",
        "",
        f"Each placement contains **{result['sample_count_per_case']:,}** serial requests per protocol. "
        "Local is the normal node-0 head arena; remote is `--perverse`, which leaves the worker and "
        "direct-attached NIC on node 0 while putting the preferred head arena on farthest node 2. "
        "Positive improvement means local placement was faster: `(remote - local) / remote`.",
        "",
        "The tables use exact nearest-rank percentiles over all samples. Response TTFB is query send "
        "to the first protocol byte; payload TTFB is query send to the first object byte; total "
        "download ends after the complete 256 KiB response.",
        "",
        "## Percentiles",
        "",
    ]
    for protocol in ("memcache", "http"):
        sections.append(markdown_percentile_table(protocol, result["protocols"][protocol]))
        sections.append("")
    sections.extend(
        (
            "## Probability of superiority",
            "",
            "`A` is oriented as `P(local latency < remote latency) + 0.5 P(tie)`: 0.5 means no "
            "distributional advantage and values above 0.5 favor local RAM. The full-trace A is "
            "exact. The blocked estimate divides the matched fixed-seed sequence into "
            f"{result['analysis']['blocks']} contiguous blocks and bootstraps their mean; its interval "
            "reduces the false precision caused by treating adjacent serial requests as independent.",
            "",
            markdown_effect_table(result),
            "",
            "A blocked interval excluding 0.5 says that the two recorded traces are statistically "
            "distinguishable at that time scale. It does **not** by itself prove that NUMA locality "
            "caused the whole difference: normal and perverse were long sequential runs rather than "
            "randomly interleaved placements, so thermal drift and other whole-run effects remain "
            "possible confounders.",
            "",
        )
    )
    output.write_text("\n".join(sections), encoding="utf-8")


def main() -> int:
    args = parse_args()
    if args.blocks < 2:
        raise SystemExit("--blocks must be at least 2")
    if args.bootstrap_reps < 100:
        raise SystemExit("--bootstrap-reps must be at least 100")
    if args.chunk_size < 1:
        raise SystemExit("--chunk-size must be positive")

    run_dir = args.run_dir.resolve()
    output_dir = (args.output_dir or run_dir / "analysis").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.seed)

    result: dict[str, Any] = {
        "run_dir": str(run_dir),
        "analysis": {
            "percentile_method": "nearest rank: ceil(p*N)",
            "A_orientation": "P(local < remote) + 0.5*P(tie)",
            "blocks": args.blocks,
            "bootstrap_repetitions": args.bootstrap_reps,
            "bootstrap_seed": args.seed,
            "chunk_size": args.chunk_size,
            "vargha_delaney_magnitude_abs_A_minus_half_thresholds": [0.06, 0.14, 0.21],
        },
        "protocols": {},
    }
    sample_counts = set()
    for protocol, (local_case, remote_case) in CASES.items():
        protocol_result = analyze_protocol(
            protocol,
            run_dir / local_case / "samples.csv.gz",
            run_dir / remote_case / "samples.csv.gz",
            args.blocks,
            args.bootstrap_reps,
            args.chunk_size,
            rng,
        )
        result["protocols"][protocol] = protocol_result
        sample_counts.add(protocol_result["sample_count"])
    if len(sample_counts) != 1:
        raise ValueError(f"protocol sample counts differ: {sorted(sample_counts)}")
    result["sample_count_per_case"] = sample_counts.pop()

    json_path = output_dir / "latency_analysis.json"
    json_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_percentiles_csv(result, output_dir / "latency_percentiles.csv")
    write_effects_csv(result, output_dir / "latency_effects.csv")
    write_markdown(result, output_dir / "latency_analysis.md")
    print(f"analysis complete: {output_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
