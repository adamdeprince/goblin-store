#!/usr/bin/env python3
"""Analyze baseline/candidate ABBA channel micro-latency traces.

The expected traces are produced by ``bench/channel_micro_latency.sh`` and are named:

* ``01-baseline-{memcache,http}.csv[.gz]``
* ``02-candidate-{memcache,http}.csv[.gz]``
* ``03-candidate-{memcache,http}.csv[.gz]``
* ``04-baseline-{memcache,http}.csv[.gz]``

Runs 01/02 and 04/03 form two temporally adjacent matched replicate pairs.  Within each pair the
program requires the complete fixed-seed key-index sequence to match exactly.  It reports exact
nearest-rank percentiles, an exact Vargha--Delaney A oriented as candidate-faster, and a paired
fixed-sequence win probability.  Confidence intervals resample summaries of fixed, nonoverlapping
temporal blocks within each replicate, reducing request-level pseudoreplication and sensitivity to
short-range autocorrelation.

Outputs (under TRACE_DIR/analysis unless --output-dir is supplied):

* channel_micro_latency_analysis.json
* channel_micro_latency_percentiles.csv
* channel_micro_latency_effects.csv
* channel_micro_latency_analysis.md

Requires NumPy.  SciPy is deliberately not required.
"""

from __future__ import annotations

import argparse
import csv
import gzip
import json
import math
from pathlib import Path
from typing import Any, TextIO

import numpy as np


