#!/usr/bin/env bash
# Goblin low-concurrency HTTP worker-count sweep. All variants retain the full Optane-local NUMA
# affinity mask; only --cores (and therefore the number of HTTP worker loops) changes.

set -Eeuo pipefail

ROOT=${ROOT:-/mnt/local/goblin-small-bench}
TOOLS=${TOOLS:-$ROOT/tools}
GOBLIN_RUNNER=${GOBLIN_RUNNER:-$TOOLS/run_optane_goblin_baseline.sh}
GOBLIN_BUILD=${GOBLIN_BUILD:-$ROOT/build-goblin}
GOBLIN_BIN=${GOBLIN_BIN:-$GOBLIN_BUILD/goblin-store}
PATH_PREP_BIN=${PATH_PREP_BIN:-$GOBLIN_BUILD/goblin-store-path-prep}

SERIES_ID=${SERIES_ID:-$(date -u +%Y%m%dT%H%M%SZ)-worker-sweep}
SERIES_DIR=${SERIES_DIR:-$ROOT/results/optane-worker-sweep-$SERIES_ID}
WORKER_COUNTS=${WORKER_COUNTS:-"4 6 8 12 24"}
CONCURRENCIES=${CONCURRENCIES:-"16 32"}

SERVER_NODE=${SERVER_NODE:-0}
SERVER_CPUS=${SERVER_CPUS:-0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92}
CLIENT_NODE=${CLIENT_NODE:-2}
CLIENT_CPUS=${CLIENT_CPUS:-2,6,10,14,18,22,26,30,34,38,42,46,50,54,58,62,66,70,74,78,82,86,90,94}
CLIENT_THREADS=${CLIENT_THREADS:-24}
CLIENT_ROOT=${CLIENT_ROOT:-/dev/shm/goblin-local-client}
CLIENT_BIN=${CLIENT_BIN:-$TOOLS/mirror-proxy-benchmark}
ORIGIN_NODE=${ORIGIN_NODE:-3}
ORIGIN_CPUS=${ORIGIN_CPUS:-3,7,11,15,19,23,27,31,35,39,43,47,51,55,59,63,67,71,75,79,83,87,91,95}
ORIGIN_WORKERS=${ORIGIN_WORKERS:-24}
MONITOR_CPU=${MONITOR_CPU:-1}

RAM_HEAD=${RAM_HEAD:-256K}
READ_IO_CHUNK=${READ_IO_CHUNK:-256K}
WRITE_IO_CHUNK=${WRITE_IO_CHUNK:-1M}
PREWARM_CONCURRENCY=${PREWARM_CONCURRENCY:-32}
MIRROR_CLIENT=${MIRROR_CLIENT:-uring}

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

[[ -x $GOBLIN_RUNNER ]] || die "missing Goblin runner: $GOBLIN_RUNNER"
[[ -x $GOBLIN_BIN ]] || die "missing Goblin server: $GOBLIN_BIN"
[[ -x $PATH_PREP_BIN ]] || die "missing Goblin path-prep tool: $PATH_PREP_BIN"
[[ -x $CLIENT_BIN ]] || die "missing local client: $CLIENT_BIN"
[[ $(cat /sys/class/nvme/nvme0/device/numa_node) == "$SERVER_NODE" ]] ||
    die "nvme0 is not attached to server node $SERVER_NODE"
[[ $(systemctl is-active numad 2>/dev/null || true) != active ]] ||
    die "numad must remain stopped for deterministic NUMA placement"
sudo -n true || die "passwordless sudo is required for trim and memory compaction"
for workers in $WORKER_COUNTS; do
    [[ $workers =~ ^[1-9][0-9]*$ ]] || die "bad worker count: $workers"
done
for concurrency in $CONCURRENCIES; do
    [[ $concurrency =~ ^[1-9][0-9]*$ ]] || die "bad concurrency: $concurrency"
done
ss -H -ltn | awk '$4 ~ /:(8080|18000)$/ {busy=1} END {exit !busy}' &&
    die "benchmark port is already occupied"
mkdir -p "$SERIES_DIR"

