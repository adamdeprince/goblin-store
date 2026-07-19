#!/usr/bin/env bash
# One-pass, completely prewarmed Goblin Store baseline for the Optane small-object corpus.

set -Eeuo pipefail

ROOT=${ROOT:-/mnt/local/goblin-small-bench}
DATASET=${DATASET:-/mnt/local/data}
PREFETCH_MANIFEST=${PREFETCH_MANIFEST:-$ROOT/manifests/prefetch.tsv}
ORDER_MANIFEST=${ORDER_MANIFEST:-$ROOT/manifests/order.tsv}
TOOLS=${TOOLS:-$ROOT/tools}
NGINX_ORIGIN_SCRIPT=${NGINX_ORIGIN_SCRIPT:-$TOOLS/nginx_mirror_origin.sh}
NGINX_BIN=${NGINX_BIN:-/usr/sbin/nginx}
MONITOR_SCRIPT=${MONITOR_SCRIPT:-$TOOLS/monitor_benchmark_resources.py}
THERMAL_MONITOR_SCRIPT=${THERMAL_MONITOR_SCRIPT:-$TOOLS/monitor_nvme_temperature.py}
GOBLIN_BUILD=${GOBLIN_BUILD:-$ROOT/build-goblin}
GOBLIN_BIN=${GOBLIN_BIN:-$GOBLIN_BUILD/goblin-store}
PATH_PREP_BIN=${PATH_PREP_BIN:-$GOBLIN_BUILD/goblin-store-path-prep}
GCC_RUNTIME=${GCC_RUNTIME:-$HOME/opt/gcc-16.1/lib64}

ORIGIN_PORT=${ORIGIN_PORT:-18000}
PROXY_PORT=${PROXY_PORT:-8080}
SERVER_IP=${SERVER_IP:-10.88.88.3}
SERVER_IFACE=${SERVER_IFACE:-ibp66s0}
SERVER_NODE=${SERVER_NODE:-1}
SERVER_CPUS=${SERVER_CPUS:-1,5,9,13,17,21,25,29,33,37,41,45,49,53,57,61,65,69,73,77,81,85,89,93}
SERVER_CORES=${SERVER_CORES:-0}
ORIGIN_NODE=${ORIGIN_NODE:-0}
ORIGIN_CPUS=${ORIGIN_CPUS:-0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92}
ORIGIN_WORKERS=${ORIGIN_WORKERS:-24}
MONITOR_CPU=${MONITOR_CPU:-3}
CLIENT_HOST=${CLIENT_HOST:-rain}
CLIENT_IP=${CLIENT_IP:-10.88.88.2}
CLIENT_IFACE=${CLIENT_IFACE:-ibp65s0}
CLIENT_NODE=${CLIENT_NODE:-1}
CLIENT_CPUS=${CLIENT_CPUS:-${CLIENT_CPU:-45}}
CLIENT_THREADS=${CLIENT_THREADS:-1}
CLIENT_MEMORY_POLICY=${CLIENT_MEMORY_POLICY:-bind}
CLIENT_MEMORY_NODES=${CLIENT_MEMORY_NODES:-$CLIENT_NODE}
CLIENT_ROOT=${CLIENT_ROOT:-/dev/shm/goblin-small-client}
CLIENT_BIN=${CLIENT_BIN:-$CLIENT_ROOT/mirror-proxy-benchmark}
CLIENT_PREFETCH_MANIFEST=${CLIENT_PREFETCH_MANIFEST:-$CLIENT_ROOT/manifests/prefetch.tsv}
CLIENT_ORDER_MANIFEST=${CLIENT_ORDER_MANIFEST:-$CLIENT_ROOT/manifests/order.tsv}

