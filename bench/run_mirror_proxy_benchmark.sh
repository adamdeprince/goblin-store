#!/usr/bin/env bash
# Reproducible Vinyl Cache vs Goblin Store --mirror benchmark.
#
# Run on hail inside tmux. Both proxies expose ordinary HTTP over TCP/IPoIB at 10.88.88.20; native
# Goblin RDMA is intentionally absent. rain buffers every case's timing rows in RAM and writes its
# local /var/tmp only after that case completes. The controller copies completed artifacts afterward.

set -Eeuo pipefail

SERVER_HOST=${SERVER_HOST:-hail}
CLIENT_HOST=${CLIENT_HOST:-rain}
SERVER_IP=${SERVER_IP:-10.88.88.20}
CLIENT_IP=${CLIENT_IP:-10.88.88.2}
SERVER_IFACE=${SERVER_IFACE:-ibp3s0}
CLIENT_IFACE=${CLIENT_IFACE:-ibp65s0}
SERVER_NUMA_NODE=${SERVER_NUMA_NODE:-0}
CLIENT_NUMA_NODE=${CLIENT_NUMA_NODE:-1}

DATASET=${DATASET:-$HOME/bulky/massive-download/stock_trade}
CACHE_DIR=${CACHE_DIR:-/var/tmp/test}
ORIGIN_PORT=${ORIGIN_PORT:-18000}
PROXY_PORT=${PROXY_PORT:-8080}
VINYL_STORAGE=${VINYL_STORAGE:-2800G}
# The cache holds multi-gigabyte objects on rotating media.  MADV_RANDOM turns
# each mmap fault into a 4 KiB disk read; Vinyl itself recommends sequential
# advice for this geometry.
VINYL_ADVICE=${VINYL_ADVICE:-sequential}
GOBLIN_MEMORY=${GOBLIN_MEMORY:-64G}
# A 32 MiB head requires an allocation block at least that large.  Matching
# sizes gives this large-object benchmark one fixed head per logical block.
GOBLIN_BLOCK=${GOBLIN_BLOCK:-32M}
GOBLIN_HEAD=${GOBLIN_HEAD:-32M}
# 64 GiB / 2 MiB per x86 HugeTLB page.  Partial reservation is harmless:
# Goblin Store falls back to ordinary node-local mappings block by block.
HUGETLB_PAGES=${HUGETLB_PAGES:-32768}
STORAGE_PCI=${STORAGE_PCI:-0000:02:00.0}
NETWORK_PCI=${NETWORK_PCI:-0000:03:00.0}

PASSES=${PASSES:-3}
PREWARM_CONCURRENCY=${PREWARM_CONCURRENCY:-11}
CONCURRENCIES=${CONCURRENCIES:-"1 2 3 5 8 11"}
SEED=${SEED:-0x243f6a8885a308d3}
PROXIES=${PROXIES:-"vinyl goblin"}

TOOLS_DIR=${TOOLS_DIR:-/var/tmp/mirror-proxy-tools}
ORIGIN_SCRIPT=${ORIGIN_SCRIPT:-$TOOLS_DIR/mirror_origin_server.py}
MANIFEST_SCRIPT=${MANIFEST_SCRIPT:-$TOOLS_DIR/mirror_make_manifest.py}
CLIENT_SOURCE=${CLIENT_SOURCE:-$TOOLS_DIR/mirror_proxy_benchmark.cpp}
CLIENT_BIN=${CLIENT_BIN:-/var/tmp/mirror-proxy-benchmark}
VINYL_BIN=${VINYL_BIN:-/opt/vinyl-cache-9.0.1/sbin/vinyld}
GOBLIN_BIN=${GOBLIN_BIN:-/opt/goblin-store-mirror-bench/bin/goblin-store}
PATH_PREP_BIN=${PATH_PREP_BIN:-/opt/goblin-store-mirror-bench/bin/goblin-store-path-prep}
IRQ_PIN_SCRIPT=${IRQ_PIN_SCRIPT:-$TOOLS_DIR/pin_pci_irqs_to_numa.sh}
NUMA_AUDIT_SCRIPT=${NUMA_AUDIT_SCRIPT:-$TOOLS_DIR/audit_process_numa.sh}

RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
RUN_DIR=${RUN_DIR:-/var/tmp/mirror-proxy-bench-runs/$RUN_ID}
CLIENT_RUN_DIR=${CLIENT_RUN_DIR:-/var/tmp/mirror-proxy-bench-$RUN_ID}
MANIFEST=$RUN_DIR/manifest.tsv
CACHE_MARKER=$CACHE_DIR/.mirror-proxy-benchmark-owned

ORIGIN_PID=
PROXY_PID=
GOBLIN_PID=
CURRENT_PROXY=
ORIGINAL_HUGETLB_PAGES=

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

remote() {
    local command
    printf -v command '%q ' "$@"
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_HOST" "$command"
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
    if sudo -n kill -0 "$pid" 2>/dev/null; then
        sudo -n kill -TERM "$pid" 2>/dev/null || true
        for _ in $(seq 1 300); do
            sudo -n kill -0 "$pid" 2>/dev/null || return 0
            sleep 0.2
        done
        sudo -n kill -KILL "$pid" 2>/dev/null || true
    fi
}

stop_proxy() {
    if [[ -n ${GOBLIN_PID:-} && $GOBLIN_PID != "${PROXY_PID:-}" ]]; then
        stop_pid "$GOBLIN_PID"
    fi
    stop_pid "$PROXY_PID"
    PROXY_PID=
    GOBLIN_PID=
}

stop_origin() {
    stop_pid "$ORIGIN_PID"
    ORIGIN_PID=
}

restore_hugetlb() {
    [[ -n ${ORIGINAL_HUGETLB_PAGES:-} ]] || return 0
    local control=/sys/devices/system/node/node${SERVER_NUMA_NODE}/hugepages/hugepages-2048kB/nr_hugepages
    sudo -n sh -c "echo '$ORIGINAL_HUGETLB_PAGES' > '$control'" || true
    ORIGINAL_HUGETLB_PAGES=
}

cleanup() {
    stop_proxy
    stop_origin
    restore_hugetlb
}
trap cleanup EXIT INT TERM

clear_cache() {
    [[ $CACHE_DIR == /var/tmp/test ]] || die "refusing unexpected CACHE_DIR=$CACHE_DIR"
    sudo -n mkdir -p "$CACHE_DIR"
    if [[ ! -e $CACHE_MARKER ]]; then
        if find "$CACHE_DIR" -mindepth 1 -print -quit | grep -q .; then
            die "$CACHE_DIR is nonempty and has no benchmark ownership marker"
        fi
        sudo -n touch "$CACHE_MARKER"
    fi
    sudo -n find "$CACHE_DIR" -mindepth 1 ! -name "$(basename "$CACHE_MARKER")" -delete
    sync -f "$CACHE_DIR"
    [[ -e $CACHE_MARKER ]] || die "cache ownership marker disappeared"
}

origin_count() {
    local log=$1
    [[ -e $log ]] || { printf '0\n'; return; }
    wc -l < "$log"
}

start_origin() {
    local proxy=$1
    local access_log=$RUN_DIR/$proxy/origin-access.jsonl
    mkdir -p "$RUN_DIR/$proxy"
    : > "$access_log"
    python3 "$ORIGIN_SCRIPT" --directory "$DATASET" --access-log "$access_log" \
        --bind 127.0.0.1 --port "$ORIGIN_PORT" \
        > "$RUN_DIR/$proxy/origin.stdout" 2> "$RUN_DIR/$proxy/origin.stderr" &
    ORIGIN_PID=$!
    wait_for_listener "$ORIGIN_PORT" "$ORIGIN_PID" || {
        tail -100 "$RUN_DIR/$proxy/origin.stderr" >&2 || true
        die "origin did not listen on port $ORIGIN_PORT"
    }
}