PERCENTILES = (50.0, 90.0, 99.0, 99.9, 99.99)
PROTOCOLS = ("memcache", "http")
METRICS = (
    ("response_ttfb", "response TTFB", 2),
    ("payload_ttfb", "payload TTFB", 3),
    ("complete", "total download", 4),
)
REPLICATES = (
    {
        "id": "replicate_1",
        "label": "01 baseline vs 02 candidate",
        "baseline": "01-baseline",
        "candidate": "02-candidate",
    },
    {
        "id": "replicate_2",
        "label": "04 baseline vs 03 candidate",
        "baseline": "04-baseline",
        "candidate": "03-candidate",
    },
)
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
# Loaded matrix columns: key_index, query_start_ns, response TTFB, payload TTFB, complete.
USECOLS = (2, 3, 5, 7, 8)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "trace_dir",
        type=Path,
        help="directory containing the eight raw or gzip-compressed ABBA CSV traces",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="output directory (default: TRACE_DIR/analysis)",
    )
    parser.add_argument(
        "--blocks",
        type=int,
        default=96,
        help="contiguous temporal blocks per matched replicate (default: 96)",
    )
    parser.add_argument(
        "--bootstrap-reps",
        type=int,
        default=20_000,
        help="temporal-block bootstrap repetitions (default: 20000)",
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


def open_text(path: Path) -> TextIO:
    if path.suffix == ".gz":
        return gzip.open(path, "rt", encoding="ascii", newline="")
    return path.open("r", encoding="ascii", newline="")


def find_trace(trace_dir: Path, stem: str) -> Path:
    candidates = (
        trace_dir / f"{stem}.csv",
        trace_dir / f"{stem}.csv.gz",
        trace_dir / stem / "samples.csv",
        trace_dir / stem / "samples.csv.gz",
    )
    matches = [path for path in candidates if path.is_file()]
    if not matches:
        expected = ", ".join(str(path) for path in candidates[:2])
        raise FileNotFoundError(f"missing trace {stem!r}; expected {expected}")
    if len(matches) != 1:
        raise ValueError(
            f"multiple inputs found for {stem!r}: " + ", ".join(str(path) for path in matches)
        )
    return matches[0]


def read_header(path: Path) -> tuple[str, ...]:
    with open_text(path) as stream:
        try:
            return tuple(next(csv.reader(stream)))
        except StopIteration as error:
            raise ValueError(f"empty trace: {path}") from error


def load_trace(path: Path) -> np.ndarray:
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
    if data.size == 0 or data.shape[0] == 0:
        raise ValueError(f"trace has a header but no samples: {path}")
    if data.shape[1] != len(USECOLS):
        raise ValueError(f"unexpected loaded shape for {path}: {data.shape}")
    if np.any(data[:, 2:] < 0):
        row, column = np.argwhere(data[:, 2:] < 0)[0]
        raise ValueError(
            f"negative latency in {path} at row {int(row) + 2}, metric column {int(column)}"
        )
    return data


def verify_matched_keys(
    protocol: str,
    replicate: dict[str, str],
    baseline: np.ndarray,
    candidate: np.ndarray,
) -> None:
    if baseline.shape != candidate.shape:
        raise ValueError(
            f"{protocol} {replicate['id']}: trace shapes differ: "
            f"baseline {baseline.shape}, candidate {candidate.shape}"
        )
    unequal = baseline[:, 0] != candidate[:, 0]
    if np.any(unequal):
        mismatch = int(np.flatnonzero(unequal)[0])
        raise ValueError(
            f"{protocol} {replicate['id']}: key sequence first differs at sample {mismatch}: "
            f"baseline={int(baseline[mismatch, 0])}, candidate={int(candidate[mismatch, 0])}"
        )


def nearest_rank(sorted_values: np.ndarray, percentile: float) -> int:
    """Return the nearest-rank percentile at rank ceil(p*N), with ranks one-indexed."""

    rank = max(1, math.ceil(percentile / 100.0 * sorted_values.size))
    return int(sorted_values[rank - 1])


def probability_candidate_faster_sorted(
    candidate_sorted: np.ndarray,
    baseline_sorted: np.ndarray,
    chunk_size: int,
) -> float:
    """Exact P(candidate < baseline) + 0.5 P(tie), with both inputs sorted."""

    baseline_count = int(baseline_sorted.size)
    twice_wins = 0
    for start in range(0, int(candidate_sorted.size), chunk_size):
        chunk = candidate_sorted[start : start + chunk_size]
        left = np.searchsorted(baseline_sorted, chunk, side="left")
        right = np.searchsorted(baseline_sorted, chunk, side="right")
        # A baseline observation greater than the candidate is one win; a tie is half a win.
        doubled = 2 * (baseline_count - right) + (right - left)
        twice_wins += int(np.sum(doubled, dtype=np.int64))
    denominator = 2.0 * float(candidate_sorted.size) * float(baseline_count)
    return twice_wins / denominator


def probability_candidate_faster(
    candidate: np.ndarray,
    baseline: np.ndarray,
    chunk_size: int,
) -> float:
    return probability_candidate_faster_sorted(
        np.sort(candidate), np.sort(baseline), chunk_size
    )


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


def improvement_percent(baseline_ns: int, candidate_ns: int) -> float | None:
    """Positive means the candidate is faster: (baseline - candidate) / baseline."""

    if baseline_ns == 0:
        return 0.0 if candidate_ns == 0 else None
    return 100.0 * (baseline_ns - candidate_ns) / baseline_ns


def summarize_distribution(
    baseline: np.ndarray,
    candidate: np.ndarray,
    chunk_size: int,
) -> dict[str, Any]:
    baseline_sorted = np.sort(baseline)
    candidate_sorted = np.sort(candidate)
    percentiles = []
    for percentile in PERCENTILES:
        baseline_ns = nearest_rank(baseline_sorted, percentile)
        candidate_ns = nearest_rank(candidate_sorted, percentile)
        percentiles.append(
            {
                "percentile": percentile,
                "baseline_ns": baseline_ns,
                "candidate_ns": candidate_ns,
                "candidate_improvement_percent": improvement_percent(
                    baseline_ns, candidate_ns
                ),
            }
        )
    exact_a = probability_candidate_faster_sorted(
        candidate_sorted, baseline_sorted, chunk_size
    )
    del baseline_sorted, candidate_sorted

    candidate_less = int(np.count_nonzero(candidate < baseline))
    equal = int(np.count_nonzero(candidate == baseline))
    paired = (candidate_less + 0.5 * equal) / candidate.size
    return {
        "sample_count_baseline": int(baseline.size),
        "sample_count_candidate": int(candidate.size),
        "percentiles": percentiles,
        "vargha_delaney_A_candidate_faster": exact_a,
        "vargha_delaney_magnitude": vd_magnitude(exact_a),
        "paired_candidate_faster_probability": paired,
    }


def contiguous_block_effects(
    baseline: np.ndarray,
    candidate: np.ndarray,
    blocks: int,
    chunk_size: int,
) -> tuple[np.ndarray, np.ndarray]:
    if baseline.size != candidate.size:
        raise ValueError("matched temporal-block analysis requires equal trace lengths")
    if blocks < 2 or blocks > baseline.size:
        raise ValueError(f"invalid block count {blocks} for {baseline.size} observations")

    edges = np.linspace(0, baseline.size, blocks + 1, dtype=np.int64)
    block_a = np.empty(blocks, dtype=np.float64)
    block_paired = np.empty(blocks, dtype=np.float64)
    for block in range(blocks):
        start, end = int(edges[block]), int(edges[block + 1])
        baseline_block = baseline[start:end]
        candidate_block = candidate[start:end]
        block_a[block] = probability_candidate_faster(
            candidate_block, baseline_block, chunk_size
        )
        less = int(np.count_nonzero(candidate_block < baseline_block))
        equal = int(np.count_nonzero(candidate_block == baseline_block))
        block_paired[block] = (less + 0.5 * equal) / (end - start)
    return block_a, block_paired


def bootstrap_grouped_mean_ci(
    groups: list[np.ndarray],
    repetitions: int,
    rng: np.random.Generator,
) -> tuple[float, float, float]:
    """Bootstrap block means, retaining each matched replicate in every draw."""

    if not groups or any(group.size == 0 for group in groups):
        raise ValueError("cannot bootstrap an empty temporal-block group")
    total_blocks = sum(int(group.size) for group in groups)
    draws = np.empty(repetitions, dtype=np.float64)
    batch_size = 512
    for start in range(0, repetitions, batch_size):
        count = min(batch_size, repetitions - start)
        sums = np.zeros(count, dtype=np.float64)
        for group in groups:
            indices = rng.integers(0, group.size, size=(count, group.size))
            sums += group[indices].sum(axis=1)
        draws[start : start + count] = sums / total_blocks
    values = np.concatenate(groups)
    low, high = np.percentile(draws, (2.5, 97.5))
    return float(values.mean()), float(low), float(high)


def attach_block_intervals(
    result: dict[str, Any],
    block_a_groups: list[np.ndarray],
    block_paired_groups: list[np.ndarray],
    bootstrap_reps: int,
    rng: np.random.Generator,
) -> None:
    blocked_mean, a_low, a_high = bootstrap_grouped_mean_ci(
        block_a_groups, bootstrap_reps, rng
    )
    _, paired_low, paired_high = bootstrap_grouped_mean_ci(
        block_paired_groups, bootstrap_reps, rng
    )
    all_a = np.concatenate(block_a_groups)
    result.update(
        {
            "temporal_block_count": int(all_a.size),
            "temporal_block_A_mean": blocked_mean,
            "temporal_block_A_bootstrap_95_ci": [a_low, a_high],
            "paired_temporal_block_bootstrap_95_ci": [paired_low, paired_high],
            "temporal_block_A_min": float(all_a.min()),
            "temporal_block_A_max": float(all_a.max()),
        }
    )


def analyze_protocol(
    protocol: str,
    trace_dir: Path,
    blocks: int,
    bootstrap_reps: int,
    chunk_size: int,
    rng: np.random.Generator,
) -> dict[str, Any]:
    loaded: dict[str, np.ndarray] = {}
    paths: dict[str, Path] = {}
    for ordinal_variant in ("01-baseline", "02-candidate", "03-candidate", "04-baseline"):
        stem = f"{ordinal_variant}-{protocol}"
        paths[ordinal_variant] = find_trace(trace_dir, stem)
        loaded[ordinal_variant] = load_trace(paths[ordinal_variant])

    result: dict[str, Any] = {"replicates": {}, "pooled": {}}
    block_cache: dict[str, dict[str, tuple[np.ndarray, np.ndarray]]] = {}

    for replicate in REPLICATES:
        baseline = loaded[replicate["baseline"]]
        candidate = loaded[replicate["candidate"]]
        verify_matched_keys(protocol, replicate, baseline, candidate)
        replicate_result: dict[str, Any] = {
            "label": replicate["label"],
            "baseline_run": replicate["baseline"],
            "candidate_run": replicate["candidate"],
            "baseline_trace": str(paths[replicate["baseline"]]),
            "candidate_trace": str(paths[replicate["candidate"]]),
            "sample_count": int(baseline.shape[0]),
            "key_sequence_matched": True,
            "baseline_elapsed_seconds": float(
                (baseline[-1, 1] - baseline[0, 1]) / 1e9
            ),
            "candidate_elapsed_seconds": float(
                (candidate[-1, 1] - candidate[0, 1]) / 1e9
            ),
            "metrics": {},
        }
        block_cache[replicate["id"]] = {}
        for metric_key, metric_label, column in METRICS:
            print(f"analyzing {protocol} {replicate['id']} {metric_label}", flush=True)
            metric_result = summarize_distribution(
                baseline[:, column], candidate[:, column], chunk_size
            )
            block_a, block_paired = contiguous_block_effects(
                baseline[:, column], candidate[:, column], blocks, chunk_size
            )
            attach_block_intervals(
                metric_result,
                [block_a],
                [block_paired],
                bootstrap_reps,
                rng,
            )
            metric_result["label"] = metric_label
            replicate_result["metrics"][metric_key] = metric_result
            block_cache[replicate["id"]][metric_key] = (block_a, block_paired)
        result["replicates"][replicate["id"]] = replicate_result

    pooled_result: dict[str, Any] = {
        "label": "pooled ABBA replicates",
        "replicate_ids": [replicate["id"] for replicate in REPLICATES],
        "sample_count": sum(
            result["replicates"][replicate["id"]]["sample_count"]
            for replicate in REPLICATES
        ),
        "key_sequences_matched_within_each_replicate": True,
        "metrics": {},
    }
    for metric_key, metric_label, column in METRICS:
        print(f"analyzing {protocol} pooled {metric_label}", flush=True)
        baselines = [loaded[replicate["baseline"]][:, column] for replicate in REPLICATES]
        candidates = [loaded[replicate["candidate"]][:, column] for replicate in REPLICATES]
        pooled_baseline = np.concatenate(baselines)
        pooled_candidate = np.concatenate(candidates)
        metric_result = summarize_distribution(
            pooled_baseline, pooled_candidate, chunk_size
        )
        attach_block_intervals(
            metric_result,
            [block_cache[replicate["id"]][metric_key][0] for replicate in REPLICATES],
            [block_cache[replicate["id"]][metric_key][1] for replicate in REPLICATES],
            bootstrap_reps,
            rng,
        )
        metric_result["label"] = metric_label
        pooled_result["metrics"][metric_key] = metric_result
    result["pooled"] = pooled_result
    return result


def scopes(protocol_result: dict[str, Any]):
    for replicate in REPLICATES:
        replicate_id = replicate["id"]
        yield replicate_id, protocol_result["replicates"][replicate_id]
    yield "pooled", protocol_result["pooled"]


def percentile_label(value: float) -> str:
    return f"p{value:g}"


def format_percent(value: float | None) -> str:
    if value is None:
        return "n/a"
    sign = "+" if value > 0 else ""
    return f"{sign}{value:.3f}%"


def write_percentiles_csv(result: dict[str, Any], output: Path) -> None:
    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            (
                "scope",
                "scope_label",
                "protocol",
                "metric",
                "percentile",
                "samples_per_variant",
                "baseline_ns",
                "candidate_ns",
                "baseline_us",
                "candidate_us",
                "candidate_improvement_percent",
            )
        )
        for protocol, protocol_result in result["protocols"].items():
            for scope_id, scope_result in scopes(protocol_result):
                for metric_key, metric_result in scope_result["metrics"].items():
                    for row in metric_result["percentiles"]:
                        improvement = row["candidate_improvement_percent"]
                        writer.writerow(
                            (
                                scope_id,
                                scope_result["label"],
                                protocol,
                                metric_key,
                                percentile_label(row["percentile"]),
                                metric_result["sample_count_baseline"],
                                row["baseline_ns"],
                                row["candidate_ns"],
                                f"{row['baseline_ns'] / 1000.0:.3f}",
                                f"{row['candidate_ns'] / 1000.0:.3f}",
                                "" if improvement is None else f"{improvement:.9f}",
                            )
                        )


