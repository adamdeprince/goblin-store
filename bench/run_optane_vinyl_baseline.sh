#!/usr/bin/env bash
# One-pass, completely prewarmed Vinyl Cache baseline for the Optane small-object corpus.

set -Eeuo pipefail

ROOT=${ROOT:-/mnt/local/goblin-small-bench}
DATASET=${DATASET:-/mnt/local/data}
PREFETCH_MANIFEST=${PREFETCH_MANIFEST:-$ROOT/manifests/prefetch.tsv}
ORDER_MANIFEST=${ORDER_MANIFEST:-$ROOT/manifests/order.tsv}
TOOLS=${TOOLS:-$ROOT/tools}
NGINX_ORIGIN_SCRIPT=${NGINX_ORIGIN_SCRIPT:-$TOOLS/nginx_mirror_origin.sh}
NGINX_BIN=${NGINX_BIN:-/usr/sbin/nginx}
MONITOR_SCRIPT=${MONITOR_SCRIPT:-$TOOLS/monitor_benchmark_resources.py}
VINYL_PREFIX=${VINYL_PREFIX:-$ROOT/install-vinyl}
VINYL_BIN=${VINYL_BIN:-$VINYL_PREFIX/sbin/vinyld}
VINYLSTAT_BIN=${VINYLSTAT_BIN:-$VINYL_PREFIX/bin/vinylstat}

ORIGIN_PORT=${ORIGIN_PORT:-18000}
PROXY_PORT=${PROXY_PORT:-8080}
SERVER_IP=${SERVER_IP:-10.88.88.3}
SERVER_IFACE=${SERVER_IFACE:-ibp66s0}
SERVER_NODE=${SERVER_NODE:-1}
SERVER_CPUS=${SERVER_CPUS:-1,5,9,13,17,21,25,29,33,37,41,45,49,53,57,61,65,69,73,77,81,85,89,93}
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
PREWARM_CONCURRENCY=${PREWARM_CONCURRENCY:-32}
MEASURED_CONCURRENCY=${MEASURED_CONCURRENCY:-1}
MEASURED_CONCURRENCIES=${MEASURED_CONCURRENCIES:-$MEASURED_CONCURRENCY}
VINYL_STORAGE=${VINYL_STORAGE:-96G}
VINYL_ADVICE=${VINYL_ADVICE:-random}
VINYL_GRANULARITY=${VINYL_GRANULARITY:-4K}
HIT_PROBE_REQUESTS=${HIT_PROBE_REQUESTS:-1000}
RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
RUN_DIR=${RUN_DIR:-$ROOT/results/vinyl-baseline-$RUN_ID}
ORIGIN_WORK_DIR=${ORIGIN_WORK_DIR:-$RUN_DIR/nginx-origin}
ORIGIN_ACCESS_LOG=${ORIGIN_ACCESS_LOG:-$RUN_DIR/origin-access.tsv}
CACHE_DIR=${CACHE_DIR:-$ROOT/cache/vinyl}
STORAGE_FILE=${STORAGE_FILE:-$CACHE_DIR/vinyl.bin}
WORK_DIR=${WORK_DIR:-/dev/shm/goblin-small-bench-vinyl-$RUN_ID}
CLIENT_RUN_DIR=${CLIENT_RUN_DIR:-$CLIENT_ROOT/results/vinyl-$RUN_ID}

ORIGIN_PID=
VINYL_PID=
MONITOR_PID=

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