start_vinyl() {
    local dir=$RUN_DIR/vinyl
    local work=$dir/work
    local storage_file=$CACHE_DIR/vinyl.bin
    mkdir -p "$work"
    [[ $VINYL_ADVICE == sequential ]] ||
        die "this large-object rotational benchmark requires VINYL_ADVICE=sequential"
    [[ ! -e $storage_file ]] || die "unexpected pre-existing Vinyl storage file $storage_file"

    printf '[%s] preallocating Vinyl storage: %s (%s)\n' \
        "$(date -u +%FT%TZ)" "$storage_file" "$VINYL_STORAGE"
    fallocate -l "$VINYL_STORAGE" "$storage_file"
    sync -f "$storage_file"
    local expected_size actual_size allocated_size
    expected_size=$(numfmt --from=iec "$VINYL_STORAGE")
    actual_size=$(stat -c %s "$storage_file")
    allocated_size=$(( $(stat -c %b "$storage_file") * 512 ))
    [[ $actual_size == "$expected_size" ]] ||
        die "Vinyl storage size is $actual_size bytes; expected $expected_size"
    (( allocated_size >= actual_size )) ||
        die "Vinyl storage is sparse: $allocated_size of $actual_size bytes allocated"
    {
        printf 'path=%s\nrequested=%s\nexpected_bytes=%s\n' \
            "$storage_file" "$VINYL_STORAGE" "$expected_size"
        printf 'size_bytes=%s\nallocated_bytes=%s\nadvice=%s\n' \
            "$actual_size" "$allocated_size" "$VINYL_ADVICE"
        stat "$storage_file"
        df -hT "$storage_file"
    } > "$dir/storage-allocation.txt"
    if command -v filefrag >/dev/null; then
        filefrag -v "$storage_file" > "$dir/storage-filefrag.txt" 2>&1 || true
    fi

    cat > "$dir/benchmark.vcl" <<EOF
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
    /usr/bin/numactl --cpunodebind="$SERVER_NUMA_NODE" \
        "$VINYL_BIN" -F -j none -n "$work" -a "$SERVER_IP:$PROXY_PORT" \
        -f "$dir/benchmark.vcl" \
        -s "file,$storage_file,$VINYL_STORAGE,2M,$VINYL_ADVICE" \
        -p default_grace=0 -p default_keep=0 \
        > "$dir/server.stdout" 2> "$dir/server.stderr" &
    PROXY_PID=$!
    wait_for_listener "$PROXY_PORT" "$PROXY_PID" || {
        tail -200 "$dir/server.stderr" >&2 || true
        die "Vinyl did not listen on $SERVER_IP:$PROXY_PORT"
    }
    printf '%s\n' "$PROXY_PID" > "$dir/server.pid"
}

reserve_hugetlb() {
    local control=/sys/devices/system/node/node${SERVER_NUMA_NODE}/hugepages/hugepages-2048kB/nr_hugepages
    ORIGINAL_HUGETLB_PAGES=$(cat "$control")
    printf '[%s] compacting node memory before Goblin Store HugeTLB reservation\n' \
        "$(date -u +%FT%TZ)" | tee -a "$RUN_DIR/goblin/hugetlb.txt"
    sudo -n sh -c 'echo 1 > /proc/sys/vm/compact_memory'
    sudo -n sh -c "echo '$HUGETLB_PAGES' > '$control'"
    local actual
    actual=$(cat "$control")
    printf 'requested_hugetlb_pages=%s actual_hugetlb_pages=%s\n' \
        "$HUGETLB_PAGES" "$actual" | tee -a "$RUN_DIR/goblin/hugetlb.txt"
}