def write_effects_csv(result: dict[str, Any], output: Path) -> None:
    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            (
                "scope",
                "scope_label",
                "protocol",
                "metric",
                "samples_per_variant",
                "A_candidate_faster_exact",
                "magnitude",
                "temporal_block_count",
                "temporal_block_A_mean",
                "temporal_block_A_ci_low",
                "temporal_block_A_ci_high",
                "paired_candidate_faster_probability",
                "paired_temporal_block_ci_low",
                "paired_temporal_block_ci_high",
            )
        )
        for protocol, protocol_result in result["protocols"].items():
            for scope_id, scope_result in scopes(protocol_result):
                for metric_key, metric_result in scope_result["metrics"].items():
                    a_low, a_high = metric_result[
                        "temporal_block_A_bootstrap_95_ci"
                    ]
                    paired_low, paired_high = metric_result[
                        "paired_temporal_block_bootstrap_95_ci"
                    ]
                    writer.writerow(
                        (
                            scope_id,
                            scope_result["label"],
                            protocol,
                            metric_key,
                            metric_result["sample_count_baseline"],
                            f"{metric_result['vargha_delaney_A_candidate_faster']:.9f}",
                            metric_result["vargha_delaney_magnitude"],
                            metric_result["temporal_block_count"],
                            f"{metric_result['temporal_block_A_mean']:.9f}",
                            f"{a_low:.9f}",
                            f"{a_high:.9f}",
                            f"{metric_result['paired_candidate_faster_probability']:.9f}",
                            f"{paired_low:.9f}",
                            f"{paired_high:.9f}",
                        )
                    )


