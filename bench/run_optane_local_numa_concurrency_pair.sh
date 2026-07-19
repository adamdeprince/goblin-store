#!/usr/bin/env bash
# Local loopback concurrency sweep with the cache on the Optane node and the client spread over
# the other three NUMA nodes. This removes IPoIB from the data path and gives the load generator
# enough CPU to drive high connection counts without competing for the server's local cores.

set -Eeuo pipefail

ROOT=${ROOT:-/mnt/local/goblin-small-bench}
TOOLS=${TOOLS:-$ROOT/tools}
VINYL_RUNNER=${VINYL_RUNNER:-$TOOLS/run_optane_vinyl_baseline.sh}
GOBLIN_RUNNER=${GOBLIN_RUNNER:-$TOOLS/run_optane_goblin_baseline.sh}

SERIES_ID=${SERIES_ID:-$(date -u +%Y%m%dT%H%M%SZ)-local-numa-concurrency}
SERIES_DIR=${SERIES_DIR:-$ROOT/results/optane-local-numa-concurrency-$SERIES_ID}
CONCURRENCIES=${CONCURRENCIES:-"16 32 64 128 256 512 1024"}

# Optane is attached to node 0. Give each cache every logical CPU belonging to its 12 physical
# cores; Goblin starts 12 protocol workers. Vinyl is constrained by the same affinity mask.
SERVER_NODE=${SERVER_NODE:-0}
SERVER_CPUS=${SERVER_CPUS:-0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92}
SERVER_CORES=${SERVER_CORES:-12}

# Nodes 1, 2, and 3 provide 36 physical cores and 72 logical CPUs. Their sorted CPU list presents
# one sibling from every core before the second siblings, so C32 uses 32 physical cores and C64+
# reaches all 36 cores before adding SMT. Interleave the client's task/result pages across exactly
# these nodes; node 0 remains private to the cache server and Optane path.
CLIENT_NODE=${CLIENT_NODE:-1}
CLIENT_NODES=${CLIENT_NODES:-1,2,3}
CLIENT_CPUS=${CLIENT_CPUS:-1,2,3,5,6,7,9,10,11,13,14,15,17,18,19,21,22,23,25,26,27,29,30,31,33,34,35,37,38,39,41,42,43,45,46,47,49,50,51,53,54,55,57,58,59,61,62,63,65,66,67,69,70,71,73,74,75,77,78,79,81,82,83,85,86,87,89,90,91,93,94,95}
CLIENT_THREADS=${CLIENT_THREADS:-72}
CLIENT_MEMORY_POLICY=${CLIENT_MEMORY_POLICY:-interleave}
CLIENT_MEMORY_NODES=${CLIENT_MEMORY_NODES:-$CLIENT_NODES}
CLIENT_ROOT=${CLIENT_ROOT:-/dev/shm/goblin-local-client}
CLIENT_BIN=${CLIENT_BIN:-$TOOLS/mirror-proxy-benchmark}

# Keep the origin off both measured slices. It is used only during prewarm.
ORIGIN_NODE=${ORIGIN_NODE:-3}
ORIGIN_CPUS=${ORIGIN_CPUS:-3,7,11,15,19,23,27,31,35,39,43,47,51,55,59,63,67,71,75,79,83,87,91,95}
ORIGIN_WORKERS=${ORIGIN_WORKERS:-24}
# The 12 server workers use node-0 CPUs 0..44; reserve an otherwise idle SMT sibling for the
# one-hertz resource monitor instead of stealing a client CPU.
MONITOR_CPU=${MONITOR_CPU:-92}

SERVER_IP=${SERVER_IP:-127.0.0.1}
CLIENT_IP=${CLIENT_IP:-127.0.0.1}
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
[[ -x $CLIENT_BIN ]] || die "missing local client: $CLIENT_BIN"
[[ $(cat /sys/class/nvme/nvme0/device/numa_node) == "$SERVER_NODE" ]] ||
    die "nvme0 is not attached to server node $SERVER_NODE"