start_goblin() {
    local dir=$RUN_DIR/goblin
    local pool=$CACHE_DIR/goblin
    mkdir -p "$dir"
    reserve_hugetlb
    sudo -n "$PATH_PREP_BIN" "$pool" > "$dir/path-prep.log" 2>&1
    # The outer policy is established before Goblin creates any rings.  --numa
    # repeats and validates it internally for application threads and arenas;
    # inherited affinity also constrains any kernel io-wq helpers.
    sudo -n /usr/bin/prlimit --memlock=unlimited:unlimited -- \
        /usr/bin/numactl --cpunodebind="$SERVER_NUMA_NODE" --membind="$SERVER_NUMA_NODE" \
        "$GOBLIN_BIN" --mirror "http://127.0.0.1:$ORIGIN_PORT" \
        --listen-address "$SERVER_IP" --http-port "$PROXY_PORT" --no-memcache --net async \
        --numa "$SERVER_NUMA_NODE" --memory "$GOBLIN_MEMORY" --block "$GOBLIN_BLOCK" \
        --ram-head "$GOBLIN_HEAD" --no-numa-promotion --ssd-dir "$pool" \
        > "$dir/server.stdout" 2> "$dir/server.stderr" &
    local launcher_pid=$!
    PROXY_PID=$launcher_pid
    wait_for_listener "$PROXY_PORT" "$launcher_pid" || {
        tail -200 "$dir/server.stdout" >&2 || true
        tail -200 "$dir/server.stderr" >&2 || true
        die "Goblin Store did not listen on $SERVER_IP:$PROXY_PORT"
    }

    # sudo/prlimit/numactl may leave a launcher/monitor process above the server.
    # Resolve the executable whose command line names this phase's unique pool.
    local candidate candidate_exe candidate_cmdline
    for _ in $(seq 1 100); do
        while IFS= read -r candidate; do
            [[ -r /proc/$candidate/cmdline ]] || continue
            candidate_exe=$(sudo -n readlink -f "/proc/$candidate/exe" 2>/dev/null || true)
            candidate_cmdline=$(tr '\0' ' ' < "/proc/$candidate/cmdline" 2>/dev/null || true)
            if [[ $candidate_exe == "$GOBLIN_BIN" && $candidate_cmdline == *"$pool"* ]]; then
                GOBLIN_PID=$candidate
                break 2
            fi
        done < <(pgrep -x goblin-store || true)
        sleep 0.1
    done
    [[ -n $GOBLIN_PID ]] || die "could not resolve the Goblin Store process behind PID $launcher_pid"
    sleep 1 # allow every configured HTTP worker/ring to finish startup
    sudo -n "$NUMA_AUDIT_SCRIPT" --require-io-uring "$GOBLIN_PID" "$SERVER_NUMA_NODE" \
        > "$dir/numa-affinity-startup.txt"
    printf '%s\n' "$GOBLIN_PID" > "$dir/server.pid"
}

copy_client_artifact() {
    local remote_path=$1 local_path=$2
    mkdir -p "$(dirname "$local_path")"
    scp -q "$CLIENT_HOST:$remote_path" "$local_path"
}

run_client() {
    local output=$1 concurrency=$2 passes=$3
    shift 3
    remote /usr/bin/numactl --cpunodebind="$CLIENT_NUMA_NODE" --membind="$CLIENT_NUMA_NODE" \
        "$CLIENT_BIN" --host "$SERVER_IP" --source "$CLIENT_IP" --port "$PROXY_PORT" \
        --manifest "$CLIENT_RUN_DIR/manifest.tsv" --output "$output" \
        --concurrency "$concurrency" --passes "$passes" --seed "$SEED" "$@"
}

