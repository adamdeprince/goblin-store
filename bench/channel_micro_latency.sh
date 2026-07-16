#!/usr/bin/env bash
# Baseline/candidate small-object latency A/B for the Dopey/Rain direct 10 Gb link.
# Run on Dopey. Raw nanosecond traces stay on Rain's local /var/tmp.

set -Eeuo pipefail

ROOT=${ROOT:-/var/tmp/goblin-channel-opt-20260715}
BASELINE_BIN=${BASELINE_BIN:-$ROOT/baseline/build-make-release/goblin-store}
CANDIDATE_BIN=${CANDIDATE_BIN:-$ROOT/candidate/build-make-release/goblin-store}
PATH_PREP_BIN=${PATH_PREP_BIN:-$ROOT/candidate/build-make-release/goblin-store-path-prep}
BASELINE_DESCRIPTION=${BASELINE_DESCRIPTION:-pre-change baseline}
CANDIDATE_DESCRIPTION=${CANDIDATE_DESCRIPTION:-candidate}
CLIENT_BIN=${CLIENT_BIN:-/var/tmp/goblin-channel-bench-20260715}
GCC_LIB=${GCC_LIB:-/home/adam/opt/gcc-16.1/lib64}

CLIENT_HOST=${CLIENT_HOST:-rain}
SERVER_IP=${SERVER_IP:-10.77.77.1}
CLIENT_IP=${CLIENT_IP:-10.77.77.2}
SERVER_IFACE=${SERVER_IFACE:-eno2}
CLIENT_IFACE=${CLIENT_IFACE:-eno2}
SERVER_CPU=${SERVER_CPU:-4}
CLIENT_CPU=${CLIENT_CPU:-4}
MEMCACHE_PORT=${MEMCACHE_PORT:-12131}
HTTP_PORT=${HTTP_PORT:-18080}

OBJECT_BYTES=${OBJECT_BYTES:-1024}
KEYS=${KEYS:-400000}
WARMUP_REQUESTS=${WARMUP_REQUESTS:-100000}
CASE_REQUESTS=${CASE_REQUESTS:-1000000}
SEED=${SEED:-0x243f6a8885a308d3}

RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-channel-micro}
RUN_DIR=${RUN_DIR:-$ROOT/artifacts/$RUN_ID}
POOL_DIR=${POOL_DIR:-/var/tmp/goblin-channel-pool-$RUN_ID}
CLIENT_RAW_DIR=${CLIENT_RAW_DIR:-/var/tmp/goblin-channel-latency-$RUN_ID}

SERVER_PID=
SERVER_LAUNCH_PID=

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

remote_command() {
    local command
    printf -v command '%q ' "$@"
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_HOST" "$command"
}

stop_server() {
    if [[ -n ${SERVER_PID:-} ]] && sudo -n kill -0 "$SERVER_PID" 2>/dev/null; then
        sudo -n kill -TERM "$SERVER_PID"
        for _ in $(seq 1 150); do
            sudo -n kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.2
        done
        if sudo -n kill -0 "$SERVER_PID" 2>/dev/null; then
            sudo -n kill -KILL "$SERVER_PID"
        fi
    fi
    if [[ -n ${SERVER_LAUNCH_PID:-} ]]; then
        wait "$SERVER_LAUNCH_PID" 2>/dev/null || true
    fi
    SERVER_PID=
    SERVER_LAUNCH_PID=
}

trap stop_server EXIT INT TERM

[[ $(hostname -s) == dopey ]] || die 'run this script on dopey'
sudo -n true || die 'passwordless sudo is required on dopey'
[[ -x $BASELINE_BIN && -x $CANDIDATE_BIN && -x $PATH_PREP_BIN ]] || die 'missing server binary'
remote_command test -x "$CLIENT_BIN" || die 'missing benchmark client on rain'
[[ $(ip -4 route get "$CLIENT_IP") == *"dev $SERVER_IFACE"* ]] || die 'Dopey direct route missing'
[[ $(remote_command ip -4 route get "$SERVER_IP") == *"dev $CLIENT_IFACE"* ]] ||
    die 'Rain direct route missing'
