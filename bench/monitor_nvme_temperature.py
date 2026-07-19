#!/usr/bin/env python3
"""Log the temperature sensors for one Linux NVMe controller as line-buffered CSV."""

from __future__ import annotations

import argparse
import csv
import signal
import time
from pathlib import Path


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", default="nvme0")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--interval", type=float, default=1.0)
    return parser.parse_args()


def controller_hwmon(controller: str) -> Path:
    target = (Path("/sys/class/nvme") / controller).resolve()
    for hwmon in sorted(Path("/sys/class/hwmon").glob("hwmon*")):
        try:
            if (hwmon / "name").read_text().strip() != "nvme":
                continue
            if (hwmon / "device").resolve() == target:
                return hwmon
        except OSError:
            continue
    raise SystemExit(f"no hwmon temperature source for {controller}")


def main() -> int:
    args = arguments()
    if args.interval <= 0:
        raise SystemExit("--interval must be positive")

    hwmon = controller_hwmon(args.controller)
    model = (Path("/sys/class/nvme") / args.controller / "model").read_text().strip()
    inputs = sorted(hwmon.glob("temp*_input"))
    if not inputs:
        raise SystemExit(f"no temperature inputs below {hwmon}")

    stopping = False

    def stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    start_ns = time.monotonic_ns()
    next_sample = time.monotonic()
    with args.output.open("w", newline="", buffering=1) as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "timestamp_ns",
                "monotonic_ns",
                "elapsed_s",
                "controller",
                "model",
                "sensor",
                "temperature_millicelsius",
                "temperature_celsius",
            ]
        )
        while not stopping:
            wall_ns = time.time_ns()
            monotonic_ns = time.monotonic_ns()
            for input_path in inputs:
                stem = input_path.name.removesuffix("_input")
                label_path = hwmon / f"{stem}_label"
                label = label_path.read_text().strip() if label_path.exists() else stem
                millidegrees = int(input_path.read_text().strip())
                writer.writerow(
                    [
                        wall_ns,
                        monotonic_ns,
                        f"{(monotonic_ns - start_ns) / 1e9:.9f}",
                        args.controller,
                        model,
                        label,
                        millidegrees,
                        f"{millidegrees / 1000:.3f}",
                    ]
                )
            next_sample += args.interval
            time.sleep(max(0.0, next_sample - time.monotonic()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