run_proxy() {
    local proxy=$1
    CURRENT_PROXY=$proxy
    printf '[%s] starting %s phase\n' "$(date -u +%FT%TZ)" "$proxy"
    ss -H -ltn | awk -v origin=":$ORIGIN_PORT" -v proxy=":$PROXY_PORT" \
        '$4 ~ origin "$" || $4 ~ proxy "$" {busy=1} END {exit !busy}' &&
        die "origin or proxy benchmark port is already occupied"
    clear_cache
    start_origin "$proxy"
    if [[ $proxy == vinyl ]]; then
        start_vinyl
    elif [[ $proxy == goblin ]]; then
        start_goblin
    else
        die "unknown proxy $proxy"
    fi

    local remote_dir=$CLIENT_RUN_DIR/$proxy
    remote mkdir -p "$remote_dir"
    printf '[%s] prewarming %s: one GET for each manifest object\n' \
        "$(date -u +%FT%TZ)" "$proxy"
    run_client "$remote_dir/prewarm.csv" "$PREWARM_CONCURRENCY" 1 \
        2>&1 | tee -a "$RUN_DIR/$proxy/prewarm.log"
    copy_client_artifact "$remote_dir/prewarm.csv" "$RUN_DIR/$proxy/prewarm.csv"

    if [[ $proxy == goblin ]]; then
        # Disk reads/writes have now occurred, so any io-wq fallback helper that this
        # workload needs has had an opportunity to appear.
        sudo -n "$NUMA_AUDIT_SCRIPT" --require-io-uring "$GOBLIN_PID" "$SERVER_NUMA_NODE" \
            > "$RUN_DIR/goblin/numa-affinity-after-prewarm.txt"
    fi

    local expected_origin_requests actual_origin_requests
    expected_origin_requests=$(awk 'BEGIN{n=0} $0 !~ /^#/ {n++} END{print n}' "$MANIFEST")
    actual_origin_requests=$(origin_count "$RUN_DIR/$proxy/origin-access.jsonl")
    [[ $actual_origin_requests == "$expected_origin_requests" ]] ||
        die "$proxy prewarm made $actual_origin_requests origin requests; expected $expected_origin_requests"

    printf '[%s] verifying a warmed %s key with a bodyless HEAD probe\n' \
        "$(date -u +%FT%TZ)" "$proxy"
    run_client "$remote_dir/head-probe.csv" 1 1 --head --max-requests 1 \
        2>&1 | tee -a "$RUN_DIR/$proxy/head-probe.log"
    copy_client_artifact "$remote_dir/head-probe.csv" "$RUN_DIR/$proxy/head-probe.csv"
    actual_origin_requests=$(origin_count "$RUN_DIR/$proxy/origin-access.jsonl")
    [[ $actual_origin_requests == "$expected_origin_requests" ]] ||
        die "$proxy HEAD probe missed the warmed cache"

    local concurrency
    for concurrency in $CONCURRENCIES; do
        printf '[%s] %s concurrency=%s passes=%s\n' \
            "$(date -u +%FT%TZ)" "$proxy" "$concurrency" "$PASSES"
        run_client "$remote_dir/c$concurrency.csv" "$concurrency" "$PASSES" \
            2>&1 | tee -a "$RUN_DIR/$proxy/c$concurrency.log"
        copy_client_artifact "$remote_dir/c$concurrency.csv" \
            "$RUN_DIR/$proxy/c$concurrency.csv"
        actual_origin_requests=$(origin_count "$RUN_DIR/$proxy/origin-access.jsonl")
        [[ $actual_origin_requests == "$expected_origin_requests" ]] ||
            die "$proxy benchmark generated origin traffic at concurrency $concurrency"
    done

    stop_proxy
    stop_origin
    if [[ $proxy == goblin ]]; then restore_hugetlb; fi
    printf '[%s] completed %s phase\n' "$(date -u +%FT%TZ)" "$proxy"
}