MEMORY=${MEMORY:-48G}
SMALL_MEMORY=${SMALL_MEMORY:-48G}
BLOCK=${BLOCK:-2M}
RAM_HEAD=${RAM_HEAD:-8K}
HEAD_SLUG=${HEAD_SLUG:-${RAM_HEAD,,}}
MIRROR_CLIENT=${MIRROR_CLIENT:-curl}
READ_IO_CHUNK=${READ_IO_CHUNK:-256K}
WRITE_IO_CHUNK=${WRITE_IO_CHUNK:-1M}
NVME_CONTROLLER=${NVME_CONTROLLER:-nvme0}
HUGETLB_PAGES=${HUGETLB_PAGES:-49152} # 96 GiB / 2 MiB
PREWARM_CONCURRENCY=${PREWARM_CONCURRENCY:-32}
MEASURED_CONCURRENCY=${MEASURED_CONCURRENCY:-1}
MEASURED_CONCURRENCIES=${MEASURED_CONCURRENCIES:-$MEASURED_CONCURRENCY}
HIT_PROBE_REQUESTS=${HIT_PROBE_REQUESTS:-1000}
RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
RUN_DIR=${RUN_DIR:-$ROOT/results/goblin-$HEAD_SLUG-baseline-$RUN_ID}
ORIGIN_WORK_DIR=${ORIGIN_WORK_DIR:-$RUN_DIR/nginx-origin}
ORIGIN_ACCESS_LOG=${ORIGIN_ACCESS_LOG:-$RUN_DIR/origin-access.tsv}
POOL=${POOL:-$ROOT/cache/goblin-$HEAD_SLUG-$RUN_ID}
CLIENT_RUN_DIR=${CLIENT_RUN_DIR:-$CLIENT_ROOT/results/goblin-$HEAD_SLUG-$RUN_ID}

ORIGIN_PID=
GOBLIN_PID=
MONITOR_PID=
THERMAL_MONITOR_PID=
ORIGINAL_HUGETLB_PAGES=

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

wait_for_listener() {
    local port=$1 pid=$2
    for _ in $(seq 1 900); do
        kill -0 "$pid" 2>/dev/null || return 1
        if ss -H -ltn | awk -v port=":$port" '$4 ~ port "$" {found=1} END {exit !found}'; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

stop_pid() {
    local pid=${1:-}
    [[ -n $pid ]] || return 0
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        for _ in $(seq 1 300); do
            kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null || true; return 0; }
            sleep 0.2
        done
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

remote() {
    if [[ $CLIENT_HOST == local ]]; then
        "$@"
        return
    fi
    local command
    printf -v command '%q ' "$@"
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_HOST" "$command"
}

copy_client_artifact() {
    local remote_path=$1 local_path=$2
    if [[ $CLIENT_HOST == local ]]; then
        cp "$remote_path" "$local_path"
    else
        scp -q "$CLIENT_HOST:$remote_path" "$local_path"
    fi
}

restore_hugetlb() {
    [[ -n $ORIGINAL_HUGETLB_PAGES ]] || return 0
    local control=/sys/devices/system/node/node${SERVER_NODE}/hugepages/hugepages-2048kB/nr_hugepages
    sudo -n sh -c "echo '$ORIGINAL_HUGETLB_PAGES' > '$control'" || true
    ORIGINAL_HUGETLB_PAGES=
}

cleanup() {
    stop_pid "$MONITOR_PID"
    stop_pid "$THERMAL_MONITOR_PID"
    stop_pid "$GOBLIN_PID"
    stop_pid "$ORIGIN_PID"
    remote pkill -TERM -f "$CLIENT_BIN" 2>/dev/null || true
    restore_hugetlb
}
trap cleanup EXIT INT TERM

origin_count() {
    if [[ -n $ORIGIN_PID ]] && kill -0 "$ORIGIN_PID" 2>/dev/null; then
        kill -USR1 "$ORIGIN_PID"
        sleep 0.2
    fi
    [[ -e $ORIGIN_ACCESS_LOG ]] && wc -l < "$ORIGIN_ACCESS_LOG" || printf '0\n'
}

run_client() {
    local manifest=$1 output=$2 concurrency=$3
    shift 3
    local numa_args=(--physcpubind="$CLIENT_CPUS")
    case $CLIENT_MEMORY_POLICY in
        bind) numa_args+=(--membind="$CLIENT_MEMORY_NODES") ;;
        interleave) numa_args+=(--interleave="$CLIENT_MEMORY_NODES") ;;
        local) numa_args+=(--localalloc) ;;
        *) die "CLIENT_MEMORY_POLICY must be bind, interleave, or local" ;;
    esac
    remote /usr/bin/numactl "${numa_args[@]}" \
        "$CLIENT_BIN" --host "$SERVER_IP" --source "$CLIENT_IP" --port "$PROXY_PORT" \
        --manifest "$manifest" --output "$output" --concurrency "$concurrency" \
        --threads "$CLIENT_THREADS" --passes 1 --manifest-order "$@"
}