def markdown_percentiles(result: dict[str, Any], protocol: str) -> str:
    lines = [
        f"### {protocol}",
        "",
        "Each cell is `baseline -> candidate (candidate improvement)`.",
        "",
        "| scope | metric | p50 | p90 | p99 | p99.9 | p99.99 |",
        "|---|---|---:|---:|---:|---:|---:|",
    ]
    for scope_id, scope_result in scopes(result["protocols"][protocol]):
        for metric_result in scope_result["metrics"].values():
            cells = []
            for row in metric_result["percentiles"]:
                cells.append(
                    "{baseline:.3f} -> {candidate:.3f} us ({improvement})".format(
                        baseline=row["baseline_ns"] / 1000.0,
                        candidate=row["candidate_ns"] / 1000.0,
                        improvement=format_percent(row["candidate_improvement_percent"]),
                    )
                )
            lines.append(
                "| {scope} | {metric} | {cells} |".format(
                    scope=scope_id.replace("_", " "),
                    metric=metric_result["label"],
                    cells=" | ".join(cells),
                )
            )
    return "\n".join(lines)


def markdown_effects(result: dict[str, Any]) -> str:
    lines = [
        "| scope | protocol | metric | exact A: candidate faster | magnitude | temporal-block A (95% CI) | paired fixed-sequence probability (95% CI) |",
        "|---|---|---|---:|---|---:|---:|",
    ]
    for protocol, protocol_result in result["protocols"].items():
        for scope_id, scope_result in scopes(protocol_result):
            for metric_result in scope_result["metrics"].values():
                a_low, a_high = metric_result["temporal_block_A_bootstrap_95_ci"]
                paired_low, paired_high = metric_result[
                    "paired_temporal_block_bootstrap_95_ci"
                ]
                lines.append(
                    "| {scope} | {protocol} | {metric} | {a:.6f} | {magnitude} | "
                    "{blocked:.6f} [{a_low:.6f}, {a_high:.6f}] | "
                    "{paired:.6f} [{paired_low:.6f}, {paired_high:.6f}] |".format(
                        scope=scope_id.replace("_", " "),
                        protocol=protocol,
                        metric=metric_result["label"],
                        a=metric_result["vargha_delaney_A_candidate_faster"],
                        magnitude=metric_result["vargha_delaney_magnitude"],
                        blocked=metric_result["temporal_block_A_mean"],
                        a_low=a_low,
                        a_high=a_high,
                        paired=metric_result["paired_candidate_faster_probability"],
                        paired_low=paired_low,
                        paired_high=paired_high,
                    )
                )
    return "\n".join(lines)


