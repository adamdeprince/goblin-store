#!/usr/bin/env python3
"""Low-overhead Linux resource monitor for long-running benchmarks.

The monitor deliberately uses procfs/sysfs and the Python standard library so
it can run on benchmark hosts without installing psutil or sysstat.  It writes
line-buffered CSV files suitable for watching live and for later analysis.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import re
import signal
import socket
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


PAGE_SIZE = os.sysconf("SC_PAGE_SIZE")
CLK_TCK = os.sysconf("SC_CLK_TCK")


SYSTEM_FIELDS = [
    "timestamp_ns",
    "timestamp_utc",
    "elapsed_s",
    "load_1m",
    "load_5m",
    "load_15m",
    "runnable_tasks",
    "total_tasks",
    "cpu_busy_pct",
    "cpu_user_pct",
    "cpu_system_pct",
    "cpu_iowait_pct",
    "cpu_idle_pct",
    "target_processes",
    "target_cpu_pct",
    "target_rss_bytes",
    "target_vms_bytes",
    "mem_total_bytes",
    "mem_available_bytes",
    "mem_free_bytes",
    "active_bytes",
    "inactive_bytes",
    "anon_pages_bytes",
    "file_pages_bytes",
    "dirty_bytes",
    "writeback_bytes",
    "swap_total_bytes",
    "swap_free_bytes",
    "hugepages_total",
    "hugepages_free",
    "hugepages_reserved",
    "hugepages_surplus",
    "hugepage_size_bytes",
    "cpu_psi_some_avg10",
    "cpu_psi_some_avg60",
    "cpu_psi_some_avg300",
    "cpu_psi_some_total_us",
    "memory_psi_some_avg10",
    "memory_psi_some_avg60",
    "memory_psi_some_avg300",
    "memory_psi_some_total_us",
    "memory_psi_full_avg10",
    "memory_psi_full_avg60",
    "memory_psi_full_avg300",
    "memory_psi_full_total_us",
    "io_psi_some_avg10",
    "io_psi_some_avg60",
    "io_psi_some_avg300",
    "io_psi_some_total_us",
    "io_psi_full_avg10",
    "io_psi_full_avg60",
    "io_psi_full_avg300",
    "io_psi_full_total_us",
]

PROCESS_FIELDS = [
    "timestamp_ns",
    "timestamp_utc",
    "elapsed_s",
    "pid",
    "ppid",
    "state",
    "threads",
    "cpu_pct",
    "rss_bytes",
    "vms_bytes",
    "read_bytes",
    "write_bytes",
    "minor_faults",
    "major_faults",
    "comm",
    "cmdline",
]

VMSTAT_KEYS = [
    "pgfault",
    "pgmajfault",
    "pswpin",
    "pswpout",
    "pgscan_kswapd",
    "pgscan_direct",
    "pgsteal_kswapd",
    "pgsteal_direct",
    "allocstall",
    "compact_stall",
    "compact_fail",
    "compact_success",
    "numa_hit",
    "numa_miss",
    "numa_foreign",
    "numa_interleave",
    "numa_local",
    "numa_other",
]

NODE_FIELDS = [
    "timestamp_ns",
    "timestamp_utc",
    "elapsed_s",
    "node",
    "mem_total_bytes",
    "mem_free_bytes",
    "mem_used_bytes",
    "active_bytes",
    "inactive_bytes",
    "anon_pages_bytes",
    "file_pages_bytes",
    "dirty_bytes",
    "writeback_bytes",
    "hugepages_total",
    "hugepages_free",
    "hugepages_surplus",
]


@dataclass
class ProcessSample:
    pid: int
    ppid: int
    state: str
    threads: int
    ticks: int
    start_ticks: int
    rss_bytes: int
    vms_bytes: int
    read_bytes: int
    write_bytes: int
    minor_faults: int
    major_faults: int
    comm: str
    cmdline: str
    cpu_pct: float | None = None


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def parse_key_values(path: Path, *, multiplier: int = 1) -> dict[str, int]:
    values: dict[str, int] = {}
    try:
        lines = read_text(path).splitlines()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return values
    for line in lines:
        fields = line.split()
        if len(fields) < 2:
            continue
        key = fields[0].rstrip(":")
        try:
            value = int(fields[1])
        except ValueError:
            continue
        if len(fields) >= 3 and fields[2].lower() == "kb":
            value *= 1024
        else:
            value *= multiplier
        values[key] = value
    return values


def read_cpu_times() -> dict[str, int]:
    fields = read_text(Path("/proc/stat")).splitlines()[0].split()
    names = ["user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal"]
    return {name: int(value) for name, value in zip(names, fields[1:])}


def cpu_percentages(
    current: dict[str, int], previous: dict[str, int] | None
) -> dict[str, float | None]:
    result = {"busy": None, "user": None, "system": None, "iowait": None, "idle": None}
    if previous is None:
        return result
    delta = {key: current.get(key, 0) - previous.get(key, 0) for key in current}
    total = sum(delta.values())
    if total <= 0:
        return result
    idle = delta.get("idle", 0)
    iowait = delta.get("iowait", 0)
    result["busy"] = 100.0 * (total - idle - iowait) / total
    result["user"] = 100.0 * (delta.get("user", 0) + delta.get("nice", 0)) / total
    result["system"] = 100.0 * (
        delta.get("system", 0) + delta.get("irq", 0) + delta.get("softirq", 0)
    ) / total
    result["iowait"] = 100.0 * iowait / total
    result["idle"] = 100.0 * idle / total
    return result


def read_pressure(resource: str) -> dict[str, float | int]:
    result: dict[str, float | int] = {}
    try:
        lines = read_text(Path("/proc/pressure") / resource).splitlines()
    except (FileNotFoundError, PermissionError):
        return result
    for line in lines:
        fields = line.split()
        if not fields:
            continue
        kind = fields[0]
        for field in fields[1:]:
            key, value = field.split("=", 1)
            result[f"{kind}_{key}"] = int(value) if key == "total" else float(value)
    return result


def parse_proc_stat(pid: int) -> tuple[str, list[str]] | None:
    try:
        value = read_text(Path("/proc") / str(pid) / "stat").strip()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return None
    left = value.find("(")
    right = value.rfind(")")
    if left < 0 or right < left:
        return None
    return value[left + 1 : right], value[right + 2 :].split()


def process_sample(pid: int) -> ProcessSample | None:
    parsed = parse_proc_stat(pid)
    if parsed is None:
        return None
    comm, stat = parsed
    try:
        # stat starts at field 3 (state); see proc_pid_stat(5).
        state = stat[0]
        ppid = int(stat[1])
        minor_faults = int(stat[7])
        major_faults = int(stat[9])
        ticks = int(stat[11]) + int(stat[12])
        start_ticks = int(stat[19])
        vms_bytes = int(stat[20])
        rss_bytes = int(stat[21]) * PAGE_SIZE
    except (IndexError, ValueError):
        return None

    proc_dir = Path("/proc") / str(pid)
    try:
        cmdline = read_text(proc_dir / "cmdline").replace("\0", " ").strip()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        cmdline = ""
    status = parse_key_values(proc_dir / "status")
    io = parse_key_values(proc_dir / "io")
    return ProcessSample(
        pid=pid,
        ppid=ppid,
        state=state,
        threads=status.get("Threads", 0),
        ticks=ticks,
        start_ticks=start_ticks,
        rss_bytes=rss_bytes,
        vms_bytes=vms_bytes,
        read_bytes=io.get("read_bytes", 0),
        write_bytes=io.get("write_bytes", 0),
        minor_faults=minor_faults,
        major_faults=major_faults,
        comm=comm,
        cmdline=cmdline or f"[{comm}]",
    )


def matching_processes(patterns: list[re.Pattern[str]]) -> list[ProcessSample]:
    samples: list[ProcessSample] = []
    own_pid = os.getpid()
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        if pid == own_pid:
            continue
        sample = process_sample(pid)
        if sample is None:
            continue
        if "monitor_benchmark_resources.py" in sample.cmdline:
            continue
        searchable = f"{sample.comm} {sample.cmdline}"
        if any(pattern.search(searchable) for pattern in patterns):
            samples.append(sample)
    return sorted(samples, key=lambda item: item.pid)


def read_loadavg() -> tuple[float, float, float, int, int]:
    fields = read_text(Path("/proc/loadavg")).split()
    runnable, total = fields[3].split("/", 1)
    return float(fields[0]), float(fields[1]), float(fields[2]), int(runnable), int(total)


def vmstat_values() -> dict[str, int]:
    values = parse_key_values(Path("/proc/vmstat"))
    # Kernel versions differ on whether these counters have a zone suffix.
    for prefix in ("pgscan_kswapd", "pgscan_direct", "pgsteal_kswapd", "pgsteal_direct", "allocstall"):
        if prefix not in values:
            values[prefix] = sum(value for key, value in values.items() if key.startswith(prefix + "_"))
    return values


def node_memory() -> Iterable[tuple[int, dict[str, int]]]:
    node_root = Path("/sys/devices/system/node")
    for path in sorted(node_root.glob("node[0-9]*"), key=lambda item: int(item.name[4:])):
        values: dict[str, int] = {}
        try:
            lines = read_text(path / "meminfo").splitlines()
        except (FileNotFoundError, PermissionError):
            lines = []
        # Node meminfo begins each row with "Node N", unlike /proc/meminfo.
        for line in lines:
            fields = line.split()
            if len(fields) < 4:
                continue
            key = fields[2].rstrip(":")
            try:
                item = int(fields[3])
            except ValueError:
                continue
            if len(fields) >= 5 and fields[4].lower() == "kb":
                item *= 1024
            values[key] = item
        yield int(path.name[4:]), values


def value(mapping: dict[str, object], key: str) -> object:
    item = mapping.get(key, "")
    if isinstance(item, float):
        return f"{item:.6f}"
    return item


def format_float(item: float | None) -> str:
    return "" if item is None else f"{item:.6f}"


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def write_metadata(output_dir: Path, args: argparse.Namespace) -> None:
    metadata = {
        "started_at_ns": time.time_ns(),
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "hostname": socket.gethostname(),
        "kernel": platform.release(),
        "platform": platform.platform(),
        "logical_cpus": os.cpu_count(),
        "page_size": PAGE_SIZE,
        "clock_ticks_per_second": CLK_TCK,
        "sample_interval_seconds": args.interval,
        "patterns": args.match,
        "watch_pid": args.watch_pid,
        "idle_exit_seconds": args.idle_exit_seconds,
        "command": sys.argv,
    }
    (output_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")


def monitor(args: argparse.Namespace) -> int:
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    write_metadata(output_dir, args)

    patterns = [re.compile(pattern) for pattern in args.match]
    stopping = False

    def request_stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    system_file = (output_dir / "system.csv").open("w", newline="", buffering=1)
    process_file = (output_dir / "processes.csv").open("w", newline="", buffering=1)
    vmstat_file = (output_dir / "vmstat.csv").open("w", newline="", buffering=1)
    node_file = (output_dir / "numa-memory.csv").open("w", newline="", buffering=1)
    system_writer = csv.DictWriter(system_file, fieldnames=SYSTEM_FIELDS)
    process_writer = csv.DictWriter(process_file, fieldnames=PROCESS_FIELDS)
    vmstat_fields = ["timestamp_ns", "timestamp_utc", "elapsed_s", *VMSTAT_KEYS]
    vmstat_writer = csv.DictWriter(vmstat_file, fieldnames=vmstat_fields)
    node_writer = csv.DictWriter(node_file, fieldnames=NODE_FIELDS)
    for writer in (system_writer, process_writer, vmstat_writer, node_writer):
        writer.writeheader()

    start_mono = time.monotonic()
    previous_cpu: dict[str, int] | None = None
    previous_process: dict[tuple[int, int], tuple[int, float]] = {}
    seen_match = False
    last_match_mono = start_mono
    next_sample = start_mono

    try:
        while not stopping:
            now_mono = time.monotonic()
            if now_mono < next_sample:
                time.sleep(next_sample - now_mono)
            sample_mono = time.monotonic()
            timestamp_ns = time.time_ns()
            timestamp_utc = datetime.fromtimestamp(timestamp_ns / 1e9, timezone.utc).isoformat()
            elapsed = sample_mono - start_mono

            cpu = read_cpu_times()
            cpu_pct = cpu_percentages(cpu, previous_cpu)
            previous_cpu = cpu

            processes = matching_processes(patterns)
            current_process: dict[tuple[int, int], tuple[int, float]] = {}
            for process in processes:
                identity = (process.pid, process.start_ticks)
                old = previous_process.get(identity)
                if old is not None and sample_mono > old[1]:
                    process.cpu_pct = (
                        100.0 * (process.ticks - old[0]) / CLK_TCK / (sample_mono - old[1])
                    )
                current_process[identity] = (process.ticks, sample_mono)
            previous_process = current_process

            if processes:
                seen_match = True
                last_match_mono = sample_mono

            load_1m, load_5m, load_15m, runnable, total_tasks = read_loadavg()
            mem = parse_key_values(Path("/proc/meminfo"))
            cpu_pressure = read_pressure("cpu")
            memory_pressure = read_pressure("memory")
            io_pressure = read_pressure("io")

            system_writer.writerow(
                {
                    "timestamp_ns": timestamp_ns,
                    "timestamp_utc": timestamp_utc,
                    "elapsed_s": f"{elapsed:.6f}",
                    "load_1m": load_1m,
                    "load_5m": load_5m,
                    "load_15m": load_15m,
                    "runnable_tasks": runnable,
                    "total_tasks": total_tasks,
                    "cpu_busy_pct": format_float(cpu_pct["busy"]),
                    "cpu_user_pct": format_float(cpu_pct["user"]),
                    "cpu_system_pct": format_float(cpu_pct["system"]),
                    "cpu_iowait_pct": format_float(cpu_pct["iowait"]),
                    "cpu_idle_pct": format_float(cpu_pct["idle"]),
                    "target_processes": len(processes),
                    "target_cpu_pct": format_float(
                        sum(item.cpu_pct for item in processes if item.cpu_pct is not None)
                    ),
                    "target_rss_bytes": sum(item.rss_bytes for item in processes),
                    "target_vms_bytes": sum(item.vms_bytes for item in processes),
                    "mem_total_bytes": value(mem, "MemTotal"),
                    "mem_available_bytes": value(mem, "MemAvailable"),
                    "mem_free_bytes": value(mem, "MemFree"),
                    "active_bytes": value(mem, "Active"),
                    "inactive_bytes": value(mem, "Inactive"),
                    "anon_pages_bytes": value(mem, "AnonPages"),
                    "file_pages_bytes": value(mem, "Cached"),
                    "dirty_bytes": value(mem, "Dirty"),
                    "writeback_bytes": value(mem, "Writeback"),
                    "swap_total_bytes": value(mem, "SwapTotal"),
                    "swap_free_bytes": value(mem, "SwapFree"),
                    "hugepages_total": value(mem, "HugePages_Total"),
                    "hugepages_free": value(mem, "HugePages_Free"),
                    "hugepages_reserved": value(mem, "HugePages_Rsvd"),
                    "hugepages_surplus": value(mem, "HugePages_Surp"),
                    "hugepage_size_bytes": value(mem, "Hugepagesize"),
                    "cpu_psi_some_avg10": value(cpu_pressure, "some_avg10"),
                    "cpu_psi_some_avg60": value(cpu_pressure, "some_avg60"),
                    "cpu_psi_some_avg300": value(cpu_pressure, "some_avg300"),
                    "cpu_psi_some_total_us": value(cpu_pressure, "some_total"),
                    "memory_psi_some_avg10": value(memory_pressure, "some_avg10"),
                    "memory_psi_some_avg60": value(memory_pressure, "some_avg60"),
                    "memory_psi_some_avg300": value(memory_pressure, "some_avg300"),
                    "memory_psi_some_total_us": value(memory_pressure, "some_total"),
                    "memory_psi_full_avg10": value(memory_pressure, "full_avg10"),
                    "memory_psi_full_avg60": value(memory_pressure, "full_avg60"),
                    "memory_psi_full_avg300": value(memory_pressure, "full_avg300"),
                    "memory_psi_full_total_us": value(memory_pressure, "full_total"),
                    "io_psi_some_avg10": value(io_pressure, "some_avg10"),
                    "io_psi_some_avg60": value(io_pressure, "some_avg60"),
                    "io_psi_some_avg300": value(io_pressure, "some_avg300"),
                    "io_psi_some_total_us": value(io_pressure, "some_total"),
                    "io_psi_full_avg10": value(io_pressure, "full_avg10"),
                    "io_psi_full_avg60": value(io_pressure, "full_avg60"),
                    "io_psi_full_avg300": value(io_pressure, "full_avg300"),
                    "io_psi_full_total_us": value(io_pressure, "full_total"),
                }
            )

            for process in processes:
                process_writer.writerow(
                    {
                        "timestamp_ns": timestamp_ns,
                        "timestamp_utc": timestamp_utc,
                        "elapsed_s": f"{elapsed:.6f}",
                        "pid": process.pid,
                        "ppid": process.ppid,
                        "state": process.state,
                        "threads": process.threads,
                        "cpu_pct": format_float(process.cpu_pct),
                        "rss_bytes": process.rss_bytes,
                        "vms_bytes": process.vms_bytes,
                        "read_bytes": process.read_bytes,
                        "write_bytes": process.write_bytes,
                        "minor_faults": process.minor_faults,
                        "major_faults": process.major_faults,
                        "comm": process.comm,
                        "cmdline": process.cmdline,
                    }
                )

            vm = vmstat_values()
            vmstat_writer.writerow(
                {
                    "timestamp_ns": timestamp_ns,
                    "timestamp_utc": timestamp_utc,
                    "elapsed_s": f"{elapsed:.6f}",
                    **{key: value(vm, key) for key in VMSTAT_KEYS},
                }
            )

            for node, node_mem in node_memory():
                node_writer.writerow(
                    {
                        "timestamp_ns": timestamp_ns,
                        "timestamp_utc": timestamp_utc,
                        "elapsed_s": f"{elapsed:.6f}",
                        "node": node,
                        "mem_total_bytes": value(node_mem, "MemTotal"),
                        "mem_free_bytes": value(node_mem, "MemFree"),
                        "mem_used_bytes": value(node_mem, "MemUsed"),
                        "active_bytes": value(node_mem, "Active"),
                        "inactive_bytes": value(node_mem, "Inactive"),
                        "anon_pages_bytes": value(node_mem, "AnonPages"),
                        "file_pages_bytes": value(node_mem, "FilePages"),
                        "dirty_bytes": value(node_mem, "Dirty"),
                        "writeback_bytes": value(node_mem, "Writeback"),
                        "hugepages_total": value(node_mem, "HugePages_Total"),
                        "hugepages_free": value(node_mem, "HugePages_Free"),
                        "hugepages_surplus": value(node_mem, "HugePages_Surp"),
                    }
                )

            if args.watch_pid is not None and not process_exists(args.watch_pid):
                break
            if (
                args.idle_exit_seconds is not None
                and seen_match
                and not processes
                and sample_mono - last_match_mono >= args.idle_exit_seconds
            ):
                break

            next_sample += args.interval
            if next_sample <= sample_mono:
                next_sample = sample_mono + args.interval
    finally:
        for handle in (system_file, process_file, vmstat_file, node_file):
            handle.close()

    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument(
        "--match",
        action="append",
        required=True,
        help="regular expression matched against process name and full command line; repeatable",
    )
    parser.add_argument("--watch-pid", type=int, help="exit when this process exits")
    parser.add_argument(
        "--idle-exit-seconds",
        type=float,
        help="after seeing a match, exit after this many seconds with no matching process",
    )
    args = parser.parse_args()
    if args.interval <= 0:
        parser.error("--interval must be positive")
    if args.idle_exit_seconds is not None and args.idle_exit_seconds <= 0:
        parser.error("--idle-exit-seconds must be positive")
    return args


if __name__ == "__main__":
    raise SystemExit(monitor(parse_args()))