[[ $(hostname -s) == "$SERVER_HOST" ]] || die "run this controller on $SERVER_HOST"
sudo -n true || die "passwordless sudo is required on $SERVER_HOST"
remote sudo -n true || die "passwordless sudo is required on $CLIENT_HOST"
for executable in "$ORIGIN_SCRIPT" "$MANIFEST_SCRIPT" "$VINYL_BIN" "$GOBLIN_BIN" \
                  "$PATH_PREP_BIN" "$IRQ_PIN_SCRIPT" "$NUMA_AUDIT_SCRIPT"; do
    [[ -x $executable ]] || die "missing executable $executable"
done
for executable in fallocate numfmt stat; do
    command -v "$executable" >/dev/null || die "missing executable $executable"
done
[[ -f $CLIENT_SOURCE ]] || die "missing client source $CLIENT_SOURCE"
[[ -d $DATASET ]] || die "missing dataset $DATASET"
[[ $(find "$DATASET" -maxdepth 1 -type f -name '*.gz' | wc -l) == 1639 ]] ||
    die "dataset does not contain exactly 1639 top-level .gz files"
[[ $(cat /sys/module/megaraid_sas/parameters/smp_affinity_enable) == 0 ]] ||
    die "megaraid_sas managed IRQ affinity is enabled; PERC vectors cannot be pinned reliably"

server_route=$(ip -4 route get "$CLIENT_IP")
client_route=$(remote ip -4 route get "$SERVER_IP")
[[ $server_route == *"dev $SERVER_IFACE"* && $server_route == *"src $SERVER_IP"* ]] ||
    die "server IPoIB route is wrong: $server_route"
[[ $client_route == *"dev $CLIENT_IFACE"* && $client_route == *"src $CLIENT_IP"* ]] ||
    die "client IPoIB route is wrong: $client_route"
[[ $(cat "/sys/class/net/$SERVER_IFACE/mode") == connected ]] ||
    die "$SERVER_IFACE is not in connected IPoIB mode"
[[ $(remote cat "/sys/class/net/$CLIENT_IFACE/mode") == connected ]] ||
    die "$CLIENT_IFACE is not in connected IPoIB mode"
[[ $(cat "/sys/class/net/$SERVER_IFACE/mtu") == 65520 ]] ||
    die "$SERVER_IFACE does not have MTU 65520"
[[ $(remote cat "/sys/class/net/$CLIENT_IFACE/mtu") == 65520 ]] ||
    die "$CLIENT_IFACE does not have MTU 65520"
server_iface_numa=$(cat "/sys/class/net/$SERVER_IFACE/device/numa_node")
client_iface_numa=$(remote cat "/sys/class/net/$CLIENT_IFACE/device/numa_node")
storage_numa=$(cat "/sys/bus/pci/devices/$STORAGE_PCI/numa_node")
network_numa=$(cat "/sys/bus/pci/devices/$NETWORK_PCI/numa_node")
server_iface_pci=$(basename "$(readlink -f "/sys/class/net/$SERVER_IFACE/device")")
[[ $server_iface_numa == "$SERVER_NUMA_NODE" ]] ||
    die "$SERVER_IFACE belongs to NUMA node $server_iface_numa, not configured node $SERVER_NUMA_NODE"
[[ $client_iface_numa == "$CLIENT_NUMA_NODE" ]] ||
    die "$CLIENT_IFACE belongs to NUMA node $client_iface_numa, not configured node $CLIENT_NUMA_NODE"
[[ $storage_numa == "$SERVER_NUMA_NODE" ]] ||
    die "$STORAGE_PCI belongs to NUMA node $storage_numa, not server node $SERVER_NUMA_NODE"
[[ $server_iface_pci == "$NETWORK_PCI" ]] ||
    die "$SERVER_IFACE belongs to PCI $server_iface_pci, not configured PCI $NETWORK_PCI"
[[ $network_numa == "$SERVER_NUMA_NODE" ]] ||
    die "$NETWORK_PCI belongs to NUMA node $network_numa, not server node $SERVER_NUMA_NODE"

