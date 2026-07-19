#!/usr/bin/env bash
# Matched Vinyl/Goblin warm-cache concurrency sweep using every client CPU.

set -Eeuo pipefail

ROOT=${ROOT:-/mnt/local/goblin-small-bench}
TOOLS=${TOOLS:-$ROOT/tools}
VINYL_RUNNER=${VINYL_RUNNER:-$TOOLS/run_optane_vinyl_baseline.sh}
GOBLIN_RUNNER=${GOBLIN_RUNNER:-$TOOLS/run_optane_goblin_baseline.sh}

SERIES_ID=${SERIES_ID:-$(date -u +%Y%m%dT%H%M%SZ)-massive-concurrency}
SERIES_DIR=${SERIES_DIR:-$ROOT/results/optane-massive-concurrency-$SERIES_ID}
CONCURRENCIES=${CONCURRENCIES:-"16 32 64 128 256 512"}

# All 12 physical cores and both SMT siblings on server NUMA node 1.
SERVER_CPUS=${SERVER_CPUS:-1,5,9,13,17,21,25,29,33,37,41,45,49,53,57,61,65,69,73,77,81,85,89,93}
SERVER_CORES=${SERVER_CORES:-12}

# All 48 physical cores and both SMT siblings on the four-node client.
CLIENT_CPUS=${CLIENT_CPUS:-0-95}
CLIENT_THREADS=${CLIENT_THREADS:-96}
CLIENT_MEMORY_POLICY=${CLIENT_MEMORY_POLICY:-interleave}

PREWARM_CONCURRENCY=${PREWARM_CONCURRENCY:-32}
GOBLIN_HEAD=${GOBLIN_HEAD:-256K}
MIRROR_CLIENT=${MIRROR_CLIENT:-uring}
READ_IO_CHUNK=${READ_IO_CHUNK:-256K}
WRITE_IO_CHUNK=${WRITE_IO_CHUNK:-1M}

VINYL_RUN_ID=${VINYL_RUN_ID:-$SERIES_ID-vinyl}
GOBLIN_RUN_ID=${GOBLIN_RUN_ID:-$SERIES_ID-goblin-256k}
VINYL_STORAGE_FILE=${VINYL_STORAGE_FILE:-$ROOT/cache/vinyl/vinyl.bin}

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

[[ -x $VINYL_RUNNER ]] || die "missing Vinyl runner: $VINYL_RUNNER"
[[ -x $GOBLIN_RUNNER ]] || die "missing Goblin runner: $GOBLIN_RUNNER"
[[ $(systemctl is-active numad 2>/dev/null || true) != active ]] ||
    die "numad must remain stopped for deterministic NUMA placement"
for concurrency in $CONCURRENCIES; do
    [[ $concurrency =~ ^[1-9][0-9]*$ ]] || die "bad concurrency: $concurrency"
done
sudo -n true || die "passwordless sudo is required for filesystem trim"
mkdir -p "$SERIES_DIR"

{
    printf 'started_utc=%s\nseries_id=%s\nconcurrencies=%s\n' \
        "$(date -u +%FT%TZ)" "$SERIES_ID" "$CONCURRENCIES"
    printf 'server_cpus=%s\nserver_cores=%s\n' "$SERVER_CPUS" "$SERVER_CORES"
    printf 'client_cpus=%s\nclient_threads=%s\nclient_memory_policy=%s\n' \
        "$CLIENT_CPUS" "$CLIENT_THREADS" "$CLIENT_MEMORY_POLICY"
    printf 'prewarm_concurrency=%s\ngoblin_head=%s\n' \
        "$PREWARM_CONCURRENCY" "$GOBLIN_HEAD"
    printf 'mirror_client=%s\nread_io_chunk=%s\nwrite_io_chunk=%s\n' \
        "$MIRROR_CLIENT" "$READ_IO_CHUNK" "$WRITE_IO_CHUNK"
    sha256sum "$VINYL_RUNNER" "$GOBLIN_RUNNER"
} > "$SERIES_DIR/config.txt"

printf '[%s] removing prior Vinyl file and trimming before Vinyl\n' "$(date -u +%FT%TZ)" \
    | tee "$SERIES_DIR/progress.log"
rm -f "$VINYL_STORAGE_FILE"
sudo -n fstrim -v /mnt/local | tee "$SERIES_DIR/before-vinyl-fstrim.txt"

printf '[%s] starting Vinyl sweep\n' "$(date -u +%FT%TZ)" \
    | tee -a "$SERIES_DIR/progress.log"
env RUN_ID="$VINYL_RUN_ID" MEASURED_CONCURRENCIES="$CONCURRENCIES" \
    PREWARM_CONCURRENCY="$PREWARM_CONCURRENCY" SERVER_CPUS="$SERVER_CPUS" \
    CLIENT_CPUS="$CLIENT_CPUS" CLIENT_THREADS="$CLIENT_THREADS" \
    CLIENT_MEMORY_POLICY="$CLIENT_MEMORY_POLICY" \
    "$VINYL_RUNNER" 2>&1 | tee "$SERIES_DIR/vinyl-controller.log"
printf '[%s] Vinyl sweep complete\n' "$(date -u +%FT%TZ)" \
    | tee -a "$SERIES_DIR/progress.log"

printf '[%s] removing Vinyl file and trimming before Goblin\n' "$(date -u +%FT%TZ)" \
    | tee -a "$SERIES_DIR/progress.log"
rm -f "$VINYL_STORAGE_FILE"
[[ ! -e $VINYL_STORAGE_FILE ]] || die "could not remove Vinyl storage file"
sudo -n fstrim -v /mnt/local | tee "$SERIES_DIR/before-goblin-fstrim.txt"
[[ $(systemctl is-active numad 2>/dev/null || true) != active ]] ||
    die "numad became active between cache runs"

printf '[%s] starting Goblin 256K-head sweep\n' "$(date -u +%FT%TZ)" \
    | tee -a "$SERIES_DIR/progress.log"
env RUN_ID="$GOBLIN_RUN_ID" RAM_HEAD="$GOBLIN_HEAD" \
    MIRROR_CLIENT="$MIRROR_CLIENT" READ_IO_CHUNK="$READ_IO_CHUNK" \
    WRITE_IO_CHUNK="$WRITE_IO_CHUNK" MEASURED_CONCURRENCIES="$CONCURRENCIES" \
    PREWARM_CONCURRENCY="$PREWARM_CONCURRENCY" SERVER_CPUS="$SERVER_CPUS" \
    SERVER_CORES="$SERVER_CORES" CLIENT_CPUS="$CLIENT_CPUS" \
    CLIENT_THREADS="$CLIENT_THREADS" CLIENT_MEMORY_POLICY="$CLIENT_MEMORY_POLICY" \
    "$GOBLIN_RUNNER" 2>&1 | tee "$SERIES_DIR/goblin-controller.log"
printf '[%s] Goblin sweep complete\n' "$(date -u +%FT%TZ)" \
    | tee -a "$SERIES_DIR/progress.log"

printf 'completed_utc=%s\n' "$(date -u +%FT%TZ)" >> "$SERIES_DIR/config.txt"
printf '[%s] massive-concurrency pair complete: %s\n' "$(date -u +%FT%TZ)" "$SERIES_DIR" \
    | tee -a "$SERIES_DIR/progress.log"