cleanup() {
    stop_pid "$MONITOR_PID"
    stop_pid "$VINYL_PID"
    stop_pid "$ORIGIN_PID"
    remote pkill -TERM -f "$CLIENT_BIN" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

origin_count() {
    if [[ -n $ORIGIN_PID ]] && kill -0 "$ORIGIN_PID" 2>/dev/null; then
        kill -USR1 "$ORIGIN_PID"
        sleep 0.2
    fi
    [[ -e $ORIGIN_ACCESS_LOG ]] && wc -l < "$ORIGIN_ACCESS_LOG" || printf '0\n'
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

mkdir -p "$RUN_DIR" "$CACHE_DIR"
for path in "$DATASET" "$PREFETCH_MANIFEST" "$ORDER_MANIFEST" "$NGINX_ORIGIN_SCRIPT" \
            "$NGINX_BIN" "$MONITOR_SCRIPT" "$VINYL_BIN" "$VINYLSTAT_BIN"; do
    [[ -e $path ]] || die "missing $path"
done
[[ $VINYL_ADVICE == random ]] || die "this small-object benchmark requires random advice"
[[ $CLIENT_THREADS =~ ^[1-9][0-9]*$ ]] || die "bad CLIENT_THREADS"
for concurrency in $MEASURED_CONCURRENCIES; do
    [[ $concurrency =~ ^[1-9][0-9]*$ ]] || die "bad measured concurrency: $concurrency"
done
[[ $(find "$DATASET" -maxdepth 1 -type f | wc -l) == 373165 ]] || die "bad corpus count"
[[ $(wc -l < "$PREFETCH_MANIFEST") == 373165 ]] || die "bad prefetch manifest length"
[[ $(wc -l < "$ORDER_MANIFEST") == 6141630 ]] || die "bad order manifest length"
remote test -x "$CLIENT_BIN" || die "missing client binary on $CLIENT_HOST"
client_prefetch_count=$(remote awk 'END { print NR }' "$CLIENT_PREFETCH_MANIFEST")
client_order_count=$(remote awk 'END { print NR }' "$CLIENT_ORDER_MANIFEST")
[[ $client_prefetch_count == 373165 ]] || die "bad client prefetch manifest length"
[[ $client_order_count == 6141630 ]] || die "bad client order manifest length"
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

rm -f "$STORAGE_FILE"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
remote rm -rf "$CLIENT_RUN_DIR"
remote mkdir -p "$CLIENT_RUN_DIR"

printf '[%s] allocating Vinyl storage %s with %s granularity and %s advice\n' \
    "$(date -u +%FT%TZ)" "$VINYL_STORAGE" "$VINYL_GRANULARITY" "$VINYL_ADVICE"
fallocate -l "$VINYL_STORAGE" "$STORAGE_FILE"
expected_size=$(numfmt --from=iec "$VINYL_STORAGE")
actual_size=$(stat -c %s "$STORAGE_FILE")
allocated_size=$(( $(stat -c %b "$STORAGE_FILE") * 512 ))
[[ $actual_size == "$expected_size" ]] || die "bad Vinyl storage length"
(( allocated_size >= actual_size )) || die "Vinyl storage file is sparse"

cat > "$RUN_DIR/benchmark.vcl" <<EOF
vcl 4.1;
backend default {
    .host = "127.0.0.1";
    .port = "$ORIGIN_PORT";
}
sub vcl_backend_response {
    set beresp.ttl = 7d;
    set beresp.grace = 0s;
    set beresp.keep = 0s;
    set beresp.do_stream = true;
    return (deliver);
}
EOF

{
    date -u +%FT%TZ
    uname -a
    "$VINYL_BIN" -V 2>&1
    sha256sum "$VINYL_BIN" "$NGINX_ORIGIN_SCRIPT" "$PREFETCH_MANIFEST" "$ORDER_MANIFEST"
    printf 'storage=%s\nstorage_bytes=%s\nallocated_bytes=%s\nadvice=%s\ngranularity=%s\n' \
        "$VINYL_STORAGE" "$actual_size" "$allocated_size" "$VINYL_ADVICE" \
        "$VINYL_GRANULARITY"
    printf 'prewarm_concurrency=%s\nmeasured_concurrencies=%s\n' \
        "$PREWARM_CONCURRENCY" "$MEASURED_CONCURRENCIES"
    printf 'origin=nginx\norigin_workers=%s\norigin_cpus=%s\n' \
        "$ORIGIN_WORKERS" "$ORIGIN_CPUS"
    "$NGINX_BIN" -v 2>&1
    printf 'server_ip=%s\nserver_iface=%s\nserver_node=%s\nserver_cpus=%s\n' \
        "$SERVER_IP" "$SERVER_IFACE" "$SERVER_NODE" "$SERVER_CPUS"
    printf 'client_host=%s\nclient_ip=%s\nclient_iface=%s\nclient_node=%s\nclient_cpus=%s\n' \
        "$CLIENT_HOST" "$CLIENT_IP" "$CLIENT_IFACE" "$CLIENT_NODE" "$CLIENT_CPUS"
    printf 'client_threads=%s\nclient_memory_policy=%s\nclient_memory_nodes=%s\n' \
        "$CLIENT_THREADS" "$CLIENT_MEMORY_POLICY" "$CLIENT_MEMORY_NODES"
    df -hT "$STORAGE_FILE" "$WORK_DIR"
} > "$RUN_DIR/identities.txt"
remote sha256sum "$CLIENT_BIN" "$CLIENT_PREFETCH_MANIFEST" "$CLIENT_ORDER_MANIFEST" \
    >> "$RUN_DIR/identities.txt"
remote df -hT /dev/shm >> "$RUN_DIR/identities.txt"

/usr/bin/numactl --physcpubind="$ORIGIN_CPUS" --membind="$ORIGIN_NODE" \
    "$NGINX_ORIGIN_SCRIPT" --nginx "$NGINX_BIN" --directory "$DATASET" \
    --access-log "$ORIGIN_ACCESS_LOG" --work-dir "$ORIGIN_WORK_DIR" \
    --port "$ORIGIN_PORT" --workers "$ORIGIN_WORKERS" \
    > "$RUN_DIR/origin.stdout" 2> "$RUN_DIR/origin.stderr" &
ORIGIN_PID=$!
wait_for_listener "$ORIGIN_PORT" "$ORIGIN_PID" || die "origin failed to listen"

/usr/bin/numactl --physcpubind="$SERVER_CPUS" --membind="$SERVER_NODE" \
    "$VINYL_BIN" -F -j none -n "$WORK_DIR" -a "$SERVER_IP:$PROXY_PORT" \
    -f "$RUN_DIR/benchmark.vcl" \
    -s "file,$STORAGE_FILE,$VINYL_STORAGE,$VINYL_GRANULARITY,$VINYL_ADVICE" \
    -p default_grace=0 -p default_keep=0 \
    > "$RUN_DIR/server.stdout" 2> "$RUN_DIR/server.stderr" &
VINYL_PID=$!
wait_for_listener "$PROXY_PORT" "$VINYL_PID" || {
    tail -200 "$RUN_DIR/server.stderr" >&2 || true
    die "Vinyl failed to listen"
}
printf '%s\n' "$VINYL_PID" > "$RUN_DIR/server.pid"
taskset -pc "$VINYL_PID" > "$RUN_DIR/server-affinity.txt"

taskset -c "$MONITOR_CPU" python3 "$MONITOR_SCRIPT" --output-dir "$RUN_DIR/monitor" \
    --interval 1 --match vinyld --match mirror-proxy-benchmark \
    > "$RUN_DIR/monitor.stdout" 2> "$RUN_DIR/monitor.stderr" &
MONITOR_PID=$!

printf '[%s] warming %s Vinyl objects at concurrency %s\n' \
    "$(date -u +%FT%TZ)" "$(wc -l < "$PREFETCH_MANIFEST")" "$PREWARM_CONCURRENCY"
run_client "$CLIENT_PREFETCH_MANIFEST" "$CLIENT_RUN_DIR/prewarm.csv" \
    "$PREWARM_CONCURRENCY" \
    2>&1 | tee "$RUN_DIR/prewarm.log"
copy_client_artifact "$CLIENT_RUN_DIR/prewarm.csv" "$RUN_DIR/prewarm.csv"
expected=$(wc -l < "$PREFETCH_MANIFEST")
[[ $(origin_count) == "$expected" ]] ||
    die "Vinyl prewarm made $(origin_count) origin requests; expected $expected"
"$VINYLSTAT_BIN" -n "$WORK_DIR" -1 > "$RUN_DIR/vinylstat-after-prewarm.txt"

printf '[%s] validating %s warmed requests before the measured pass\n' \
    "$(date -u +%FT%TZ)" "$HIT_PROBE_REQUESTS"
run_client "$CLIENT_ORDER_MANIFEST" "$CLIENT_RUN_DIR/hit-probe.csv" 1 \
    --max-requests "$HIT_PROBE_REQUESTS" 2>&1 | tee "$RUN_DIR/hit-probe.log"
copy_client_artifact "$CLIENT_RUN_DIR/hit-probe.csv" "$RUN_DIR/hit-probe.csv"
[[ $(origin_count) == "$expected" ]] || die "warm-cache probe generated origin traffic"
"$VINYLSTAT_BIN" -n "$WORK_DIR" -1 > "$RUN_DIR/vinylstat-after-hit-probe.txt"

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
    "$VINYLSTAT_BIN" -n "$WORK_DIR" -1 > "$RUN_DIR/vinylstat-$state_label.txt"
done

stop_pid "$MONITOR_PID"
MONITOR_PID=
stop_pid "$VINYL_PID"
VINYL_PID=
stop_pid "$ORIGIN_PID"
ORIGIN_PID=

sha256sum "${result_files[@]}" > "$RUN_DIR/result-checksums.txt"
printf '[%s] Vinyl baseline complete: %s\n' "$(date -u +%FT%TZ)" "$RUN_DIR"
if (( measured_request_failures )); then
    printf 'WARNING: one or more measured passes contained failed requests; see %s\n' \
        "$RUN_DIR/measured-request-failures.log"
fi
