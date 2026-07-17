#!/usr/bin/env bash
# Pin every MSI/MSI-X vector for a PCI device to CPUs local to one NUMA node.
# IRQ numbers are discovered dynamically because Linux may renumber them after
# a reboot or PCI topology change.

set -Eeuo pipefail

if [[ $# != 2 ]]; then
    printf 'usage: %s PCI_BDF NUMA_NODE\n' "$0" >&2
    exit 2
fi

pci=$1
node=$2
device=/sys/bus/pci/devices/$pci
node_dir=/sys/devices/system/node/node$node

[[ $pci =~ ^[[:xdigit:]]{4}:[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[[:xdigit:]]$ ]] || {
    printf 'invalid PCI BDF: %s\n' "$pci" >&2
    exit 2
}
[[ $node =~ ^[0-9]+$ ]] || {
    printf 'invalid NUMA node: %s\n' "$node" >&2
    exit 2
}
[[ -d $device ]] || { printf 'PCI device not found: %s\n' "$pci" >&2; exit 1; }
[[ -d $node_dir ]] || { printf 'NUMA node not found: %s\n' "$node" >&2; exit 1; }

actual_node=$(<"$device/numa_node")
[[ $actual_node == "$node" ]] || {
    printf 'PCI device %s belongs to NUMA node %s, not requested node %s\n' \
        "$pci" "$actual_node" "$node" >&2
    exit 1
}

cpus=$(<"$node_dir/cpulist")
[[ -n $cpus ]] || { printf 'NUMA node %s has no online CPUs\n' "$node" >&2; exit 1; }

expand_cpulist() {
    local list=$1 part first last cpu
    local IFS=,
    for part in $list; do
        if [[ $part == *-* ]]; then
            first=${part%-*}
            last=${part#*-}
            for ((cpu = first; cpu <= last; ++cpu)); do printf '%s\n' "$cpu"; done
        else
            printf '%s\n' "$part"
        fi
    done
}

declare -A allowed=()
while IFS= read -r cpu; do allowed[$cpu]=1; done < <(expand_cpulist "$cpus")

shopt -s nullglob
irq_files=("$device"/msi_irqs/*)
(( ${#irq_files[@]} > 0 )) || {
    printf 'PCI device %s has no MSI/MSI-X IRQs\n' "$pci" >&2
    exit 1
}

printf 'pci=%s numa_node=%s allowed_cpus=%s irq_count=%s\n' \
    "$pci" "$node" "$cpus" "${#irq_files[@]}"

for irq_file in "${irq_files[@]}"; do
    irq=${irq_file##*/}
    affinity=/proc/irq/$irq/smp_affinity_list
    effective_file=/proc/irq/$irq/effective_affinity_list
    [[ -w $affinity ]] || { printf 'IRQ %s affinity is not writable\n' "$irq" >&2; exit 1; }

    printf '%s\n' "$cpus" > "$affinity"
    configured=$(<"$affinity")
    effective=$(<"$effective_file")

    while IFS= read -r cpu; do
        [[ ${allowed[$cpu]+present} ]] || {
            printf 'IRQ %s remained effective on nonlocal CPU %s (%s)\n' \
                "$irq" "$cpu" "$effective" >&2
            exit 1
        }
    done < <(expand_cpulist "$effective")

    printf 'irq=%s type=%s configured=%s effective=%s\n' \
        "$irq" "$(<"$irq_file")" "$configured" "$effective"
done