def write_markdown(result: dict[str, Any], output: Path) -> None:
    sections = [
        "# Channel micro-latency ABBA analysis",
        "",
        "The two matched replicate pairs are **01 baseline vs 02 candidate** and "
        "**04 baseline vs 03 candidate**. Their fixed-seed key-index sequences matched exactly "
        "within each protocol and pair. Positive improvement means the candidate was faster: "
        "`(baseline - candidate) / baseline`.",
        "",
        "Percentiles are exact nearest-rank values. Response TTFB runs from query send to the "
        "first protocol byte, payload TTFB to the first object byte, and total download to the "
        "complete response.",
        "",
        "## Percentiles",
        "",
        markdown_percentiles(result, "memcache"),
        "",
        markdown_percentiles(result, "http"),
        "",
        "## Probability of superiority",
        "",
        "Exact Vargha-Delaney `A` is `P(candidate < baseline) + 0.5 P(tie)`, so values above "
        "0.5 favor the candidate. The paired probability compares the same fixed-seed request "
        "position in each matched trace. Confidence intervals resample summaries of fixed, "
        f"nonoverlapping temporal blocks ({result['analysis']['blocks_per_replicate']} per "
        "replicate); pooled intervals retain both replicates in every bootstrap draw. The pooled "
        "exact A is a Cartesian distribution comparison and includes cross-replicate comparisons; "
        "the replicate rows and blocked mean expose run drift.",
        "",
        markdown_effects(result),
        "",
        "The blocked intervals reduce request-level pseudoreplication and sensitivity to "
        "short-range autocorrelation, but do not eliminate whole-run effects such as thermal "
        "drift. With four server launches they are conditional within-run intervals, not evidence "
        "of general build-wide significance. The separate replicate rows expose ABBA order "
        "effects; the pooled row is their combined summary.",
        "",
    ]
    output.write_text("\n".join(sections), encoding="utf-8")