capture_server_state() {
    local label=$1
    sudo -n cat "/proc/$GOBLIN_PID/numa_maps" > "$RUN_DIR/numa-maps-$label.txt"
    cat "/proc/$GOBLIN_PID/status" > "$RUN_DIR/process-status-$label.txt"
    du -sh "$POOL" > "$RUN_DIR/pool-usage-$label.txt"
    df -hT "$POOL" >> "$RUN_DIR/pool-usage-$label.txt"
}

mkdir -p "$RUN_DIR"
[[ $MIRROR_CLIENT == curl || $MIRROR_CLIENT == uring ]] ||
    die "MIRROR_CLIENT must be curl or uring"
[[ $CLIENT_THREADS =~ ^[1-9][0-9]*$ ]] || die "bad CLIENT_THREADS"
for concurrency in $MEASURED_CONCURRENCIES; do
    [[ $concurrency =~ ^[1-9][0-9]*$ ]] || die "bad measured concurrency: $concurrency"
done
for path in "$DATASET" "$PREFETCH_MANIFEST" "$ORDER_MANIFEST" "$NGINX_ORIGIN_SCRIPT" \
            "$NGINX_BIN" "$MONITOR_SCRIPT" "$THERMAL_MONITOR_SCRIPT" "$GOBLIN_BIN" "$PATH_PREP_BIN" \
            "$GCC_RUNTIME"; do
    [[ -e $path ]] || die "missing $path"
done
sudo -n true || die "passwordless sudo is required for compaction and HugeTLB reservation"
[[ $(systemctl is-active numad 2>/dev/null || true) != active ]] ||
    die "numad must be stopped for deterministic NUMA placement"
[[ $(find "$DATASET" -maxdepth 1 -type f | wc -l) == 373165 ]] || die "bad corpus count"
[[ $(wc -l < "$PREFETCH_MANIFEST") == 373165 ]] || die "bad prefetch manifest length"
[[ $(wc -l < "$ORDER_MANIFEST") == 6141630 ]] || die "bad order manifest length"
remote test -x "$CLIENT_BIN" || die "missing client binary on $CLIENT_HOST"
[[ $(remote awk 'END { print NR }' "$CLIENT_PREFETCH_MANIFEST") == 373165 ]] ||
    die "bad client prefetch manifest length"
[[ $(remote awk 'END { print NR }' "$CLIENT_ORDER_MANIFEST") == 6141630 ]] ||
    die "bad client order manifest length"

if [[ $CLIENT_HOST == local ]]; then
    [[ $SERVER_IP == 127.0.0.1 && $CLIENT_IP == 127.0.0.1 ]] ||
        die "local client mode requires loopback server and client addresses"
else
    server_route=$(ip -4 route get "$CLIENT_IP")
    client_route=$(remote ip -4 route get "$SERVER_IP")
    [[ $server_route == *"dev $SERVER_IFACE"* && $server_route == *"src $SERVER_IP"* ]] ||
        die "server IPoIB route is wrong: $server_route"
    [[ $client_route == *"dev $CLIENT_IFACE"* && $client_route == *"src $CLIENT_IP"* ]] ||
        die "client IPoIB route is wrong: $client_route"
    [[ $(cat "/sys/class/net/$SERVER_IFACE/mode") == connected ]] ||
        die "$SERVER_IFACE is not in connected mode"
    [[ $(cat "/sys/class/net/$SERVER_IFACE/mtu") == 65520 ]] ||
        die "$SERVER_IFACE MTU is not 65520"
    remote test "$(remote cat "/sys/class/net/$CLIENT_IFACE/mode")" = connected ||
        die "$CLIENT_IFACE is not in connected mode"
    remote test "$(remote cat "/sys/class/net/$CLIENT_IFACE/mtu")" = 65520 ||
        die "$CLIENT_IFACE MTU is not 65520"
