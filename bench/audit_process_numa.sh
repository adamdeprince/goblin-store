#!/usr/bin/env bash
# Fail unless every thread in a process is CPU-affine to the requested NUMA node.
# Also record the process memory-policy view, actual resident-page placement, io_uring
# descriptors, and any io-wq helpers owned by the process. Run as root when the target
# is root-owned so /proc/<pid>/fd and numa_maps are readable.

set -Eeuo pipefail

usage() {
    printf 'usage: %s [--require-io-uring] PID NUMA_NODE\n' "$0" >&2
    exit 2
}

require_io_uring=false
if [[ ${1:-} == --require-io-uring ]]; then
    require_io_uring=true
    shift
fi
[[ $# == 2 ]] || usage

pid=$1
node=$2
[[ $pid =~ ^[1-9][0-9]*$ ]] || usage
[[ $node =~ ^[0-9]+$ ]] || usage
[[ -d /proc/$pid/task ]] || {
    printf 'process %s is not running\n' "$pid" >&2
    exit 1
}

node_dir=/sys/devices/system/node/node$node
[[ -d $node_dir ]] || {
    printf 'NUMA node %s is not present\n' "$node" >&2
    exit 1
}
node_cpus=$(<"$node_dir/cpulist")
[[ -n $node_cpus ]] || {
    printf 'NUMA node %s has no online CPUs\n' "$node" >&2
    exit 1
}

expand_list() {
    local list=$1 part first last value
    local IFS=,
    for part in $list; do
        if [[ $part == *-* ]]; then
            first=${part%-*}
            last=${part#*-}
            for ((value = first; value <= last; ++value)); do printf '%s\n' "$value"; done
        else
            printf '%s\n' "$part"
        fi
    done
}

declare -A node_cpu_set=()
while IFS= read -r cpu; do node_cpu_set[$cpu]=1; done < <(expand_list "$node_cpus")

check_cpu_subset() {
    local label=$1 list=$2 cpu
    [[ -n $list ]] || {
        printf '%s has an empty CPU allowance\n' "$label" >&2
        return 1
    }
    while IFS= read -r cpu; do
        [[ ${node_cpu_set[$cpu]+present} ]] || {
            printf '%s allows nonlocal CPU %s (allowed=%s, node%s=%s)\n' \
                "$label" "$cpu" "$list" "$node" "$node_cpus" >&2
            return 1
        }
    done < <(expand_list "$list")
}

read_status_list() {
    local status=$1 field=$2
    sed -n "s/^${field}:[[:space:]]*//p" "$status"
}

exe=$(readlink -f "/proc/$pid/exe")
cmdline=$(tr '\0' ' ' < "/proc/$pid/cmdline")
printf 'pid=%s exe=%s requested_node=%s node_cpus=%s\n' "$pid" "$exe" "$node" "$node_cpus"
printf 'cmdline=%s\n' "$cmdline"

task_count=0
for status in /proc/$pid/task/*/status; do
    tid_dir=${status%/status}
    tid=${tid_dir##*/}
    comm=$(<"$tid_dir/comm")
    cpus=$(read_status_list "$status" Cpus_allowed_list)
    mems=$(read_status_list "$status" Mems_allowed_list)
    check_cpu_subset "task $tid ($comm)" "$cpus"
    printf 'task=%s comm=%s cpus=%s cpuset_mems=%s\n' "$tid" "$comm" "$cpus" "$mems"
    ((task_count += 1))
done
(( task_count > 0 )) || {
    printf 'process %s has no visible tasks\n' "$pid" >&2
    exit 1
}
printf 'task_count=%s\n' "$task_count"

# Mems_allowed_list is the cpuset/cgroup ceiling, not set_mempolicy(2). numa_maps
# reports the effective policy of each mapping and the nodes holding its resident pages.
printf 'numa_map_policies:\n'
cut -d ' ' -f 2 "/proc/$pid/numa_maps" | sort | uniq -c
while IFS= read -r policy; do
    case $policy in
        "bind:$node"|"bind=static:$node"|"bind=relative:$node") ;;
        *)
            printf 'process %s has nonlocal mapping policy %s (expected node %s)\n' \
                "$pid" "$policy" "$node" >&2
            exit 1
            ;;
    esac
done < <(cut -d ' ' -f 2 "/proc/$pid/numa_maps" | sort -u)
printf 'resident_pages_by_node:\n'
grep -oE 'N[0-9]+=[0-9]+' "/proc/$pid/numa_maps" \
    | cut -d = -f 1 | sort | uniq -c

ring_count=0
for fd in /proc/$pid/fd/*; do
    [[ -e $fd || -L $fd ]] || continue
    [[ $(readlink "$fd" 2>/dev/null || true) == 'anon_inode:[io_uring]' ]] || continue
    ((ring_count += 1))
done
printf 'io_uring_fd_count=%s\n' "$ring_count"
if $require_io_uring && (( ring_count == 0 )); then
    printf 'process %s has no io_uring descriptors\n' "$pid" >&2
    exit 1
fi

# Native io-wq workers are separate tasks on kernels that advertise
# IORING_FEAT_NATIVE_WORKERS. They retain the owning userspace PID in their name;
# audit any that exist at this instant. It is also valid for native async operations
# to complete without creating an io-wq fallback worker.
iowq_count=0
for proc_status in /proc/[1-9]*/status; do
    [[ -r $proc_status ]] || continue
    proc_dir=${proc_status%/status}
    helper_pid=${proc_dir##*/}
    comm=$(cat "$proc_dir/comm" 2>/dev/null || true)
    [[ $comm == iou-*"-$pid" || $comm == iou-*"-$pid"-* ]] || continue
    cpus=$(read_status_list "$proc_status" Cpus_allowed_list || true)
    [[ -n $cpus ]] || continue
    check_cpu_subset "io-wq helper $helper_pid ($comm)" "$cpus"
    printf 'io_wq_pid=%s comm=%s cpus=%s\n' "$helper_pid" "$comm" "$cpus"
    ((iowq_count += 1))
done
printf 'io_wq_helper_count=%s\n' "$iowq_count"
printf 'numa_affinity_audit=PASS\n'
