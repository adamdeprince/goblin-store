#!/usr/bin/env bash
# NUMA interconnect-load benchmark for the directly connected dopey/rain R820 pair.
#
# Run on dopey.  Both server variants keep their workers and NIC on NUMA node 0 and allocate
# 4 GiB on each of the four NUMA nodes.  The normal variant stores a nominal 4 GiB working set in
# the preferred node with promotion enabled.  The intentionally adverse ("perverse" in result
# labels only) variant stores a nominal 16 GiB working set across all four arenas and disables
# promotion with the existing --no-numa-promotion option.  It never passes --perverse: changing
# CPU/PCI locality would confound the DRAM-interconnect measurement.
#
# Each measured protocol reads exactly 512 GiB of 256 KiB objects.  Intel PCM's NUMA events are
# sampled only around the measured client, and eno2 byte counters supply the network denominator.
# Active logs live under /var/tmp; small completed artifacts are copied to the shared home only
# after each measurement so NFS writes cannot contaminate the measured window.

set -Eeuo pipefail

SERVER_HOST=${SERVER_HOST:-dopey}
CLIENT_HOST=${CLIENT_HOST:-rain}
SERVER_IFACE=${SERVER_IFACE:-eno2}
CLIENT_IFACE=${CLIENT_IFACE:-eno2}
MANAGEMENT_IFACE=${MANAGEMENT_IFACE:-eno1}
SERVER_IP=${SERVER_IP:-10.77.77.1}
CLIENT_IP=${CLIENT_IP:-10.77.77.2}

BIN_DIR=${BIN_DIR:-$HOME/goblin-numa-bin}
SERVER_BIN=${SERVER_BIN:-$BIN_DIR/goblin-store}
PATH_PREP_BIN=${PATH_PREP_BIN:-$BIN_DIR/goblin-store-path-prep}
BENCH_BIN=${BENCH_BIN:-$BIN_DIR/goblin-bench}
GCC_LIB=${GCC_LIB:-$HOME/opt/gcc-16.1/lib64}
PCM_NUMA=${PCM_NUMA:-/usr/sbin/pcm-numa}

MEMCACHE_PORT=${MEMCACHE_PORT:-12132}
HTTP_PORT=${HTTP_PORT:-18081}
OBJECT_BYTES=${OBJECT_BYTES:-262144}
NORMAL_KEYS=${NORMAL_KEYS:-16000}
PERVERSE_KEYS=${PERVERSE_KEYS:-64000}
CASE_READ_BYTES=${CASE_READ_BYTES:-549755813888} # 512 GiB
CASE_REQUESTS=${CASE_REQUESTS:-$((CASE_READ_BYTES / OBJECT_BYTES))}
WARMUP_REQUESTS=${WARMUP_REQUESTS:-65536}
SEED=${SEED:-0x243f6a8885a308d3}
SERVER_CORES=${SERVER_CORES:-4}
CLIENT_CONNS=${CLIENT_CONNS:-8}
# Physical cores on rain's NIC-local node 0.  eno2's IRQs use the upper node-0 cores.
CLIENT_CPUS=${CLIENT_CPUS:-0,4,8,12,16,20,24,28,32,36,40,44}
PCM_INTERVAL=${PCM_INTERVAL:-1}
NIC_QUIET_POLLS=${NIC_QUIET_POLLS:-3}
NIC_QUIET_TIMEOUT=${NIC_QUIET_TIMEOUT:-20}
NIC_QUIET_BYTES=${NIC_QUIET_BYTES:-65536}
EXPECTED_HEAD_PAGES=${EXPECTED_HEAD_PAGES:-2048}
EXPECTED_LOCAL_EXTRA_PAGES=${EXPECTED_LOCAL_EXTRA_PAGES:-256}
PREPARE_MEMORY=${PREPARE_MEMORY:-1}