fi
ss -H -ltn | awk -v origin=":$ORIGIN_PORT" -v proxy=":$PROXY_PORT" \
    '$4 ~ origin "$" || $4 ~ proxy "$" {busy=1} END {exit !busy}' &&
    die "benchmark port is already occupied"

[[ ! -e $POOL ]] || die "fresh Goblin pool already exists: $POOL"
remote rm -rf "$CLIENT_RUN_DIR"
remote mkdir -p "$CLIENT_RUN_DIR"

{
    date -u +%FT%TZ
    uname -a
    env LD_LIBRARY_PATH="$GCC_RUNTIME" "$GOBLIN_BIN" --help 2>&1 | head -1
    sha256sum "$GOBLIN_BIN" "$PATH_PREP_BIN" "$NGINX_ORIGIN_SCRIPT" \
        "$PREFETCH_MANIFEST" "$ORDER_MANIFEST"
    printf 'memory=%s\nsmall_memory=%s\nblock=%s\nram_head=%s\n' \
        "$MEMORY" "$SMALL_MEMORY" "$BLOCK" "$RAM_HEAD"
    printf 'mirror_client=%s\nread_io_chunk=%s\nwrite_io_chunk=%s\n' \
        "$MIRROR_CLIENT" "$READ_IO_CHUNK" "$WRITE_IO_CHUNK"
    printf 'prewarm_concurrency=%s\nmeasured_concurrencies=%s\n' \
        "$PREWARM_CONCURRENCY" "$MEASURED_CONCURRENCIES"
    printf 'origin=nginx\norigin_workers=%s\norigin_cpus=%s\n' \
        "$ORIGIN_WORKERS" "$ORIGIN_CPUS"
    "$NGINX_BIN" -v 2>&1
    printf 'nvme_controller=%s\nnvme_model=%s\n' "$NVME_CONTROLLER" \
        "$(cat "/sys/class/nvme/$NVME_CONTROLLER/model")"
    printf 'pool=%s\nserver_ip=%s\nserver_iface=%s\nserver_node=%s\nserver_cpus=%s\nserver_cores=%s\n' \
        "$POOL" "$SERVER_IP" "$SERVER_IFACE" "$SERVER_NODE" "$SERVER_CPUS" "$SERVER_CORES"
    printf 'client_host=%s\nclient_ip=%s\nclient_iface=%s\nclient_node=%s\nclient_cpus=%s\n' \
        "$CLIENT_HOST" "$CLIENT_IP" "$CLIENT_IFACE" "$CLIENT_NODE" "$CLIENT_CPUS"
    printf 'client_threads=%s\nclient_memory_policy=%s\nclient_memory_nodes=%s\n' \
        "$CLIENT_THREADS" "$CLIENT_MEMORY_POLICY" "$CLIENT_MEMORY_NODES"
    printf 'numad=%s\n' "$(systemctl is-active numad 2>/dev/null || true)"
    printf 'memlock_kib=%s\n' "$(ulimit -l)"
    df -hT "$ROOT"
} > "$RUN_DIR/identities.txt"
remote sha256sum "$CLIENT_BIN" "$CLIENT_PREFETCH_MANIFEST" "$CLIENT_ORDER_MANIFEST" \
    >> "$RUN_DIR/identities.txt"
remote df -hT /dev/shm >> "$RUN_DIR/identities.txt"

env LD_LIBRARY_PATH="$GCC_RUNTIME" "$PATH_PREP_BIN" "$POOL"

taskset -c "$MONITOR_CPU" python3 "$THERMAL_MONITOR_SCRIPT" \
    --controller "$NVME_CONTROLLER" --output "$RUN_DIR/nvme-temperature.csv" --interval 1 \
    > "$RUN_DIR/thermal-monitor.stdout" 2> "$RUN_DIR/thermal-monitor.stderr" &