def main() -> int:
    args = parse_args()
    if args.blocks < 2:
        raise SystemExit("--blocks must be at least 2")
    if args.bootstrap_reps < 100:
        raise SystemExit("--bootstrap-reps must be at least 100")
    if args.chunk_size < 1:
        raise SystemExit("--chunk-size must be positive")

    trace_dir = args.trace_dir.resolve()
    if not trace_dir.is_dir():
        raise SystemExit(f"trace directory does not exist: {trace_dir}")
    output_dir = (args.output_dir or trace_dir / "analysis").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.seed)

    result: dict[str, Any] = {
        "trace_dir": str(trace_dir),
        "analysis": {
            "design": "ABBA: baseline, candidate, candidate, baseline",
            "replicate_pairs": [
                {
                    "id": replicate["id"],
                    "baseline": replicate["baseline"],
                    "candidate": replicate["candidate"],
                }
                for replicate in REPLICATES
            ],
            "percentile_method": "nearest rank: ceil(p*N)",
            "improvement_orientation": "100 * (baseline - candidate) / baseline",
            "A_orientation": "P(candidate < baseline) + 0.5*P(tie)",
            "blocks_per_replicate": args.blocks,
            "bootstrap_method": (
                "resample summaries of fixed nonoverlapping temporal blocks within each matched "
                "replicate; retain every replicate in each pooled draw"
            ),
            "bootstrap_repetitions": args.bootstrap_reps,
            "bootstrap_seed": args.seed,
            "chunk_size": args.chunk_size,
            "vargha_delaney_magnitude_abs_A_minus_half_thresholds": [0.06, 0.14, 0.21],
        },
        "protocols": {},
    }
    for protocol in PROTOCOLS:
        result["protocols"][protocol] = analyze_protocol(
            protocol,
            trace_dir,
            args.blocks,
            args.bootstrap_reps,
            args.chunk_size,
            rng,
        )

    json_path = output_dir / "channel_micro_latency_analysis.json"
    json_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_percentiles_csv(
        result, output_dir / "channel_micro_latency_percentiles.csv"
    )
    write_effects_csv(result, output_dir / "channel_micro_latency_effects.csv")
    write_markdown(result, output_dir / "channel_micro_latency_analysis.md")
    print(f"analysis complete: {output_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
