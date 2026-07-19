#!/usr/bin/env bash
# Wait for a running coarse worker sweep, select its top three throughput scores, run rotated
# three-repetition confirmations, and generate the full latency/CPU analysis unattended.

set -Eeuo pipefail

ROOT=${ROOT:-/mnt/local/goblin-small-bench}
TOOLS=${TOOLS:-$ROOT/tools}
ANALYZER=${ANALYZER:-$TOOLS/analyze_optane_worker_sweep.py}
CONFIRM=${CONFIRM:-$TOOLS/run_optane_worker_confirmation.sh}
GOBLIN_BUILD=${GOBLIN_BUILD:-/mnt/local/goblin-dispatch-src/build-linux}

: "${COARSE_SESSION:?set COARSE_SESSION to the active tmux session}"
: "${COARSE_SERIES_DIR:?set COARSE_SERIES_DIR to its controller artifact directory}"
CONFIRM_CONCURRENCIES=${CONFIRM_CONCURRENCIES:-"16 32 64 128"}
CONFIRM_REPETITIONS=${CONFIRM_REPETITIONS:-3}
FOLLOWUP_ID=${FOLLOWUP_ID:-$(date -u +%Y%m%dT%H%M%SZ)-dispatcher-worker-followup}
FOLLOWUP_DIR=${FOLLOWUP_DIR:-$ROOT/results/optane-worker-followup-$FOLLOWUP_ID}

mkdir -p "$FOLLOWUP_DIR"
exec > >(tee -a "$FOLLOWUP_DIR/controller.log") 2>&1

printf '[%s] waiting for coarse session %s\n' "$(date -u +%FT%TZ)" "$COARSE_SESSION"
while tmux has-session -t "$COARSE_SESSION" 2>/dev/null; do
    sleep 30
done
grep -q '^completed_utc=' "$COARSE_SERIES_DIR/config.txt" || {
    printf 'ERROR: coarse session exited without a completed artifact\n' >&2
    exit 1
}

printf '[%s] analyzing coarse throughput\n' "$(date -u +%FT%TZ)"
"$ANALYZER" --series-dir "$COARSE_SERIES_DIR" \
    --output-dir "$FOLLOWUP_DIR/coarse-analysis"
top_workers=$(<"$FOLLOWUP_DIR/coarse-analysis/top-workers.txt")
printf '[%s] selected confirmation workers: %s\n' "$(date -u +%FT%TZ)" "$top_workers"

confirmation_id="$FOLLOWUP_ID-confirmation"
confirmation_dir="$ROOT/results/optane-worker-confirmation-$confirmation_id"
WORKER_COUNTS="$top_workers" CONCURRENCIES="$CONFIRM_CONCURRENCIES" \
    REPETITIONS="$CONFIRM_REPETITIONS" SERIES_ID="$confirmation_id" \
    CONTROLLER_DIR="$confirmation_dir" GOBLIN_BUILD="$GOBLIN_BUILD" \
    "$CONFIRM"

analysis_args=()
while IFS= read -r series_dir; do
    [[ -n $series_dir ]] && analysis_args+=(--series-dir "$series_dir")
done < "$confirmation_dir/series-dirs.txt"

printf '[%s] computing exact confirmation latency and CPU statistics\n' \
    "$(date -u +%FT%TZ)"
"$ANALYZER" "${analysis_args[@]}" --latency \
    --output-dir "$FOLLOWUP_DIR/confirmation-analysis"
printf '[%s] worker experiment and analysis complete: %s\n' \
    "$(date -u +%FT%TZ)" "$FOLLOWUP_DIR"