THERMAL_MONITOR_PID=$!
sleep 1
kill -0 "$THERMAL_MONITOR_PID" 2>/dev/null || {
    cat "$RUN_DIR/thermal-monitor.stderr" >&2 || true
    die "NVMe thermal monitor failed to start"
}

hugetlb_control=/sys/devices/system/node/node${SERVER_NODE}/hugepages/hugepages-2048kB/nr_hugepages
ORIGINAL_HUGETLB_PAGES=$(cat "$hugetlb_control")
printf '[%s] compacting memory and requesting %s HugeTLB pages on node %s\n' \
    "$(date -u +%FT%TZ)" "$HUGETLB_PAGES" "$SERVER_NODE" | tee "$RUN_DIR/hugetlb.txt"
sudo -n sh -c 'echo 1 > /proc/sys/vm/compact_memory'
sudo -n sh -c "echo '$HUGETLB_PAGES' > '$hugetlb_control'"
printf 'original_pages=%s\nrequested_pages=%s\nactual_pages=%s\n' \
    "$ORIGINAL_HUGETLB_PAGES" "$HUGETLB_PAGES" "$(cat "$hugetlb_control")" \
    | tee -a "$RUN_DIR/hugetlb.txt"

/usr/bin/numactl --physcpubind="$ORIGIN_CPUS" --membind="$ORIGIN_NODE" \
    "$NGINX_ORIGIN_SCRIPT" --nginx "$NGINX_BIN" --directory "$DATASET" \
    --access-log "$ORIGIN_ACCESS_LOG" --work-dir "$ORIGIN_WORK_DIR" \
    --port "$ORIGIN_PORT" --workers "$ORIGIN_WORKERS" \
    > "$RUN_DIR/origin.stdout" 2> "$RUN_DIR/origin.stderr" &
ORIGIN_PID=$!
wait_for_listener "$ORIGIN_PORT" "$ORIGIN_PID" || die "origin failed to listen"

env LD_LIBRARY_PATH="$GCC_RUNTIME" \
    /usr/bin/numactl --physcpubind="$SERVER_CPUS" --membind="$SERVER_NODE" \
    "$GOBLIN_BIN" --mirror "http://127.0.0.1:$ORIGIN_PORT" \
    --mirror-client "$MIRROR_CLIENT" \
    --listen-address "$SERVER_IP" --http-port "$PROXY_PORT" --no-memcache --net async \
    --numa "$SERVER_NODE" --cores "$SERVER_CORES" \
    --memory "$MEMORY" --small-memory "$SMALL_MEMORY" \
    --block "$BLOCK" --ram-head "$RAM_HEAD" --io-chunk "$READ_IO_CHUNK" \
    --write-io-chunk "$WRITE_IO_CHUNK" --no-numa-promotion --ssd-dir "$POOL" \
    > "$RUN_DIR/server.stdout" 2> "$RUN_DIR/server.stderr" &
GOBLIN_PID=$!
wait_for_listener "$PROXY_PORT" "$GOBLIN_PID" || {
    tail -200 "$RUN_DIR/server.stdout" >&2 || true
    tail -200 "$RUN_DIR/server.stderr" >&2 || true
    die "Goblin Store failed to listen"
}
printf '%s\n' "$GOBLIN_PID" > "$RUN_DIR/server.pid"
taskset -pc "$GOBLIN_PID" > "$RUN_DIR/server-affinity.txt"

taskset -c "$MONITOR_CPU" python3 "$MONITOR_SCRIPT" --output-dir "$RUN_DIR/monitor" \
    --interval 1 --match goblin-store --match mirror-proxy-benchmark \
    > "$RUN_DIR/monitor.stdout" 2> "$RUN_DIR/monitor.stderr" &
MONITOR_PID=$!