mkdir -p "$RUN_DIR"
exec > >(tee -a "$RUN_DIR/run.log") 2>&1
printf '[%s] mirror proxy benchmark %s\n' "$(date -u +%FT%TZ)" "$RUN_ID"
sudo -n "$IRQ_PIN_SCRIPT" "$STORAGE_PCI" "$SERVER_NUMA_NODE" \
    | tee "$RUN_DIR/storage-irq-affinity.txt"
sudo -n "$IRQ_PIN_SCRIPT" "$NETWORK_PCI" "$SERVER_NUMA_NODE" \
    | tee "$RUN_DIR/network-irq-affinity.txt"
python3 "$MANIFEST_SCRIPT" "$DATASET" "$MANIFEST" | tee "$RUN_DIR/manifest-summary.json"
remote mkdir -p "$CLIENT_RUN_DIR"
scp -q "$MANIFEST" "$CLIENT_HOST:$CLIENT_RUN_DIR/manifest.tsv"
scp -q "$CLIENT_SOURCE" "$CLIENT_HOST:$CLIENT_RUN_DIR/mirror_proxy_benchmark.cpp"
remote /usr/bin/g++ -O3 -std=c++20 -pthread -Wall -Wextra -Werror \
    "$CLIENT_RUN_DIR/mirror_proxy_benchmark.cpp" -o "$CLIENT_BIN"

{
    printf 'run_id=%s\nstarted_utc=%s\n' "$RUN_ID" "$(date -u +%FT%TZ)"
    printf 'transport=HTTP over TCP/IPoIB (Goblin native RDMA disabled)\n'
    printf 'server=%s %s %s numa_node=%s client=%s %s %s numa_node=%s\n' \
        "$SERVER_HOST" "$SERVER_IFACE" "$SERVER_IP" \
        "$SERVER_NUMA_NODE" "$CLIENT_HOST" "$CLIENT_IFACE" "$CLIENT_IP" \
        "$CLIENT_NUMA_NODE"
    printf 'passes=%s concurrencies=%s seed=%s\n' "$PASSES" "$CONCURRENCIES" "$SEED"
    printf 'vinyl_storage=%s vinyl_advice=%s vinyl_preallocated=true\n' \
        "$VINYL_STORAGE" "$VINYL_ADVICE"
    printf 'goblin_memory=%s goblin_block=%s goblin_head=%s\n' \
        "$GOBLIN_MEMORY" "$GOBLIN_BLOCK" "$GOBLIN_HEAD"
    printf 'storage_pci=%s storage_numa_node=%s irq_affinity=%s\n' \
        "$STORAGE_PCI" "$storage_numa" "$RUN_DIR/storage-irq-affinity.txt"
    printf 'network_pci=%s network_numa_node=%s irq_affinity=%s\n' \
        "$NETWORK_PCI" "$network_numa" "$RUN_DIR/network-irq-affinity.txt"
    printf 'server_route=%s\nclient_route=%s\n' "$server_route" "$client_route"
    sha256sum "$VINYL_BIN" "$GOBLIN_BIN" "$PATH_PREP_BIN" "$ORIGIN_SCRIPT" \
        "$MANIFEST_SCRIPT" "$MANIFEST"
    remote sha256sum "$CLIENT_BIN"
    printf '\nSERVER\n'
    uname -a
    lscpu
    /usr/bin/numactl --hardware
    ip -d -s link show "$SERVER_IFACE"
    df -hT "$CACHE_DIR"
    printf '\nCLIENT\n'
    remote uname -a
    remote lscpu
    remote /usr/bin/numactl --hardware
    remote ip -d -s link show "$CLIENT_IFACE"
} > "$RUN_DIR/metadata.txt"

for proxy in $PROXIES; do
    run_proxy "$proxy"
done

printf '[%s] all requested phases complete: %s\n' "$(date -u +%FT%TZ)" "$RUN_DIR"
