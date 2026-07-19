#!/usr/bin/env bash
# Run complete, matched Goblin Store baselines for several resident-head sizes.

set -Eeuo pipefail

ROOT=${ROOT:-/mnt/local/goblin-small-bench}
TOOLS=${TOOLS:-$ROOT/tools}
BASELINE_SCRIPT=${BASELINE_SCRIPT:-$TOOLS/run_optane_goblin_baseline.sh}

HEADS=${HEADS:-"32K 64K 128K 256K"}
SWEEP_ID=${SWEEP_ID:-$(date -u +%Y%m%dT%H%M%SZ)-native-uring-head-sweep}
SWEEP_DIR=${SWEEP_DIR:-$ROOT/results/goblin-head-sweep-$SWEEP_ID}

MIRROR_CLIENT=${MIRROR_CLIENT:-uring}
READ_IO_CHUNK=${READ_IO_CHUNK:-256K}
WRITE_IO_CHUNK=${WRITE_IO_CHUNK:-1M}
SERVER_CPUS=${SERVER_CPUS:-1,5,9,13,17,21,25,29,49,53,57,61,65,69,73,77}
SERVER_CORES=${SERVER_CORES:-8}
PREWARM_CONCURRENCY=${PREWARM_CONCURRENCY:-32}
MEASURED_CONCURRENCY=${MEASURED_CONCURRENCY:-8}
CLIENT_CPUS=${CLIENT_CPUS:-45}

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

[[ -x $BASELINE_SCRIPT ]] || die "missing executable baseline runner: $BASELINE_SCRIPT"
sudo -n true || die "passwordless sudo is required for filesystem trim"
mkdir -p "$SWEEP_DIR"

{
    printf 'started_utc=%s\n' "$(date -u +%FT%TZ)"
    printf 'heads=%s\n' "$HEADS"
    printf 'mirror_client=%s\n' "$MIRROR_CLIENT"
    printf 'read_io_chunk=%s\nwrite_io_chunk=%s\n' "$READ_IO_CHUNK" "$WRITE_IO_CHUNK"
    printf 'server_cpus=%s\nserver_cores=%s\n' "$SERVER_CPUS" "$SERVER_CORES"
    printf 'prewarm_concurrency=%s\nmeasured_concurrency=%s\n' \
        "$PREWARM_CONCURRENCY" "$MEASURED_CONCURRENCY"
    printf 'client_cpus=%s\n' "$CLIENT_CPUS"
    sha256sum "$BASELINE_SCRIPT"
} > "$SWEEP_DIR/config.txt"

for head in $HEADS; do
    slug=${head,,}
    case_id=$SWEEP_ID-head-$slug
    case_log=$SWEEP_DIR/$slug.log

    printf '[%s] trimming /mnt/local before head=%s\n' "$(date -u +%FT%TZ)" "$head" \
        | tee -a "$SWEEP_DIR/progress.log"
    sudo -n fstrim -v /mnt/local | tee -a "$case_log"

    printf '[%s] starting complete baseline for head=%s\n' "$(date -u +%FT%TZ)" "$head" \
        | tee -a "$SWEEP_DIR/progress.log" "$case_log"
    env RUN_ID="$case_id" RAM_HEAD="$head" \
        MIRROR_CLIENT="$MIRROR_CLIENT" \
        READ_IO_CHUNK="$READ_IO_CHUNK" WRITE_IO_CHUNK="$WRITE_IO_CHUNK" \
        SERVER_CPUS="$SERVER_CPUS" SERVER_CORES="$SERVER_CORES" \
        PREWARM_CONCURRENCY="$PREWARM_CONCURRENCY" \
        MEASURED_CONCURRENCY="$MEASURED_CONCURRENCY" CLIENT_CPUS="$CLIENT_CPUS" \
        "$BASELINE_SCRIPT" 2>&1 | tee -a "$case_log"
    printf '[%s] completed baseline for head=%s\n' "$(date -u +%FT%TZ)" "$head" \
        | tee -a "$SWEEP_DIR/progress.log" "$case_log"
done

printf 'completed_utc=%s\n' "$(date -u +%FT%TZ)" >> "$SWEEP_DIR/config.txt"
printf '[%s] head sweep complete: %s\n' "$(date -u +%FT%TZ)" "$SWEEP_DIR" \
    | tee -a "$SWEEP_DIR/progress.log"
