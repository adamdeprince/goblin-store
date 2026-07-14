#!/usr/bin/env bash
# Controlled normal/perverse first-object-byte benchmark for the dopey/rain R820 pair.
#
# Run this on dopey.  It starts goblin-store locally, drives one strictly serial keep-alive client
# on rain, and sends all benchmark traffic over the direct eno2 link.  Every measured case uses the
# same fixed-seed pseudorandom key sequence.  Completed cases have .done markers, so an interrupted
# run can be resumed by supplying the same RUN_DIR.
#
# Examples:
#   tmux new-session -d -s goblin-numa \
#     'bash ~/goblin-numa-bin/numa_first_byte_latency.sh'
#   RUN_DIR=~/goblin-numa-results/20260714T120000Z \
#     bash ~/goblin-numa-bin/numa_first_byte_latency.sh
#
# Override CASE_REQUESTS and WARMUP_REQUESTS for a short validation run.  The default request count
# is the number of 256 KiB payloads in one hour at 10,000,000,000 bit/s.  Strict request/response
# serialization means a case can take longer than that line-rate lower bound.

set -Eeuo pipefail

SERVER_HOST=${SERVER_HOST:-dopey}
CLIENT_HOST=${CLIENT_HOST:-rain}
SERVER_IFACE=${SERVER_IFACE:-eno2}
CLIENT_IFACE=${CLIENT_IFACE:-eno2}
MANAGEMENT_IFACE=${MANAGEMENT_IFACE:-eno1}
SERVER_IP=${SERVER_IP:-10.77.77.1}
CLIENT_IP=${CLIENT_IP:-10.77.77.2}
SERVER_CPU=${SERVER_CPU:-4}
CLIENT_CPU=${CLIENT_CPU:-4}

BIN_DIR=${BIN_DIR:-$HOME/goblin-numa-bin}
SERVER_BIN=${SERVER_BIN:-$BIN_DIR/goblin-store}
PATH_PREP_BIN=${PATH_PREP_BIN:-$BIN_DIR/goblin-store-path-prep}
BENCH_BIN=${BENCH_BIN:-$BIN_DIR/goblin-bench}
GCC_LIB=${GCC_LIB:-$HOME/opt/gcc-16.1/lib64}

MEMCACHE_PORT=${MEMCACHE_PORT:-12131}
HTTP_PORT=${HTTP_PORT:-18080}
LINK_BITS_PER_SECOND=${LINK_BITS_PER_SECOND:-10000000000}
CASE_SECONDS=${CASE_SECONDS:-3600}
OBJECT_BYTES=${OBJECT_BYTES:-262144}
KEYS=${KEYS:-16000}
WARMUP_REQUESTS=${WARMUP_REQUESTS:-200000}
SEED=${SEED:-0x243f6a8885a308d3}
CASE_REQUESTS=${CASE_REQUESTS:-$((LINK_BITS_PER_SECOND * CASE_SECONDS / (8 * OBJECT_BYTES)))}
EXPECTED_HEAD_PAGES=${EXPECTED_HEAD_PAGES:-2048}
EXPECTED_LOCAL_EXTRA_PAGES=${EXPECTED_LOCAL_EXTRA_PAGES:-64}

if [[ -n ${RUN_DIR:-} ]]; then
    RUN_ID=${RUN_ID:-$(basename "$RUN_DIR")}
else
    RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
    RUN_DIR=${RESULT_BASE:-$HOME/goblin-numa-results}/$RUN_ID
fi
POOL_DIR=${POOL_DIR:-/var/tmp/goblin-numa-latency-$RUN_ID}
CLIENT_RAW_DIR=${CLIENT_RAW_DIR:-/var/tmp/goblin-numa-latency-$RUN_ID}
CONTROLLER_TMP_DIR=${CONTROLLER_TMP_DIR:-/var/tmp/goblin-numa-controller-$RUN_ID}

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

huge_free() {
    local node=$1
    awk '/HugePages_Free/ {print $4}' \
        "/sys/devices/system/node/node${node}/meminfo"
}