printf '[%s] warming %s Goblin objects at concurrency %s with %s heads\n' \
    "$(date -u +%FT%TZ)" "$(wc -l < "$PREFETCH_MANIFEST")" \
    "$PREWARM_CONCURRENCY" "$RAM_HEAD"
run_client "$CLIENT_PREFETCH_MANIFEST" "$CLIENT_RUN_DIR/prewarm.csv" \
    "$PREWARM_CONCURRENCY" 2>&1 | tee "$RUN_DIR/prewarm.log"
copy_client_artifact "$CLIENT_RUN_DIR/prewarm.csv" "$RUN_DIR/prewarm.csv"
expected=$(wc -l < "$PREFETCH_MANIFEST")
[[ $(origin_count) == "$expected" ]] ||
    die "Goblin prewarm made $(origin_count) origin requests; expected $expected"
capture_server_state after-prewarm

printf '[%s] validating %s warmed requests before the measured pass\n' \
    "$(date -u +%FT%TZ)" "$HIT_PROBE_REQUESTS"
run_client "$CLIENT_ORDER_MANIFEST" "$CLIENT_RUN_DIR/hit-probe.csv" 1 \
    --max-requests "$HIT_PROBE_REQUESTS" 2>&1 | tee "$RUN_DIR/hit-probe.log"
copy_client_artifact "$CLIENT_RUN_DIR/hit-probe.csv" "$RUN_DIR/hit-probe.csv"
[[ $(origin_count) == "$expected" ]] || die "warm-cache probe generated origin traffic"

read -r -a measured_values <<< "$MEASURED_CONCURRENCIES"
result_files=("$RUN_DIR/prewarm.csv" "$RUN_DIR/hit-probe.csv")
measured_request_failures=0
for concurrency in "${measured_values[@]}"; do
    if (( ${#measured_values[@]} == 1 )); then
        stem=one-pass
        state_label=after-pass
    else
        stem=one-pass-c$concurrency
        state_label=after-pass-c$concurrency
    fi
    printf '[%s] running exact-order pass: %s requests, concurrency %s, client loops up to %s\n' \
        "$(date -u +%FT%TZ)" "$(wc -l < "$ORDER_MANIFEST")" "$concurrency" "$CLIENT_THREADS"
    set +e
    run_client "$CLIENT_ORDER_MANIFEST" "$CLIENT_RUN_DIR/$stem.csv" "$concurrency" \
        2>&1 | tee "$RUN_DIR/$stem.log"
    pipeline_status=("${PIPESTATUS[@]}")
    set -e
    client_status=${pipeline_status[0]}
    tee_status=${pipeline_status[1]}
    (( tee_status == 0 )) || die "could not write concurrency-$concurrency client log"
    (( client_status <= 1 )) || die "concurrency-$concurrency client failed with status $client_status"
    if (( client_status == 1 )); then
        measured_request_failures=1
        printf 'WARNING: concurrency-%s completed with one or more failed requests; continuing sweep\n' \
            "$concurrency" | tee -a "$RUN_DIR/measured-request-failures.log"
    fi
    copy_client_artifact "$CLIENT_RUN_DIR/$stem.csv" "$RUN_DIR/$stem.csv"
    result_files+=("$RUN_DIR/$stem.csv")
    [[ $(origin_count) == "$expected" ]] || \
        die "concurrency-$concurrency measured pass generated origin traffic"
    capture_server_state "$state_label"
done

stop_pid "$MONITOR_PID"
MONITOR_PID=
stop_pid "$THERMAL_MONITOR_PID"
THERMAL_MONITOR_PID=
stop_pid "$GOBLIN_PID"
GOBLIN_PID=
stop_pid "$ORIGIN_PID"
ORIGIN_PID=
restore_hugetlb

sha256sum "${result_files[@]}" > "$RUN_DIR/result-checksums.txt"
printf '[%s] Goblin %s baseline complete: %s\n' \
    "$(date -u +%FT%TZ)" "$RAM_HEAD" "$RUN_DIR"
if (( measured_request_failures )); then
    printf 'WARNING: one or more measured passes contained failed requests; see %s\n' \
        "$RUN_DIR/measured-request-failures.log"
fi
