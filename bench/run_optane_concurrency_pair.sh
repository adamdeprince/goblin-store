#!/usr/bin/env bash
# Run matched Vinyl and Goblin 16 KiB-head warm-cache passes at a selected read concurrency.

set -Eeuo pipefail

ROOT=${ROOT:-/mnt/local/goblin-small-bench}
TOOLS=${TOOLS:-$ROOT/tools}
VINYL_RUNNER=${VINYL_RUNNER:-$TOOLS/run_optane_vinyl_baseline.sh}
GOBLIN_RUNNER=${GOBLIN_RUNNER:-$TOOLS/run_optane_goblin_baseline.sh}
PAIR_ID=${PAIR_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
MEASURED_CONCURRENCY=${MEASURED_CONCURRENCY:-8}
CLIENT_CPUS=${CLIENT_CPUS:-45}
SERVER_CPUS=${SERVER_CPUS:-}
SERVER_CORES=${SERVER_CORES:-0}
VINYL_RUN_ID=${VINYL_RUN_ID:-${PAIR_ID}-c${MEASURED_CONCURRENCY}-vinyl}
GOBLIN_RUN_ID=${GOBLIN_RUN_ID:-${PAIR_ID}-c${MEASURED_CONCURRENCY}-goblin16k}
VINYL_STORAGE_FILE=${VINYL_STORAGE_FILE:-$ROOT/cache/vinyl/vinyl.bin}
PAIR_DIR=${PAIR_DIR:-$ROOT/results/concurrency${MEASURED_CONCURRENCY}-pair-$PAIR_ID}

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

mkdir -p "$PAIR_DIR"
[[ -x $VINYL_RUNNER ]] || die "missing Vinyl runner: $VINYL_RUNNER"
[[ -x $GOBLIN_RUNNER ]] || die "missing Goblin runner: $GOBLIN_RUNNER"
[[ $MEASURED_CONCURRENCY =~ ^[1-9][0-9]*$ ]] || die "bad measured concurrency"
[[ $(systemctl is-active numad 2>/dev/null || true) != active ]] ||
    die "numad must remain stopped for deterministic NUMA placement"

{
    printf 'pair_id=%s\nvinyl_run_id=%s\ngoblin_run_id=%s\n' \
        "$PAIR_ID" "$VINYL_RUN_ID" "$GOBLIN_RUN_ID"
    printf 'measured_concurrency=%s\nclient_cpus=%s\nserver_cpus=%s\nserver_cores=%s\ngoblin_ram_head=16K\n' \
        "$MEASURED_CONCURRENCY" "$CLIENT_CPUS" "${SERVER_CPUS:-baseline-default}" "$SERVER_CORES"
    date -u +%FT%TZ
} > "$PAIR_DIR/config.txt"

printf '[%s] starting Vinyl concurrency-%s run %s on client CPUs %s\n' \
    "$(date -u +%FT%TZ)" "$MEASURED_CONCURRENCY" "$VINYL_RUN_ID" "$CLIENT_CPUS"
env RUN_ID="$VINYL_RUN_ID" MEASURED_CONCURRENCY="$MEASURED_CONCURRENCY" \
    CLIENT_CPUS="$CLIENT_CPUS" SERVER_CPUS="$SERVER_CPUS" \
    "$VINYL_RUNNER" 2>&1 | tee "$PAIR_DIR/vinyl-controller.log"

printf '[%s] removing Vinyl cache and trimming /mnt/local before Goblin\n' \
    "$(date -u +%FT%TZ)"
rm -f "$VINYL_STORAGE_FILE"
[[ ! -e $VINYL_STORAGE_FILE ]] || die "could not remove Vinyl storage file"
sudo -n fstrim -v /mnt/local | tee "$PAIR_DIR/between-runs-fstrim.txt"

[[ $(systemctl is-active numad 2>/dev/null || true) != active ]] ||
    die "numad became active between runs"
printf '[%s] starting Goblin 16K concurrency-%s run %s on client CPUs %s\n' \
    "$(date -u +%FT%TZ)" "$MEASURED_CONCURRENCY" "$GOBLIN_RUN_ID" "$CLIENT_CPUS"
env RUN_ID="$GOBLIN_RUN_ID" RAM_HEAD=16K \
    MEASURED_CONCURRENCY="$MEASURED_CONCURRENCY" CLIENT_CPUS="$CLIENT_CPUS" \
    SERVER_CPUS="$SERVER_CPUS" SERVER_CORES="$SERVER_CORES" \
    "$GOBLIN_RUNNER" 2>&1 | tee "$PAIR_DIR/goblin-controller.log"

printf '[%s] concurrency-%s pair complete: %s\n' \
    "$(date -u +%FT%TZ)" "$MEASURED_CONCURRENCY" "$PAIR_DIR"