stop_server() {
    if [[ -n ${SERVER_PID:-} ]] && sudo -n kill -0 "$SERVER_PID" 2>/dev/null; then
        sudo -n kill -TERM "$SERVER_PID"
        for _ in $(seq 1 150); do
            sudo -n kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.2
        done
        if sudo -n kill -0 "$SERVER_PID" 2>/dev/null; then
            printf 'server did not drain in 30 seconds; killing benchmark-owned PID %s\n' \
                "$SERVER_PID" >&2
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

mkdir -p "$RUN_DIR"
RUN_LOG=$RUN_DIR/run.log
exec > >(tee -a "$RUN_LOG") 2>&1

printf '[%s] Goblin Store NUMA first-byte benchmark %s\n' "$(date -u +%FT%TZ)" "$RUN_ID"
printf 'request formula: %s bit/s * %s s / (8 * %s bytes) = %s requests/case\n' \
    "$LINK_BITS_PER_SECOND" "$CASE_SECONDS" "$OBJECT_BYTES" "$CASE_REQUESTS"
printf 'dataset: %s keys * %s bytes = %s bytes; seed=%s; one connection, no overlap\n' \
    "$KEYS" "$OBJECT_BYTES" "$((KEYS * OBJECT_BYTES))" "$SEED"

[[ $(hostname -s) == "$SERVER_HOST" ]] || die "run this script on $SERVER_HOST"
sudo -n true || die "passwordless sudo is required on $SERVER_HOST"
for binary in "$SERVER_BIN" "$PATH_PREP_BIN"; do
    [[ -x $binary ]] || die "missing executable $binary"
done
[[ -x $BENCH_BIN ]] || die "missing executable $BENCH_BIN on the shared home directory"
command -v taskset >/dev/null || die "taskset is required on $SERVER_HOST"
command -v ethtool >/dev/null || die "ethtool is required on $SERVER_HOST"
remote_command true || die "passwordless SSH from $SERVER_HOST to $CLIENT_HOST failed"
remote_command command -v taskset >/dev/null || die "taskset is required on $CLIENT_HOST"
remote_command command -v gzip >/dev/null || die "gzip is required on $CLIENT_HOST"

if pgrep -x goblin-store >/dev/null; then
    die "a goblin-store process is already running on $SERVER_HOST"
fi
if remote_command pgrep -x goblin-bench >/dev/null 2>&1; then
    die "a goblin-bench process is already running on $CLIENT_HOST"
fi

server_route=$(ip -4 route get "$CLIENT_IP")
client_route=$(remote_command ip -4 route get "$SERVER_IP")
[[ $server_route == *"dev $SERVER_IFACE"* && $server_route == *"src $SERVER_IP"* ]] ||
    die "$CLIENT_IP is not routed from $SERVER_IP over $SERVER_IFACE"
[[ $client_route == *"dev $CLIENT_IFACE"* && $client_route == *"src $CLIENT_IP"* ]] ||
    die "$SERVER_IP is not routed from $CLIENT_IP over $CLIENT_IFACE"
ip -4 route show default | grep -q "dev $MANAGEMENT_IFACE" ||
    die "server default route is not on $MANAGEMENT_IFACE"
remote_command ip -4 route show default | grep -q "dev $MANAGEMENT_IFACE" ||
    die "client default route is not on $MANAGEMENT_IFACE"
ip -4 route show default | grep -q "dev $SERVER_IFACE" &&
    die "refusing to run with a default route on $SERVER_IFACE"
remote_command ip -4 route show default | grep -q "dev $CLIENT_IFACE" &&
    die "refusing to run with a default route on $CLIENT_IFACE"
[[ $(cat "/sys/class/net/$SERVER_IFACE/device/numa_node") == 0 ]] ||
    die "$SERVER_IFACE is not attached to NUMA node 0"
[[ $(remote_command cat "/sys/class/net/$CLIENT_IFACE/device/numa_node") == 0 ]] ||
    die "$CLIENT_IFACE is not attached to NUMA node 0"
sudo -n ethtool "$SERVER_IFACE" | grep -q 'Speed: 10000Mb/s' ||
    die "$SERVER_IFACE is not at 10 Gb/s"
sudo -n ethtool "$SERVER_IFACE" | grep -q 'Link detected: yes' ||
    die "$SERVER_IFACE has no carrier"
remote_command sudo -n ethtool "$CLIENT_IFACE" | grep -q 'Speed: 10000Mb/s' ||
    die "$CLIENT_IFACE is not at 10 Gb/s"
remote_command sudo -n ethtool "$CLIENT_IFACE" | grep -q 'Link detected: yes' ||
    die "$CLIENT_IFACE has no carrier"
ping -q -c 3 -I "$SERVER_IFACE" "$CLIENT_IP" >/dev/null || die "direct link ping failed"

for node in 0 1 2 3; do
    target=$EXPECTED_HEAD_PAGES
    [[ $node == 0 ]] && target=$((target + EXPECTED_LOCAL_EXTRA_PAGES))
    free_pages=$(huge_free "$node")
    (( free_pages >= target )) ||
        die "node $node has $free_pages free HugeTLB pages; need at least $target"
done

remote_command mkdir -p "$CLIENT_RAW_DIR"
mkdir -p "$CONTROLLER_TMP_DIR"
if [[ ! -e $POOL_DIR/.goblin-store-marker ]]; then
    if [[ -d $POOL_DIR ]] && find "$POOL_DIR" -mindepth 1 -print -quit | grep -q .; then
        die "$POOL_DIR is non-empty and is not a prepared Goblin pool"
    fi
    env LD_LIBRARY_PATH="$GCC_LIB" "$PATH_PREP_BIN" "$POOL_DIR"
fi

METADATA=$RUN_DIR/metadata.txt
if [[ ! -e $METADATA ]]; then
    {
        printf 'run_id=%s\n' "$RUN_ID"
        printf 'started_utc=%s\n' "$(date -u +%FT%TZ)"
        printf 'server=%s client=%s direct=%s->%s\n' \
            "$SERVER_HOST" "$CLIENT_HOST" "$CLIENT_IP" "$SERVER_IP"
        printf 'server_route=%s\nclient_route=%s\n' "$server_route" "$client_route"
        printf 'object_bytes=%s keys=%s working_set_bytes=%s\n' \
            "$OBJECT_BYTES" "$KEYS" "$((OBJECT_BYTES * KEYS))"
        printf 'link_bits_per_second=%s case_seconds=%s case_requests=%s warmup_requests=%s\n' \
            "$LINK_BITS_PER_SECOND" "$CASE_SECONDS" "$CASE_REQUESTS" "$WARMUP_REQUESTS"
        printf 'seed=%s server_cpu=%s client_cpu=%s\n' "$SEED" "$SERVER_CPU" "$CLIENT_CPU"
        sha256sum "$SERVER_BIN" "$PATH_PREP_BIN" "$BENCH_BIN"
        uname -a
        lscpu
        for node in /sys/devices/system/node/node[0-9]*; do
            printf '\n%s cpus=' "$(basename "$node")"
            cat "$node/cpulist"
            printf '%s distance=' "$(basename "$node")"
            cat "$node/distance"
            cat "$node/meminfo"
        done
        ip -br address show "$SERVER_IFACE"
        sudo -n ethtool "$SERVER_IFACE"
        for node in 0 1 2 3; do
            grep -E 'HugePages_(Total|Free|Surp)' \
                "/sys/devices/system/node/node${node}/meminfo"
        done
        printf '\nCLIENT\n'
        remote_command uname -a
        remote_command lscpu
        remote_command bash -c \
            'for node in /sys/devices/system/node/node[0-9]*; do printf "\\n%s cpus=" "$(basename "$node")"; cat "$node/cpulist"; printf "%s distance=" "$(basename "$node")"; cat "$node/distance"; cat "$node/meminfo"; done'
        remote_command ip -br address show "$CLIENT_IFACE"
        remote_command sudo -n ethtool "$CLIENT_IFACE"
        printf '\nSERVER ENO2 IRQ AFFINITY\n'
        for irq in $(awk "/$SERVER_IFACE/ {gsub(\":\",\"\",\$1); print \$1}" /proc/interrupts); do
            printf '%s ' "$irq"
            cat "/proc/irq/$irq/smp_affinity_list"
        done
        printf '\nCLIENT ENO2 IRQ AFFINITY\n'
        remote_command bash -c \
            'for irq in $(awk "/eno2/ {gsub(\":\",\"\",\$1); print \$1}" /proc/interrupts); do printf "%s " "$irq"; cat /proc/irq/$irq/smp_affinity_list; done'
    } > "$METADATA"
fi

start_server() {
    local placement=$1
    local case_dir=$2
    local -a perverse=()
    [[ $placement == perverse ]] && perverse=(--perverse)

    printf '[%s] starting %s server\n' "$(date -u +%FT%TZ)" "$placement"
    sudo -n env LD_LIBRARY_PATH="$GCC_LIB" "$SERVER_BIN" \
        --ssd-dir "$POOL_DIR" \
        --memory 4G --sub-memory 4G --block 2M --ram-head 256K \
        --io-chunk 256K --io-buffers 64 --cores 1 \
        --numa 0 --no-numa-promotion --net async \
        --max-objects "$((KEYS + 1024))" \
        --memcache-port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        "${perverse[@]}" > "$case_dir/server.log" 2>&1 &
    SERVER_LAUNCH_PID=$!

    for _ in $(seq 1 600); do
        if ! sudo -n kill -0 "$SERVER_LAUNCH_PID" 2>/dev/null; then
            wait "$SERVER_LAUNCH_PID" || true
            tail -100 "$case_dir/server.log" || true
            die "$placement server exited during startup"
        fi
        SERVER_PID=$(pgrep -n -x goblin-store || true)
        if [[ -n $SERVER_PID ]] && ss -H -ltn "sport = :$MEMCACHE_PORT" | grep -q . &&
           ss -H -ltn "sport = :$HTTP_PORT" | grep -q .; then
            break
        fi
        sleep 0.2
    done
    [[ -n $SERVER_PID ]] || die "could not find the $placement server PID"
    ss -H -ltn "sport = :$MEMCACHE_PORT" | grep -q . || die "memcache listener did not start"
    ss -H -ltn "sport = :$HTTP_PORT" | grep -q . || die "HTTP listener did not start"

    sudo -n taskset -apc "$SERVER_CPU" "$SERVER_PID" > "$case_dir/server-affinity.txt"
    {
        printf 'pid=%s\n' "$SERVER_PID"
        grep -E 'Cpus_allowed_list|Mems_allowed_list' "/proc/$SERVER_PID/status"
        for task in /proc/"$SERVER_PID"/task/*; do
            printf '%s ' "$(basename "$task")"
            grep 'Cpus_allowed_list' "$task/status"
        done
    } >> "$case_dir/server-affinity.txt"

    # The four 4 GiB head arenas consume 2,048 pages/node.  Node 0 may retain only the explicit
    # streaming-pool reserve.  A non-local free page here would mean a head arena fell back.
    for node in 1 2 3; do
        [[ $(huge_free "$node") == 0 ]] ||
            die "node $node retained HugeTLB pages after startup; a head arena may have fallen back"
    done
    (( $(huge_free 0) <= EXPECTED_LOCAL_EXTRA_PAGES )) ||
        die "node 0 retained too many HugeTLB pages; its head arena may have fallen back"
}

run_protocol_case() {
    local ordinal=$1
    local placement=$2
    local protocol=$3
    local label=${ordinal}-${placement}-${protocol}
    local case_dir=$RUN_DIR/$label
    local raw_path=$CLIENT_RAW_DIR/$label.csv
    local summary_path=$CONTROLLER_TMP_DIR/$label.summary.txt
    local warmup_path=$CONTROLLER_TMP_DIR/$label.warmup.log
    local key_prefix

    if [[ -e $case_dir/.done || -e $case_dir/.measured ]]; then
        printf '[%s] skipping already measured %s\n' "$(date -u +%FT%TZ)" "$label"
        return
    fi
    mkdir -p "$case_dir"
    sudo -n ethtool -S "$SERVER_IFACE" > "$case_dir/server-nic-before.txt"
    remote_command sudo -n ethtool -S "$CLIENT_IFACE" > "$case_dir/client-nic-before.txt"

    if [[ $protocol == memcache ]]; then
        key_prefix=/numa-latency-
    else
        key_prefix=numa-latency-
    fi

    printf '[%s] warming %s with %s deterministic serial reads\n' \
        "$(date -u +%FT%TZ)" "$label" "$WARMUP_REQUESTS"
    remote_command env LD_LIBRARY_PATH="$GCC_LIB" taskset -c "$CLIENT_CPU" "$BENCH_BIN" \
        --host "$SERVER_IP" --port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        --proto "$protocol" --key "$key_prefix" --size "$OBJECT_BYTES" --keys "$KEYS" \
        --hot-keys "$KEYS" --conns 1 --no-store --seed "$SEED" \
        --requests "$WARMUP_REQUESTS" > "$warmup_path" 2>&1

    printf '[%s] measuring %s: %s requests, one at a time\n' \
        "$(date -u +%FT%TZ)" "$label" "$CASE_REQUESTS"
    remote_command env LD_LIBRARY_PATH="$GCC_LIB" taskset -c "$CLIENT_CPU" "$BENCH_BIN" \
        --host "$SERVER_IP" --port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        --proto "$protocol" --key "$key_prefix" --size "$OBJECT_BYTES" --keys "$KEYS" \
        --hot-keys "$KEYS" --conns 1 --no-store --seed "$SEED" \
        --requests "$CASE_REQUESTS" --raw "$raw_path" > "$summary_path" 2>&1

    local sequence_hash
    sequence_hash=$(awk '/key sequence/ {print $4}' "$summary_path")
    [[ -n $sequence_hash ]] || die "$label did not report a key-sequence hash"
    if [[ -e $RUN_DIR/key-sequence.hash ]]; then
        [[ $sequence_hash == "$(cat "$RUN_DIR/key-sequence.hash")" ]] ||
            die "$label used a different key sequence ($sequence_hash)"
    else
        printf '%s\n' "$sequence_hash" > "$RUN_DIR/key-sequence.hash"
    fi

    sudo -n ethtool -S "$SERVER_IFACE" > "$case_dir/server-nic-after.txt"
    remote_command sudo -n ethtool -S "$CLIENT_IFACE" > "$case_dir/client-nic-after.txt"
    sudo -n cat "/proc/$SERVER_PID/numa_maps" > \
        "$case_dir/server-numa-maps-after-measurement.txt"
    date -u +%FT%TZ > "$case_dir/.measured"
    printf '[%s] measured %s; raw samples remain on rain:/var/tmp\n' \
        "$(date -u +%FT%TZ)" "$label"
}

finalize_case() {
    local ordinal=$1
    local placement=$2
    local protocol=$3
    local label=${ordinal}-${placement}-${protocol}
    local case_dir=$RUN_DIR/$label
    local raw_path=$CLIENT_RAW_DIR/$label.csv
    local summary_path=$CONTROLLER_TMP_DIR/$label.summary.txt
    local warmup_path=$CONTROLLER_TMP_DIR/$label.warmup.log

    [[ -e $case_dir/.done ]] && return
    [[ -e $case_dir/.measured ]] || die "$label has not completed measurement"

    printf '[%s] compressing raw samples for %s after measurement\n' \
        "$(date -u +%FT%TZ)" "$label"
    cp "$summary_path" "$case_dir/summary.txt"
    cp "$warmup_path" "$case_dir/warmup.log"
    remote_command gzip -1 -f "$raw_path"
    remote_command cp "$raw_path.gz" "$case_dir/samples.csv.gz"
    remote_command rm -f "$raw_path.gz"
    sha256sum "$case_dir/samples.csv.gz" > "$case_dir/samples.csv.gz.sha256"
    date -u +%FT%TZ > "$case_dir/.done"
    rm -f "$case_dir/.measured"
    printf '[%s] completed %s\n' "$(date -u +%FT%TZ)" "$label"
}

run_placement() {
    local placement=$1
    local memcache_ordinal=$2
    local http_ordinal=$3
    local placement_dir=$RUN_DIR/$placement
    local memcache_dir=$RUN_DIR/${memcache_ordinal}-${placement}-memcache
    local http_dir=$RUN_DIR/${http_ordinal}-${placement}-http
    local need_server=false

    if [[ ! -e $memcache_dir/.done && ! -e $memcache_dir/.measured ]] ||
       [[ ! -e $http_dir/.done && ! -e $http_dir/.measured ]]; then
        need_server=true
    fi

    if [[ $need_server == true ]]; then
        mkdir -p "$placement_dir"
        start_server "$placement" "$placement_dir"

        printf '[%s] populating %s objects (%s bytes) once for %s\n' \
            "$(date -u +%FT%TZ)" "$KEYS" "$((KEYS * OBJECT_BYTES))" "$placement"
        remote_command env LD_LIBRARY_PATH="$GCC_LIB" taskset -c "$CLIENT_CPU" "$BENCH_BIN" \
            --host "$SERVER_IP" --port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
            --proto http --key numa-latency- --size "$OBJECT_BYTES" --keys "$KEYS" \
            --conns 1 --populate-only > "$CONTROLLER_TMP_DIR/$placement-populate.log" 2>&1
        sudo -n cat "/proc/$SERVER_PID/numa_maps" > \
            "$placement_dir/server-numa-maps-after-populate.txt"

        run_protocol_case "$memcache_ordinal" "$placement" memcache
        run_protocol_case "$http_ordinal" "$placement" http
        stop_server
        cp "$CONTROLLER_TMP_DIR/$placement-populate.log" "$placement_dir/populate.log"
    fi

    finalize_case "$memcache_ordinal" "$placement" memcache
    finalize_case "$http_ordinal" "$placement" http
}

# Placement is the outer loop: one normal server, one switch, then one perverse server.
run_placement normal 01 02
run_placement perverse 03 04

printf '[%s] all cases complete: %s\n' "$(date -u +%FT%TZ)" "$RUN_DIR"