[[ $(cat "/sys/class/net/$SERVER_IFACE/device/numa_node") == 0 ]] || die 'Dopey NIC not on node 0'
[[ $(remote_command cat "/sys/class/net/$CLIENT_IFACE/device/numa_node") == 0 ]] ||
    die 'Rain NIC not on node 0'
pgrep -x goblin-store >/dev/null && die 'goblin-store already running on dopey'
remote_command pgrep -x goblin-bench >/dev/null 2>&1 && die 'goblin-bench already running on rain'
[[ ! -e $RUN_DIR ]] || die "run directory already exists: $RUN_DIR"
[[ ! -e $POOL_DIR ]] || die "pool directory already exists: $POOL_DIR"
remote_command test ! -e "$CLIENT_RAW_DIR" ||
    die "client trace directory already exists on $CLIENT_HOST: $CLIENT_RAW_DIR"

mkdir -p "$RUN_DIR"
remote_command mkdir -p "$CLIENT_RAW_DIR"
env LD_LIBRARY_PATH="$GCC_LIB" "$PATH_PREP_BIN" "$POOL_DIR"

{
    printf 'run_id=%s\n' "$RUN_ID"
    printf 'started_utc=%s\n' "$(date -u +%FT%TZ)"
    printf 'variant_order=baseline,candidate,candidate,baseline\n'
    printf 'baseline_description=%s\n' "$BASELINE_DESCRIPTION"
    printf 'candidate_description=%s\n' "$CANDIDATE_DESCRIPTION"
    printf 'object_bytes=%s keys=%s working_set_bytes=%s\n' \
        "$OBJECT_BYTES" "$KEYS" "$((OBJECT_BYTES * KEYS))"
    printf 'warmup_requests=%s case_requests=%s seed=%s\n' \
        "$WARMUP_REQUESTS" "$CASE_REQUESTS" "$SEED"
    printf 'server_cpu=%s client_cpu=%s server_ip=%s client_ip=%s\n' \
        "$SERVER_CPU" "$CLIENT_CPU" "$SERVER_IP" "$CLIENT_IP"
    printf 'server_route=%s\n' "$(ip -4 route get "$CLIENT_IP")"
    printf 'client_route=%s\n' "$(remote_command ip -4 route get "$SERVER_IP")"
    sha256sum "$BASELINE_BIN" "$CANDIDATE_BIN" "$PATH_PREP_BIN"
    remote_command sha256sum "$CLIENT_BIN"
    /home/adam/opt/gcc-16.1/bin/g++ --version | head -1
    uname -a
    remote_command uname -a
    printf 'dopey_load=%s\n' "$(uptime)"
    printf 'rain_load=%s\n' "$(remote_command uptime)"
} > "$RUN_DIR/metadata.txt"

start_server() {
    local variant=$1
    local ordinal=$2
    local binary=$BASELINE_BIN
    [[ $variant == candidate ]] && binary=$CANDIDATE_BIN
    local server_dir=$RUN_DIR/${ordinal}-${variant}
    mkdir -p "$server_dir"

    sudo -n env LD_LIBRARY_PATH="$GCC_LIB" "$binary" \
        --ssd-dir "$POOL_DIR" \
        --memory 1G --block 2M --ram-head 256K --small-min-alloc 16 \
        --io-chunk 256K --io-buffers 64 --cores 1 \
        --numa 0 --no-numa-promotion --net async --no-mlock \
        --max-objects "$((KEYS + 1024))" \
        --memcache-port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        > "$server_dir/server.log" 2>&1 &
    SERVER_LAUNCH_PID=$!

    for _ in $(seq 1 600); do
        if ! sudo -n kill -0 "$SERVER_LAUNCH_PID" 2>/dev/null; then
            wait "$SERVER_LAUNCH_PID" || true
            tail -100 "$server_dir/server.log" || true
            die "$variant server exited during startup"
        fi
        SERVER_PID=$(pgrep -n -x goblin-store || true)
        if [[ -n $SERVER_PID ]] && ss -H -ltn "sport = :$MEMCACHE_PORT" | grep -q . &&
           ss -H -ltn "sport = :$HTTP_PORT" | grep -q .; then
            break
        fi
        sleep 0.2
    done
    [[ -n $SERVER_PID ]] || die 'server PID not found'
    sudo -n taskset -apc "$SERVER_CPU" "$SERVER_PID" > "$server_dir/affinity.txt"
    grep -E 'Cpus_allowed_list|Mems_allowed_list' "/proc/$SERVER_PID/status" >> \
        "$server_dir/affinity.txt"
    sudo -n cat "/proc/$SERVER_PID/numa_maps" > "$server_dir/numa-maps-start.txt"
}

