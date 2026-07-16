#!/usr/bin/env bash
set -euo pipefail

# Reproducible native-RDMA benchmark for a 4 GiB, uniformly selected working set of exact
# 256 KiB values. Run this on the client host after loading the dataset with the benchmark's
# `load` mode. Artifacts remain on client-local storage rather than an NFS-mounted home.

BIN=${BIN:-./build-client/goblin_store_rdma_benchmark}
OUT=${1:-/var/tmp/goblin-rdma-bench-256k-results}
ADDRESS=${ADDRESS:-10.88.88.1}
PORT=${PORT:-11211}
IBDEV=${IBDEV:-ibp65s0}
LATENCY_CPU=${LATENCY_CPU:-1}
THROUGHPUT_CPUS=${THROUGHPUT_CPUS:-1,5,9,13,17,21,25,29,33,37,41,45}
OBJECTS=${OBJECTS:-16384}
OBJECT_BYTES=${OBJECT_BYTES:-262144}
SEED=${SEED:-0x676f626c696e}
LATENCY_REPS=${LATENCY_REPS:-5}
LATENCY_REQUESTS=${LATENCY_REQUESTS:-200000}
LATENCY_WARMUP=${LATENCY_WARMUP:-5000}
THROUGHPUT_QPS=${THROUGHPUT_QPS:-"1 2 4 8 12"}
THROUGHPUT_WARMUP_SECONDS=${THROUGHPUT_WARMUP_SECONDS:-5}
THROUGHPUT_SECONDS=${THROUGHPUT_SECONDS:-20}
CONFIRM_QPS=${CONFIRM_QPS:-2}
CONFIRM_REPS=${CONFIRM_REPS:-3}
CONFIRM_CPUS=${CONFIRM_CPUS:-1,5}
CONFIRM_WARMUP_SECONDS=${CONFIRM_WARMUP_SECONDS:-5}
CONFIRM_SECONDS=${CONFIRM_SECONDS:-30}
COUNTERS=/sys/class/infiniband/${IBDEV}/ports/1/counters

mkdir -p "${OUT}"

{
    date --iso-8601=seconds
    hostname
    uname -a
    printf 'benchmark_binary='
    sha256sum "${BIN}"
    printf 'address=%s port=%s objects=%s object_bytes=%s dataset_bytes=%s seed=%s\n' \
        "${ADDRESS}" "${PORT}" "${OBJECTS}" "${OBJECT_BYTES}" \
        "$((OBJECTS * OBJECT_BYTES))" "${SEED}"
    printf 'latency_cpu=%s throughput_cpus=%s\n' \
        "${LATENCY_CPU}" "${THROUGHPUT_CPUS}"
    printf 'latency_reps=%s latency_requests=%s latency_warmup=%s\n' \
        "${LATENCY_REPS}" "${LATENCY_REQUESTS}" "${LATENCY_WARMUP}"
    printf 'throughput_qps=%s warmup_seconds=%s measurement_seconds=%s\n' \
        "${THROUGHPUT_QPS}" "${THROUGHPUT_WARMUP_SECONDS}" \
        "${THROUGHPUT_SECONDS}"
    printf 'confirm_qps=%s confirm_reps=%s confirm_cpus=%s warmup_seconds=%s measurement_seconds=%s\n' \
        "${CONFIRM_QPS}" "${CONFIRM_REPS}" "${CONFIRM_CPUS}" \
        "${CONFIRM_WARMUP_SECONDS}" "${CONFIRM_SECONDS}"
    cat "/sys/class/infiniband/${IBDEV}/ports/1/state"
    cat "/sys/class/infiniband/${IBDEV}/ports/1/rate"
    ibv_devinfo -d "${IBDEV}" -i 1
    lscpu
} > "${OUT}/metadata.txt"

printf 'phase,rep,qps,xmit_data_before,xmit_data_after,rcv_data_before,rcv_data_after,xmit_packets_before,xmit_packets_after,rcv_packets_before,rcv_packets_after\n' \
    > "${OUT}/hca-counters.csv"