actual_server_cpus=$(numactl -H | awk -v node="$SERVER_NODE" \
    '$1 == "node" && $2 == node && $3 == "cpus:" {$1=$2=$3=""; sub(/^ +/, ""); print; exit}')
actual_client_cpus=$(
    for node in ${CLIENT_NODES//,/ }; do
        numactl -H | awk -v node="$node" \
            '$1 == "node" && $2 == node && $3 == "cpus:" {$1=$2=$3=""; sub(/^ +/, ""); print; exit}'
    done | tr ' ' '\n' | sed '/^$/d' | sort -n | paste -sd, -
)
expected_client_cpus=$(tr ',' '\n' <<< "$CLIENT_CPUS" | sort -n | paste -sd, -)
[[ $actual_server_cpus == "${SERVER_CPUS//,/ }" ]] ||
    die "server CPU list does not match node $SERVER_NODE"
[[ $actual_client_cpus == "$expected_client_cpus" ]] ||
    die "client CPU list does not match nodes $CLIENT_NODES"
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
    printf 'transport=local-loopback\nserver_ip=%s\nclient_ip=%s\n' "$SERVER_IP" "$CLIENT_IP"
    printf 'server_node=%s\nserver_physical_cores=12\n' "$SERVER_NODE"
    printf 'server_cpus=%s\nserver_cores=%s\n' "$SERVER_CPUS" "$SERVER_CORES"
    printf 'client_nodes=%s\nclient_cpus=%s\nclient_threads=%s\n' \
        "$CLIENT_NODES" "$CLIENT_CPUS" "$CLIENT_THREADS"
    printf 'client_memory_policy=%s\nclient_memory_nodes=%s\n' \
        "$CLIENT_MEMORY_POLICY" "$CLIENT_MEMORY_NODES"
    numactl -H
    printf 'origin_node=%s\norigin_cpus=%s\norigin_workers=%s\n' \
        "$ORIGIN_NODE" "$ORIGIN_CPUS" "$ORIGIN_WORKERS"
    printf 'prewarm_concurrency=%s\ngoblin_head=%s\n' \
        "$PREWARM_CONCURRENCY" "$GOBLIN_HEAD"
    printf 'mirror_client=%s\nread_io_chunk=%s\nwrite_io_chunk=%s\n' \
        "$MIRROR_CLIENT" "$READ_IO_CHUNK" "$WRITE_IO_CHUNK"
    sha256sum "$VINYL_RUNNER" "$GOBLIN_RUNNER" "$CLIENT_BIN"
} > "$SERIES_DIR/config.txt"

printf '[%s] removing prior Vinyl file and trimming before local Vinyl sweep\n' \
    "$(date -u +%FT%TZ)" | tee "$SERIES_DIR/progress.log"
rm -f "$VINYL_STORAGE_FILE"
sudo -n fstrim -v /mnt/local | tee "$SERIES_DIR/before-vinyl-fstrim.txt"

printf '[%s] starting local Vinyl sweep on node %s, client on nodes %s\n' \
    "$(date -u +%FT%TZ)" "$SERVER_NODE" "$CLIENT_NODES" | tee -a "$SERIES_DIR/progress.log"
env RUN_ID="$VINYL_RUN_ID" MEASURED_CONCURRENCIES="$CONCURRENCIES" \
    PREWARM_CONCURRENCY="$PREWARM_CONCURRENCY" \
    SERVER_IP="$SERVER_IP" SERVER_IFACE=lo SERVER_NODE="$SERVER_NODE" \
    SERVER_CPUS="$SERVER_CPUS" ORIGIN_NODE="$ORIGIN_NODE" ORIGIN_CPUS="$ORIGIN_CPUS" \
    ORIGIN_WORKERS="$ORIGIN_WORKERS" MONITOR_CPU="$MONITOR_CPU" \
    CLIENT_HOST=local CLIENT_IP="$CLIENT_IP" CLIENT_IFACE=lo CLIENT_NODE="$CLIENT_NODE" \
    CLIENT_CPUS="$CLIENT_CPUS" CLIENT_THREADS="$CLIENT_THREADS" \
    CLIENT_MEMORY_POLICY="$CLIENT_MEMORY_POLICY" CLIENT_MEMORY_NODES="$CLIENT_MEMORY_NODES" \
    CLIENT_ROOT="$CLIENT_ROOT" \
    CLIENT_BIN="$CLIENT_BIN" CLIENT_PREFETCH_MANIFEST="$ROOT/manifests/prefetch.tsv" \
    CLIENT_ORDER_MANIFEST="$ROOT/manifests/order.tsv" \
    "$VINYL_RUNNER" 2>&1 | tee "$SERIES_DIR/vinyl-controller.log"
