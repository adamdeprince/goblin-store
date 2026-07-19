#!/usr/bin/env bash
# Repeat the selected post-dispatcher worker counts. Each repetition is a complete independent
# worker sweep: fresh pool, fstrim, HugeTLB preparation, population, and fixed-order warm traces.

set -Eeuo pipefail

ROOT=${ROOT:-/mnt/local/goblin-small-bench}
TOOLS=${TOOLS:-$ROOT/tools}
SWEEP=${SWEEP:-$TOOLS/run_optane_worker_sweep.sh}
GOBLIN_BUILD=${GOBLIN_BUILD:-/mnt/local/goblin-dispatch-src/build-linux}

: "${WORKER_COUNTS:?set WORKER_COUNTS to the coarse sweep finalists}"
CONCURRENCIES=${CONCURRENCIES:-"16 32 64 128"}
REPETITIONS=${REPETITIONS:-3}
SERIES_ID=${SERIES_ID:-$(date -u +%Y%m%dT%H%M%SZ)-dispatcher-worker-confirmation}
CONTROLLER_DIR=${CONTROLLER_DIR:-$ROOT/results/optane-worker-confirmation-$SERIES_ID}

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

[[ -x $SWEEP ]] || die "missing worker sweep: $SWEEP"
[[ $REPETITIONS =~ ^[1-9][0-9]*$ ]] || die "bad repetition count: $REPETITIONS"
mkdir -p "$CONTROLLER_DIR"

{
    printf 'started_utc=%s\nseries_id=%s\n' "$(date -u +%FT%TZ)" "$SERIES_ID"
    printf 'worker_counts=%s\nconcurrencies=%s\nrepetitions=%s\n' \
        "$WORKER_COUNTS" "$CONCURRENCIES" "$REPETITIONS"
    printf 'goblin_build=%s\n' "$GOBLIN_BUILD"
    sha256sum "$SWEEP" "$GOBLIN_BUILD/goblin-store" \
        "$GOBLIN_BUILD/goblin-store-path-prep"
} > "$CONTROLLER_DIR/config.txt"
: > "$CONTROLLER_DIR/series-dirs.txt"
read -r -a selected_workers <<< "$WORKER_COUNTS"

for ((rep = 1; rep <= REPETITIONS; ++rep)); do
    # Rotate the candidate order so a temperature/time trend cannot consistently favor the first
    # or last worker count. Every candidate still runs once per repetition.
    ordered_workers=()
    for ((position = 0; position < ${#selected_workers[@]}; ++position)); do
        index=$(( (position + rep - 1) % ${#selected_workers[@]} ))
        ordered_workers+=("${selected_workers[index]}")
    done
    child_worker_counts="${ordered_workers[*]}"
    child_id="$SERIES_ID-r$rep"
    child_dir="$ROOT/results/optane-worker-sweep-$child_id"
    printf '%s\n' "$child_dir" >> "$CONTROLLER_DIR/series-dirs.txt"
    printf '[%s] starting confirmation repetition %s/%s: workers %s; concurrency %s\n' \
        "$(date -u +%FT%TZ)" "$rep" "$REPETITIONS" "$child_worker_counts" "$CONCURRENCIES" \
        | tee -a "$CONTROLLER_DIR/progress.log"
    env SERIES_ID="$child_id" WORKER_COUNTS="$child_worker_counts" \
        CONCURRENCIES="$CONCURRENCIES" GOBLIN_BUILD="$GOBLIN_BUILD" \
        "$SWEEP" 2>&1 | tee "$CONTROLLER_DIR/repetition-$rep.log"
    printf '[%s] confirmation repetition %s/%s complete\n' \
        "$(date -u +%FT%TZ)" "$rep" "$REPETITIONS" \
        | tee -a "$CONTROLLER_DIR/progress.log"
done

printf 'completed_utc=%s\n' "$(date -u +%FT%TZ)" >> "$CONTROLLER_DIR/config.txt"
printf '[%s] all worker confirmations complete: %s\n' \
    "$(date -u +%FT%TZ)" "$CONTROLLER_DIR" | tee -a "$CONTROLLER_DIR/progress.log"
