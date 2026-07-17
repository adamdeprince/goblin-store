#!/usr/bin/env python3
"""Create the stable URL/size manifest used by the mirror-proxy benchmark."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from urllib.parse import quote


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    directory = args.directory.resolve()
    files = sorted(directory.glob("*.gz"), key=lambda path: path.name)
    if not files:
        raise SystemExit(f"no *.gz objects in {directory}")
    rows = [("/" + quote(path.name, safe=""), path.stat().st_size) for path in files]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("# url_path\tbytes\n")
        for path, size in rows:
            stream.write(f"{path}\t{size}\n")
    temporary.replace(args.output)
    sizes = sorted(size for _, size in rows)
    summary = {
        "objects": len(rows),
        "bytes": sum(sizes),
        "minimum_bytes": sizes[0],
        "median_bytes": sizes[len(sizes) // 2],
        "maximum_bytes": sizes[-1],
        "over_1_gib": sum(size > 1 << 30 for size in sizes),
        "over_2_gib": sum(size > 2 << 30 for size in sizes),
        "over_4_gib": sum(size > 4 << 30 for size in sizes),
    }
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