record_counters() {
    local phase=$1
    local rep=$2
    local qps=$3
    local xmit_data_before=$4
    local rcv_data_before=$5
    local xmit_packets_before=$6
    local rcv_packets_before=$7
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${phase}" "${rep}" "${qps}" \
        "${xmit_data_before}" "$(<"${COUNTERS}/port_xmit_data")" \
        "${rcv_data_before}" "$(<"${COUNTERS}/port_rcv_data")" \
        "${xmit_packets_before}" "$(<"${COUNTERS}/port_xmit_packets")" \
        "${rcv_packets_before}" "$(<"${COUNTERS}/port_rcv_packets")" \
        >> "${OUT}/hca-counters.csv"
}

for ((rep = 1; rep <= LATENCY_REPS; ++rep)); do
    xmit_data_before=$(<"${COUNTERS}/port_xmit_data")
    rcv_data_before=$(<"${COUNTERS}/port_rcv_data")
    xmit_packets_before=$(<"${COUNTERS}/port_xmit_packets")
    rcv_packets_before=$(<"${COUNTERS}/port_rcv_packets")
    taskset -c "${LATENCY_CPU}" "${BIN}" latency \
        --address "${ADDRESS}" --port "${PORT}" \
        --objects "${OBJECTS}" --object-bytes "${OBJECT_BYTES}" --seed "${SEED}" \
        --warmup-requests "${LATENCY_WARMUP}" --requests "${LATENCY_REQUESTS}" \
        --csv "${OUT}/latency-rep-${rep}.csv" \
        > "${OUT}/latency-rep-${rep}.txt"
    record_counters latency "${rep}" 1 \
        "${xmit_data_before}" "${rcv_data_before}" \
        "${xmit_packets_before}" "${rcv_packets_before}"
done

for qps in ${THROUGHPUT_QPS}; do
    xmit_data_before=$(<"${COUNTERS}/port_xmit_data")
    rcv_data_before=$(<"${COUNTERS}/port_rcv_data")
    xmit_packets_before=$(<"${COUNTERS}/port_xmit_packets")
    rcv_packets_before=$(<"${COUNTERS}/port_rcv_packets")
    taskset -c "${THROUGHPUT_CPUS}" "${BIN}" throughput \
        --address "${ADDRESS}" --port "${PORT}" \
        --objects "${OBJECTS}" --object-bytes "${OBJECT_BYTES}" --seed "${SEED}" \
        --threads "${qps}" --warmup-seconds "${THROUGHPUT_WARMUP_SECONDS}" \
        --seconds "${THROUGHPUT_SECONDS}" \
        --csv "${OUT}/throughput-${qps}-qp.csv" \
        > "${OUT}/throughput-${qps}-qp.txt"
    record_counters throughput 1 "${qps}" \
        "${xmit_data_before}" "${rcv_data_before}" \
        "${xmit_packets_before}" "${rcv_packets_before}"
done

for ((rep = 1; rep <= CONFIRM_REPS; ++rep)); do
    xmit_data_before=$(<"${COUNTERS}/port_xmit_data")
    rcv_data_before=$(<"${COUNTERS}/port_rcv_data")
    xmit_packets_before=$(<"${COUNTERS}/port_xmit_packets")
    rcv_packets_before=$(<"${COUNTERS}/port_rcv_packets")
    taskset -c "${CONFIRM_CPUS}" "${BIN}" throughput \
        --address "${ADDRESS}" --port "${PORT}" \
        --objects "${OBJECTS}" --object-bytes "${OBJECT_BYTES}" --seed "${SEED}" \
        --threads "${CONFIRM_QPS}" --warmup-seconds "${CONFIRM_WARMUP_SECONDS}" \
        --seconds "${CONFIRM_SECONDS}" \
        --csv "${OUT}/throughput-${CONFIRM_QPS}-qp-confirm-${rep}.csv" \
        > "${OUT}/throughput-${CONFIRM_QPS}-qp-confirm-${rep}.txt"
    record_counters confirmation "${rep}" "${CONFIRM_QPS}" \
        "${xmit_data_before}" "${rcv_data_before}" \
        "${xmit_packets_before}" "${rcv_packets_before}"
done

date --iso-8601=seconds > "${OUT}/complete.txt"