printf '[%s] local Vinyl sweep complete\n' "$(date -u +%FT%TZ)" \
    | tee -a "$SERIES_DIR/progress.log"

printf '[%s] removing Vinyl file and trimming before local Goblin sweep\n' \
    "$(date -u +%FT%TZ)" | tee -a "$SERIES_DIR/progress.log"
rm -f "$VINYL_STORAGE_FILE"
[[ ! -e $VINYL_STORAGE_FILE ]] || die "could not remove Vinyl storage file"
sudo -n fstrim -v /mnt/local | tee "$SERIES_DIR/before-goblin-fstrim.txt"
[[ $(systemctl is-active numad 2>/dev/null || true) != active ]] ||
    die "numad became active between cache runs"

printf '[%s] starting local Goblin 256K-head sweep on node %s, client on nodes %s\n' \
    "$(date -u +%FT%TZ)" "$SERVER_NODE" "$CLIENT_NODES" | tee -a "$SERIES_DIR/progress.log"
env RUN_ID="$GOBLIN_RUN_ID" RAM_HEAD="$GOBLIN_HEAD" \
    MIRROR_CLIENT="$MIRROR_CLIENT" READ_IO_CHUNK="$READ_IO_CHUNK" \
    WRITE_IO_CHUNK="$WRITE_IO_CHUNK" MEASURED_CONCURRENCIES="$CONCURRENCIES" \
    PREWARM_CONCURRENCY="$PREWARM_CONCURRENCY" \
    SERVER_IP="$SERVER_IP" SERVER_IFACE=lo SERVER_NODE="$SERVER_NODE" \
    SERVER_CPUS="$SERVER_CPUS" SERVER_CORES="$SERVER_CORES" \
    ORIGIN_NODE="$ORIGIN_NODE" ORIGIN_CPUS="$ORIGIN_CPUS" ORIGIN_WORKERS="$ORIGIN_WORKERS" \
    MONITOR_CPU="$MONITOR_CPU" CLIENT_HOST=local CLIENT_IP="$CLIENT_IP" CLIENT_IFACE=lo \
    CLIENT_NODE="$CLIENT_NODE" CLIENT_CPUS="$CLIENT_CPUS" CLIENT_THREADS="$CLIENT_THREADS" \
    CLIENT_MEMORY_POLICY="$CLIENT_MEMORY_POLICY" CLIENT_MEMORY_NODES="$CLIENT_MEMORY_NODES" \
    CLIENT_ROOT="$CLIENT_ROOT" \
    CLIENT_BIN="$CLIENT_BIN" CLIENT_PREFETCH_MANIFEST="$ROOT/manifests/prefetch.tsv" \
    CLIENT_ORDER_MANIFEST="$ROOT/manifests/order.tsv" \
    "$GOBLIN_RUNNER" 2>&1 | tee "$SERIES_DIR/goblin-controller.log"
printf '[%s] local Goblin sweep complete\n' "$(date -u +%FT%TZ)" \
    | tee -a "$SERIES_DIR/progress.log"

printf 'completed_utc=%s\n' "$(date -u +%FT%TZ)" >> "$SERIES_DIR/config.txt"
printf '[%s] local-NUMA concurrency pair complete: %s\n' \
    "$(date -u +%FT%TZ)" "$SERIES_DIR" | tee -a "$SERIES_DIR/progress.log"