populate() {
    local ordinal=$1
    local variant=$2
    remote_command env LD_LIBRARY_PATH="$GCC_LIB" taskset -c "$CLIENT_CPU" "$CLIENT_BIN" \
        --host "$SERVER_IP" --port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        --proto http --key channel-small- --size "$OBJECT_BYTES" --keys "$KEYS" \
        --conns 1 --populate-only > "$RUN_DIR/${ordinal}-${variant}/populate.log" 2>&1
    sudo -n cat "/proc/$SERVER_PID/numa_maps" > \
        "$RUN_DIR/${ordinal}-${variant}/numa-maps-populated.txt"
}

run_case() {
    local ordinal=$1
    local variant=$2
    local protocol=$3
    local key_prefix=channel-small-
    [[ $protocol == memcache ]] && key_prefix=/channel-small-
    local label=${ordinal}-${variant}-${protocol}
    local raw=$CLIENT_RAW_DIR/$label.csv

    printf '[%s] warmup %s (%s requests)\n' "$(date -u +%FT%TZ)" "$label" "$WARMUP_REQUESTS"
    remote_command env LD_LIBRARY_PATH="$GCC_LIB" taskset -c "$CLIENT_CPU" "$CLIENT_BIN" \
        --host "$SERVER_IP" --port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        --proto "$protocol" --key "$key_prefix" --size "$OBJECT_BYTES" --keys "$KEYS" \
        --hot-keys "$KEYS" --conns 1 --no-store --seed "$SEED" \
        --requests "$WARMUP_REQUESTS" > "$RUN_DIR/$label.warmup.txt" 2>&1

    printf '[%s] measure %s (%s serial requests)\n' "$(date -u +%FT%TZ)" "$label" "$CASE_REQUESTS"
    remote_command env LD_LIBRARY_PATH="$GCC_LIB" taskset -c "$CLIENT_CPU" "$CLIENT_BIN" \
        --host "$SERVER_IP" --port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        --proto "$protocol" --key "$key_prefix" --size "$OBJECT_BYTES" --keys "$KEYS" \
        --hot-keys "$KEYS" --conns 1 --no-store --seed "$SEED" \
        --requests "$CASE_REQUESTS" --raw "$raw" > "$RUN_DIR/$label.summary.txt" 2>&1
    remote_command sha256sum "$raw" > "$RUN_DIR/$label.csv.sha256"
}

variants=(baseline candidate candidate baseline)
for i in "${!variants[@]}"; do
    ordinal=$(printf '%02d' "$((i + 1))")
    variant=${variants[$i]}
    printf '[%s] start %s %s\n' "$(date -u +%FT%TZ)" "$ordinal" "$variant"
    start_server "$variant" "$ordinal"
    populate "$ordinal" "$variant"
    if (( i % 2 == 0 )); then
        run_case "$ordinal" "$variant" memcache
        run_case "$ordinal" "$variant" http
    else
        run_case "$ordinal" "$variant" http
        run_case "$ordinal" "$variant" memcache
    fi
    sudo -n cat "/proc/$SERVER_PID/numa_maps" > \
        "$RUN_DIR/${ordinal}-${variant}/numa-maps-end.txt"
    stop_server
done

date -u +%FT%TZ > "$RUN_DIR/completed.txt"
printf 'complete: dopey:%s\nraw: rain:%s\n' "$RUN_DIR" "$CLIENT_RAW_DIR"