(( OBJECT_BYTES > 0 )) || { printf 'ERROR: OBJECT_BYTES must be positive\n' >&2; exit 1; }
(( CASE_READ_BYTES % OBJECT_BYTES == 0 )) || {
    printf 'ERROR: CASE_READ_BYTES must be exactly divisible by OBJECT_BYTES\n' >&2
    exit 1
}

if [[ -n ${RUN_DIR:-} ]]; then
    RUN_ID=${RUN_ID:-$(basename "$RUN_DIR")}
else
    RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-numa-bandwidth}
    RUN_DIR=${RESULT_BASE:-$HOME/goblin-numa-results}/$RUN_ID
fi
POOL_DIR=${POOL_DIR:-/var/tmp/goblin-numa-bandwidth-$RUN_ID}
ACTIVE_DIR=${ACTIVE_DIR:-/var/tmp/goblin-numa-bandwidth-active-$RUN_ID}
CLIENT_TMP_DIR=${CLIENT_TMP_DIR:-/var/tmp/goblin-numa-bandwidth-client-$RUN_ID}

SERVER_PID=
SERVER_LAUNCH_PID=
PCM_PID=
PCM_LAUNCH_PID=

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

stop_pcm() {
    if [[ -n ${PCM_PID:-} ]] && sudo -n kill -0 "$PCM_PID" 2>/dev/null; then
        sudo -n kill -INT "$PCM_PID" 2>/dev/null || true
        for _ in $(seq 1 100); do
            sudo -n kill -0 "$PCM_PID" 2>/dev/null || break
            sleep 0.1
        done
        if sudo -n kill -0 "$PCM_PID" 2>/dev/null; then
            sudo -n kill -TERM "$PCM_PID" 2>/dev/null || true
        fi
    fi
    if [[ -n ${PCM_LAUNCH_PID:-} ]]; then
        wait "$PCM_LAUNCH_PID" 2>/dev/null || true
    fi
    PCM_PID=
    PCM_LAUNCH_PID=
}