{
    printf 'started_utc=%s\nseries_id=%s\nworker_counts=%s\nconcurrencies=%s\n' \
        "$(date -u +%FT%TZ)" "$SERIES_ID" "$WORKER_COUNTS" "$CONCURRENCIES"
    printf 'transport=local-loopback\nram_head=%s\nread_io_chunk=%s\nwrite_io_chunk=%s\n' \
        "$RAM_HEAD" "$READ_IO_CHUNK" "$WRITE_IO_CHUNK"
    printf 'server_node=%s\nserver_cpus=%s\nserver_physical_cpu_capacity=12\n' \
        "$SERVER_NODE" "$SERVER_CPUS"
    printf 'client_node=%s\nclient_cpus=%s\nclient_threads=%s\n' \
        "$CLIENT_NODE" "$CLIENT_CPUS" "$CLIENT_THREADS"
    printf 'origin_node=%s\norigin_cpus=%s\norigin_workers=%s\n' \
        "$ORIGIN_NODE" "$ORIGIN_CPUS" "$ORIGIN_WORKERS"
    printf 'prewarm_concurrency=%s\nmirror_client=%s\n' \
        "$PREWARM_CONCURRENCY" "$MIRROR_CLIENT"
    sha256sum "$GOBLIN_RUNNER" "$GOBLIN_BIN" "$PATH_PREP_BIN" "$CLIENT_BIN"
} > "$SERIES_DIR/config.txt"

printf '[%s] starting Goblin worker sweep: %s\n' \
    "$(date -u +%FT%TZ)" "$WORKER_COUNTS" | tee "$SERIES_DIR/progress.log"

for workers in $WORKER_COUNTS; do
    run_id="$SERIES_ID-workers-$workers"
    pool="$ROOT/cache/goblin-workers-$workers-$SERIES_ID"
    run_dir="$ROOT/results/goblin-workers-$workers-$SERIES_ID"

    printf '[%s] removing stale workers-%s pool and trimming\n' \
        "$(date -u +%FT%TZ)" "$workers" | tee -a "$SERIES_DIR/progress.log"
    rm -rf "$pool"
    sudo -n fstrim -v /mnt/local | tee "$SERIES_DIR/before-workers-$workers-fstrim.txt"

    printf '[%s] starting %s workers (head %s; I/O %s; concurrency %s)\n' \
        "$(date -u +%FT%TZ)" "$workers" "$RAM_HEAD" "$READ_IO_CHUNK" "$CONCURRENCIES" \
        | tee -a "$SERIES_DIR/progress.log"
    env GOBLIN_BUILD="$GOBLIN_BUILD" GOBLIN_BIN="$GOBLIN_BIN" \
        PATH_PREP_BIN="$PATH_PREP_BIN" \
        RUN_ID="$run_id" RUN_DIR="$run_dir" POOL="$pool" RAM_HEAD="$RAM_HEAD" \
        READ_IO_CHUNK="$READ_IO_CHUNK" WRITE_IO_CHUNK="$WRITE_IO_CHUNK" \
        MIRROR_CLIENT="$MIRROR_CLIENT" MEASURED_CONCURRENCIES="$CONCURRENCIES" \
        PREWARM_CONCURRENCY="$PREWARM_CONCURRENCY" \
        SERVER_IP=127.0.0.1 SERVER_IFACE=lo SERVER_NODE="$SERVER_NODE" \
        SERVER_CPUS="$SERVER_CPUS" SERVER_CORES="$workers" \
        ORIGIN_NODE="$ORIGIN_NODE" ORIGIN_CPUS="$ORIGIN_CPUS" \
        ORIGIN_WORKERS="$ORIGIN_WORKERS" MONITOR_CPU="$MONITOR_CPU" \
        CLIENT_HOST=local CLIENT_IP=127.0.0.1 CLIENT_IFACE=lo CLIENT_NODE="$CLIENT_NODE" \
        CLIENT_CPUS="$CLIENT_CPUS" CLIENT_THREADS="$CLIENT_THREADS" \
        CLIENT_MEMORY_POLICY=bind CLIENT_ROOT="$CLIENT_ROOT" CLIENT_BIN="$CLIENT_BIN" \
        CLIENT_PREFETCH_MANIFEST="$ROOT/manifests/prefetch.tsv" \
        CLIENT_ORDER_MANIFEST="$ROOT/manifests/order.tsv" \
        "$GOBLIN_RUNNER" 2>&1 | tee "$SERIES_DIR/workers-$workers-controller.log"

    printf '[%s] workers-%s run complete; removing disposable cache pool\n' \
        "$(date -u +%FT%TZ)" "$workers" | tee -a "$SERIES_DIR/progress.log"
    rm -rf "$pool"
done

printf 'completed_utc=%s\n' "$(date -u +%FT%TZ)" >> "$SERIES_DIR/config.txt"
printf '[%s] Goblin worker sweep complete: %s\n' \
    "$(date -u +%FT%TZ)" "$SERIES_DIR" | tee -a "$SERIES_DIR/progress.log"