stop_server() {
    stop_pcm
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

mkdir -p "$RUN_DIR" "$ACTIVE_DIR"
RUN_LOG=$ACTIVE_DIR/run.log
exec > >(tee -a "$RUN_LOG") 2>&1

printf '[%s] Goblin Store NUMA interconnect benchmark %s\n' "$(date -u +%FT%TZ)" "$RUN_ID"
printf 'traffic: %s requests * %s bytes = %s bytes (%.0f GiB) per protocol case\n' \
    "$CASE_REQUESTS" "$OBJECT_BYTES" "$((CASE_REQUESTS * OBJECT_BYTES))" \
    "$(awk -v b="$((CASE_REQUESTS * OBJECT_BYTES))" 'BEGIN {print b / 1073741824}')"
printf 'normal dataset: %s objects = %.5f GiB; promotion enabled\n' \
    "$NORMAL_KEYS" "$(awk -v n="$NORMAL_KEYS" -v b="$OBJECT_BYTES" 'BEGIN {print n*b/1073741824}')"
printf 'perverse-label dataset: %s objects = %.5f GiB; --no-numa-promotion; no --perverse\n' \
    "$PERVERSE_KEYS" "$(awk -v n="$PERVERSE_KEYS" -v b="$OBJECT_BYTES" 'BEGIN {print n*b/1073741824}')"

[[ $(hostname -s) == "$SERVER_HOST" ]] || die "run this script on $SERVER_HOST"
sudo -n true || die "passwordless sudo is required on $SERVER_HOST"
for binary in "$SERVER_BIN" "$PATH_PREP_BIN" "$BENCH_BIN" "$PCM_NUMA"; do
    [[ -x $binary ]] || die "missing executable $binary"
done
for command in taskset ethtool awk sha256sum; do
    command -v "$command" >/dev/null || die "$command is required on $SERVER_HOST"
done
remote_command true || die "passwordless SSH from $SERVER_HOST to $CLIENT_HOST failed"
remote_command command -v taskset >/dev/null || die "taskset is required on $CLIENT_HOST"
remote_command mkdir -p "$CLIENT_TMP_DIR"

if pgrep -x goblin-store >/dev/null; then
    die "a goblin-store process is already running on $SERVER_HOST"
fi
if pgrep -x pcm-numa >/dev/null || pgrep -x pcm-memory >/dev/null || pgrep -x pcm >/dev/null; then
    die "an Intel PCM process is already running on $SERVER_HOST"
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
ping -q -c 3 -I "$SERVER_IFACE" "$CLIENT_IP" >/dev/null || die "direct-link ping failed"

for node in 0 1 2 3; do
    target=$EXPECTED_HEAD_PAGES
    [[ $node == 0 ]] && target=$((target + EXPECTED_LOCAL_EXTRA_PAGES))
    free_pages=$(huge_free "$node")
    (( free_pages >= target )) ||
        die "node $node has $free_pages free HugeTLB pages; need at least $target"
done

if (( PREPARE_MEMORY )); then
    printf '[%s] syncing, dropping the page cache, and compacting ordinary memory\n' \
        "$(date -u +%FT%TZ)"
    sudo -n sync
    sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    sudo -n sh -c 'echo 1 > /proc/sys/vm/compact_memory'
fi
sudo -n modprobe msr

if [[ ! -e $POOL_DIR/.goblin-store-marker ]]; then
    if [[ -d $POOL_DIR ]] && find "$POOL_DIR" -mindepth 1 -print -quit | grep -q .; then
        die "$POOL_DIR is non-empty and is not a prepared Goblin pool"
    fi
    env LD_LIBRARY_PATH="$GCC_LIB" "$PATH_PREP_BIN" "$POOL_DIR"
fi

METADATA_TMP=$ACTIVE_DIR/metadata.txt
if [[ ! -e $RUN_DIR/metadata.txt ]]; then
    {
        printf 'run_id=%s\nstarted_utc=%s\n' "$RUN_ID" "$(date -u +%FT%TZ)"
        printf 'server=%s client=%s direct=%s:%s<->%s:%s\n' \
            "$SERVER_HOST" "$CLIENT_HOST" "$SERVER_IFACE" "$SERVER_IP" \
            "$CLIENT_IFACE" "$CLIENT_IP"
        printf 'server_route=%s\nclient_route=%s\n' "$server_route" "$client_route"
        printf 'object_bytes=%s case_read_bytes=%s case_requests=%s warmup_requests=%s\n' \
            "$OBJECT_BYTES" "$CASE_READ_BYTES" "$CASE_REQUESTS" "$WARMUP_REQUESTS"
        printf 'normal_keys=%s normal_bytes=%s normal_promotion=enabled\n' \
            "$NORMAL_KEYS" "$((NORMAL_KEYS * OBJECT_BYTES))"
        printf 'perverse_label_keys=%s perverse_label_bytes=%s perverse_label_flag=--no-numa-promotion\n' \
            "$PERVERSE_KEYS" "$((PERVERSE_KEYS * OBJECT_BYTES))"
        printf 'server_perverse_flag_used=false\n'
        printf 'memory_per_node=4G allocation_block=2M ram_head=256K numa_node=0\n'
        printf 'server_cores=%s client_conns=%s client_cpus=%s seed=%s pcm_interval=%s\n' \
            "$SERVER_CORES" "$CLIENT_CONNS" "$CLIENT_CPUS" "$SEED" "$PCM_INTERVAL"
        printf 'remote_byte_estimator=pcm-numa_remote_dram_accesses_times_64_bytes\n'
        printf 'pcm_scope=all_dopey_cpus; first_baseline_interval_excluded; final_bracketing_interval_included\n'
        sha256sum "$SERVER_BIN" "$PATH_PREP_BIN" "$BENCH_BIN" "$0"
        uname -a
        lscpu
        for node in /sys/devices/system/node/node[0-9]*; do
            printf '\n%s cpus=' "$(basename "$node")"
            cat "$node/cpulist"
            printf '%s distance=' "$(basename "$node")"
            cat "$node/distance"
            grep -E 'MemTotal|MemFree|HugePages_(Total|Free|Surp)' "$node/meminfo"
        done
        ip -br address show "$SERVER_IFACE"
        sudo -n ethtool "$SERVER_IFACE"
        printf '\nCLIENT\n'
        remote_command uname -a
        remote_command lscpu
        remote_command ip -br address show "$CLIENT_IFACE"
        remote_command sudo -n ethtool "$CLIENT_IFACE"
    } > "$METADATA_TMP"
    cp "$METADATA_TMP" "$RUN_DIR/metadata.txt"
fi

RESULTS_TMP=$ACTIVE_DIR/results.tsv
if [[ ! -e $RESULTS_TMP ]]; then
    printf 'case\tplacement\tprotocol\tobjects\tdataset_bytes\trequests\tpayload_bytes\telapsed_seconds\tserver_tx_bytes\tclient_rx_bytes\tpcm_intervals\tpcm_rows\tlocal_dram_accesses\tremote_dram_accesses\testimated_remote_data_bytes\tremote_access_percent\testimated_remote_GiB_per_second\testimated_remote_MiB_per_GiB_payload\testimated_remote_MiB_per_GiB_server_tx\n' \
        > "$RESULTS_TMP"
fi

capture_local_nic() {
    local output=$1
    : > "$output"
    for stat in rx_bytes tx_bytes rx_packets tx_packets; do
        printf '%s=%s\n' "$stat" "$(cat "/sys/class/net/$SERVER_IFACE/statistics/$stat")" \
            >> "$output"
    done
}

capture_client_nic() {
    local output=$1
    remote_command bash -c \
        'iface=$1; for stat in rx_bytes tx_bytes rx_packets tx_packets; do printf "%s=%s\\n" "$stat" "$(cat "/sys/class/net/$iface/statistics/$stat")"; done' \
        _ "$CLIENT_IFACE" > "$output"
}

stat_value() {
    local file=$1 key=$2
    awk -F= -v key="$key" '$1 == key {print $2}' "$file"
}

wait_nic_counters_quiet() {
    local previous_server=-1 previous_client=-1 stable=0
    local current_server current_client server_delta client_delta
    for _ in $(seq 1 "$NIC_QUIET_TIMEOUT"); do
        current_server=$(cat "/sys/class/net/$SERVER_IFACE/statistics/tx_bytes")
        current_client=$(remote_command cat "/sys/class/net/$CLIENT_IFACE/statistics/rx_bytes")
        if (( previous_server >= 0 )); then
            server_delta=$((current_server - previous_server))
            client_delta=$((current_client - previous_client))
            if (( server_delta >= 0 && server_delta <= NIC_QUIET_BYTES &&
                  client_delta >= 0 && client_delta <= NIC_QUIET_BYTES )); then
                ((++stable))
                (( stable >= NIC_QUIET_POLLS )) && return
            else
                stable=0
            fi
        fi
        previous_server=$current_server
        previous_client=$current_client
        sleep 1
    done
    die "$SERVER_IFACE TX / $CLIENT_IFACE RX counters did not quiesce"
}

start_server() {
    local placement=$1 keys=$2 placement_tmp=$3
    local -a promotion_args=()
    [[ $placement == perverse ]] && promotion_args=(--no-numa-promotion)

    printf '[%s] starting %s server (%s objects; %s)\n' \
        "$(date -u +%FT%TZ)" "$placement" "$keys" \
        "$([[ $placement == normal ]] && printf 'promotion enabled' || printf 'promotion disabled')"
    sudo -n env LD_LIBRARY_PATH="$GCC_LIB" "$SERVER_BIN" \
        --ssd-dir "$POOL_DIR" \
        --memory 4G --sub-memory 4G --block 2M --ram-head 256K \
        --io-chunk 256K --io-buffers 64 --cores "$SERVER_CORES" \
        --numa 0 --net async \
        --max-objects "$((keys + 1024))" \
        --memcache-port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        "${promotion_args[@]}" > "$placement_tmp/server.log" 2>&1 &
    SERVER_LAUNCH_PID=$!

    for _ in $(seq 1 900); do
        if ! sudo -n kill -0 "$SERVER_LAUNCH_PID" 2>/dev/null; then
            wait "$SERVER_LAUNCH_PID" || true
            tail -100 "$placement_tmp/server.log" || true
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

    {
        printf 'pid=%s\n' "$SERVER_PID"
        grep -E 'Cpus_allowed_list|Mems_allowed_list' "/proc/$SERVER_PID/status"
        for task in /proc/"$SERVER_PID"/task/*; do
            printf '%s ' "$(basename "$task")"
            grep 'Cpus_allowed_list' "$task/status"
        done
    } > "$placement_tmp/server-affinity.txt"
    for node in 0 1 2 3; do
        printf 'node%s_hugepages_free=%s\n' "$node" "$(huge_free "$node")"
    done > "$placement_tmp/hugepages-after-start.txt"

    # Every 4 GiB head arena should have consumed its node's 2,048-page reserve.  Node 0 retains
    # only the separately reserved streaming-pool allowance.
    for node in 1 2 3; do
        [[ $(huge_free "$node") == 0 ]] ||
            die "node $node retained HugeTLB pages after startup; its head arena may have fallen back"
    done
    (( $(huge_free 0) <= EXPECTED_LOCAL_EXTRA_PAGES )) ||
        die "node 0 retained too many HugeTLB pages; its head arena may have fallen back"
}

start_pcm() {
    local csv=$1 log=$2
    [[ -z $(pgrep -x pcm-numa || true) ]] || die "pcm-numa was already running"
    sudo -n "$PCM_NUMA" -csv="$csv" "$PCM_INTERVAL" > "$log" 2>&1 &
    PCM_LAUNCH_PID=$!
    for _ in $(seq 1 200); do
        PCM_PID=$(pgrep -n -x pcm-numa || true)
        [[ -n $PCM_PID ]] && break
        if ! kill -0 "$PCM_LAUNCH_PID" 2>/dev/null; then
            wait "$PCM_LAUNCH_PID" || true
            tail -100 "$log" || true
            die "pcm-numa exited during startup"
        fi
        sleep 0.05
    done
    [[ -n $PCM_PID ]] || die "could not find pcm-numa PID"

    # Wait for one complete idle baseline interval.  The parser excludes it, which makes the first
    # included interval begin immediately before the NIC counters and client measurement.
    for _ in $(seq 1 200); do
        [[ -e $csv ]] && (( $(grep -c '^Time elapsed:' "$csv" || true) >= 1 )) && return
        sleep 0.05
    done
    die "pcm-numa produced no sample interval"
}

parse_pcm_window() {
    local csv=$1 skip=$2 last_interval=$3 output=$4
    awk -F, -v skip="$skip" -v last_interval="$last_interval" '
        /^Time elapsed:/ {++interval; next}
        interval > skip && interval <= last_interval && $1 ~ /^[0-9]+$/ && NF >= 6 {
            local_access += $5
            remote_access += $6
            ++rows
        }
        END {
            printf "intervals=%d\nrows=%d\nlocal_dram_accesses=%.0f\nremote_dram_accesses=%.0f\n", \
                last_interval-skip, rows, local_access, remote_access
        }
    ' "$csv" > "$output"
}

run_protocol_case() {
    local ordinal=$1 placement=$2 protocol=$3 keys=$4 placement_tmp=$5
    local label=${ordinal}-${placement}-${protocol}
    local case_dir=$RUN_DIR/$label
    local case_tmp=$ACTIVE_DIR/$label
    local key_prefix

    if [[ -e $case_dir/.done ]]; then
        printf '[%s] skipping completed %s\n' "$(date -u +%FT%TZ)" "$label"
        return
    fi
    mkdir -p "$case_tmp" "$case_dir"
    if [[ $protocol == memcache ]]; then
        key_prefix=/numa-bandwidth-
    else
        key_prefix=numa-bandwidth-
    fi

    printf '[%s] warming %s with %s deterministic reads over %s connections\n' \
        "$(date -u +%FT%TZ)" "$label" "$WARMUP_REQUESTS" "$CLIENT_CONNS"
    remote_command env LD_LIBRARY_PATH="$GCC_LIB" taskset -c "$CLIENT_CPUS" "$BENCH_BIN" \
        --host "$SERVER_IP" --port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        --proto "$protocol" --key "$key_prefix" --size "$OBJECT_BYTES" --keys "$keys" \
        --hot-keys "$keys" --conns "$CLIENT_CONNS" --no-store --seed "$SEED" \
        --requests "$WARMUP_REQUESTS" > "$case_tmp/warmup.log" 2>&1

    # The ixgbe/driver counters can lag completed traffic.  Wait until the warmup is fully reflected
    # before taking the baseline; otherwise the warmup's tail leaks into the measured byte delta.
    wait_nic_counters_quiet
    local pcm_csv=$case_tmp/pcm-numa.csv
    start_pcm "$pcm_csv" "$case_tmp/pcm-numa.log"
    local pcm_skip
    pcm_skip=$(grep -c '^Time elapsed:' "$pcm_csv")
    capture_local_nic "$case_tmp/server-nic-before.txt"
    capture_client_nic "$case_tmp/client-nic-before.txt"
    local start_ns end_ns
    start_ns=$(date +%s%N)
    printf '[%s] measuring %s: %s requests / %.0f GiB\n' \
        "$(date -u +%FT%TZ)" "$label" "$CASE_REQUESTS" \
        "$(awk -v b="$((CASE_REQUESTS * OBJECT_BYTES))" 'BEGIN {print b/1073741824}')"
    if ! remote_command env LD_LIBRARY_PATH="$GCC_LIB" taskset -c "$CLIENT_CPUS" "$BENCH_BIN" \
        --host "$SERVER_IP" --port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        --proto "$protocol" --key "$key_prefix" --size "$OBJECT_BYTES" --keys "$keys" \
        --hot-keys "$keys" --conns "$CLIENT_CONNS" --no-store --seed "$SEED" \
        --requests "$CASE_REQUESTS" > "$case_tmp/client-summary.txt" 2>&1; then
        stop_pcm
        tail -100 "$case_tmp/client-summary.txt" || true
        die "$label client failed"
    fi
    end_ns=$(date +%s%N)

    # Let the PCM interval containing the final response close, then stop.  This brackets the full
    # request window with at most one second of quiet tail rather than dropping a partial interval.
    local pcm_at_end pcm_include
    pcm_at_end=$(grep -c '^Time elapsed:' "$pcm_csv")
    for _ in $(seq 1 200); do
        pcm_include=$(grep -c '^Time elapsed:' "$pcm_csv")
        (( pcm_include > pcm_at_end )) && break
        sleep 0.05
    done
    (( pcm_include > pcm_at_end )) || die "pcm-numa did not close its final interval"
    sleep 0.2
    stop_pcm

    # Likewise, wait for the final measured packets to reach the driver's exported counters.  This
    # quiet wait is outside both the client elapsed time and the parsed PCM interval window.
    wait_nic_counters_quiet
    capture_local_nic "$case_tmp/server-nic-after.txt"
    capture_client_nic "$case_tmp/client-nic-after.txt"

    parse_pcm_window "$pcm_csv" "$pcm_skip" "$pcm_include" "$case_tmp/pcm-window.txt"
    sudo -n cat "/proc/$SERVER_PID/numa_maps" > "$case_tmp/server-numa-maps.txt"

    local sequence_hash expected_hash_file
    sequence_hash=$(awk '/key sequence/ {print $4}' "$case_tmp/client-summary.txt")
    [[ -n $sequence_hash ]] || die "$label did not report a key-sequence hash"
    expected_hash_file=$ACTIVE_DIR/$placement-key-sequence.hash
    if [[ -e $expected_hash_file ]]; then
        [[ $sequence_hash == "$(cat "$expected_hash_file")" ]] ||
            die "$label used a different key sequence ($sequence_hash)"
    else
        printf '%s\n' "$sequence_hash" > "$expected_hash_file"
    fi

    local server_tx_before server_tx_after client_rx_before client_rx_after
    local server_tx_delta client_rx_delta elapsed_ns payload_bytes
    server_tx_before=$(stat_value "$case_tmp/server-nic-before.txt" tx_bytes)
    server_tx_after=$(stat_value "$case_tmp/server-nic-after.txt" tx_bytes)
    client_rx_before=$(stat_value "$case_tmp/client-nic-before.txt" rx_bytes)
    client_rx_after=$(stat_value "$case_tmp/client-nic-after.txt" rx_bytes)
    server_tx_delta=$((server_tx_after - server_tx_before))
    client_rx_delta=$((client_rx_after - client_rx_before))
    elapsed_ns=$((end_ns - start_ns))
    payload_bytes=$((CASE_REQUESTS * OBJECT_BYTES))

    local pcm_intervals pcm_rows local_access remote_access estimated_remote_bytes
    pcm_intervals=$(stat_value "$case_tmp/pcm-window.txt" intervals)
    pcm_rows=$(stat_value "$case_tmp/pcm-window.txt" rows)
    local_access=$(stat_value "$case_tmp/pcm-window.txt" local_dram_accesses)
    remote_access=$(stat_value "$case_tmp/pcm-window.txt" remote_dram_accesses)
    estimated_remote_bytes=$((remote_access * 64))
    (( pcm_rows > 0 )) || die "$label PCM window contained no core samples"

    local elapsed_seconds remote_percent remote_gib_s remote_mib_per_payload_gib
    local remote_mib_per_tx_gib
    elapsed_seconds=$(awk -v n="$elapsed_ns" 'BEGIN {printf "%.9f", n/1000000000}')
    remote_percent=$(awk -v l="$local_access" -v r="$remote_access" \
        'BEGIN {d=l+r; printf "%.6f", d ? 100*r/d : 0}')
    remote_gib_s=$(awk -v b="$estimated_remote_bytes" -v n="$elapsed_ns" \
        'BEGIN {printf "%.6f", n ? b/1073741824/(n/1000000000) : 0}')
    remote_mib_per_payload_gib=$(awk -v b="$estimated_remote_bytes" -v payload="$payload_bytes" \
        'BEGIN {printf "%.6f", payload ? (b/1048576)/(payload/1073741824) : 0}')
    remote_mib_per_tx_gib=$(awk -v b="$estimated_remote_bytes" -v tx="$server_tx_delta" \
        'BEGIN {printf "%.6f", tx ? (b/1048576)/(tx/1073741824) : 0}')

    {
        printf 'case=%s\nplacement=%s\nprotocol=%s\nobjects=%s\ndataset_bytes=%s\n' \
            "$label" "$placement" "$protocol" "$keys" "$((keys * OBJECT_BYTES))"
        printf 'requests=%s\npayload_bytes=%s\nstart_monotonic_wall_ns=%s\nend_monotonic_wall_ns=%s\n' \
            "$CASE_REQUESTS" "$payload_bytes" "$start_ns" "$end_ns"
        printf 'elapsed_seconds=%s\nserver_tx_bytes=%s\nclient_rx_bytes=%s\n' \
            "$elapsed_seconds" "$server_tx_delta" "$client_rx_delta"
        printf 'pcm_skip_intervals=%s\npcm_include_through_interval=%s\n' \
            "$pcm_skip" "$pcm_include"
        cat "$case_tmp/pcm-window.txt"
        printf 'estimated_remote_data_bytes=%s\nremote_access_percent=%s\n' \
            "$estimated_remote_bytes" "$remote_percent"
        printf 'estimated_remote_GiB_per_second=%s\n' "$remote_gib_s"
        printf 'estimated_remote_MiB_per_GiB_payload=%s\n' "$remote_mib_per_payload_gib"
        printf 'estimated_remote_MiB_per_GiB_server_tx=%s\n' "$remote_mib_per_tx_gib"
        printf 'key_sequence_hash=%s\n' "$sequence_hash"
    } > "$case_tmp/measurement.txt"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$placement" "$protocol" "$keys" "$((keys * OBJECT_BYTES))" \
        "$CASE_REQUESTS" "$payload_bytes" "$elapsed_seconds" "$server_tx_delta" \
        "$client_rx_delta" "$pcm_intervals" "$pcm_rows" "$local_access" "$remote_access" \
        "$estimated_remote_bytes" "$remote_percent" "$remote_gib_s" \
        "$remote_mib_per_payload_gib" "$remote_mib_per_tx_gib" >> "$RESULTS_TMP"

    cp -a "$case_tmp/." "$case_dir/"
    date -u +%FT%TZ > "$case_dir/.done"
    cp "$RESULTS_TMP" "$RUN_DIR/results.tsv"
    printf '[%s] completed %s: remote %.3f%%, estimated %.3f MiB/GiB server TX\n' \
        "$(date -u +%FT%TZ)" "$label" "$remote_percent" "$remote_mib_per_tx_gib"
}

run_placement() {
    local placement=$1 keys=$2 memcache_ordinal=$3 http_ordinal=$4
    local placement_dir=$RUN_DIR/$placement
    local placement_tmp=$ACTIVE_DIR/$placement
    local memcache_dir=$RUN_DIR/${memcache_ordinal}-${placement}-memcache
    local http_dir=$RUN_DIR/${http_ordinal}-${placement}-http
    local need_server=false
    [[ ! -e $memcache_dir/.done || ! -e $http_dir/.done ]] && need_server=true
    [[ $need_server == true ]] || return

    mkdir -p "$placement_dir" "$placement_tmp"
    start_server "$placement" "$keys" "$placement_tmp"
    printf '[%s] populating %s objects (%s bytes) once for %s\n' \
        "$(date -u +%FT%TZ)" "$keys" "$((keys * OBJECT_BYTES))" "$placement"
    remote_command env LD_LIBRARY_PATH="$GCC_LIB" taskset -c "$CLIENT_CPUS" "$BENCH_BIN" \
        --host "$SERVER_IP" --port "$MEMCACHE_PORT" --http-port "$HTTP_PORT" \
        --proto http --key numa-bandwidth- --size "$OBJECT_BYTES" --keys "$keys" \
        --conns 1 --populate-only > "$placement_tmp/populate.log" 2>&1
    sudo -n cat "/proc/$SERVER_PID/numa_maps" > "$placement_tmp/server-numa-maps-after-populate.txt"

    run_protocol_case "$memcache_ordinal" "$placement" memcache "$keys" "$placement_tmp"
    run_protocol_case "$http_ordinal" "$placement" http "$keys" "$placement_tmp"
    stop_server
    cp -a "$placement_tmp/." "$placement_dir/"
}

# Placement is the outer loop: one normal server, then one server switch to the distributed,
# promotion-disabled variant.  "Perverse" below is only a stable result label.
run_placement normal "$NORMAL_KEYS" 01 02
run_placement perverse "$PERVERSE_KEYS" 03 04

cp "$RESULTS_TMP" "$RUN_DIR/results.tsv"
cp "$RUN_LOG" "$RUN_DIR/run.log"
date -u +%FT%TZ > "$RUN_DIR/.done"
printf '[%s] all NUMA interconnect cases complete: %s\n' "$(date -u +%FT%TZ)" "$RUN_DIR"
