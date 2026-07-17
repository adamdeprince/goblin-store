#!/usr/bin/env python3
"""Serve a cacheable, fixed corpus to the mirror-proxy benchmark.

This deliberately remains a small Python HTTP origin.  Every accepted request is appended to a
local access log so the controller can prove that a warmed benchmark generated no origin traffic.
Only top-level ``*.gz`` objects are exposed.
"""

from __future__ import annotations

import argparse
import http.server
import json
import os
from pathlib import Path
import threading
import time
from urllib.parse import unquote, urlsplit


class CorpusHandler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "GoblinMirrorOrigin/1"

    def __init__(self, *args: object, directory: str, access_log: Path, **kwargs: object) -> None:
        self._access_log = access_log
        super().__init__(*args, directory=directory, **kwargs)

    def _allowed(self) -> bool:
        path = unquote(urlsplit(self.path).path)
        name = path.removeprefix("/")
        return bool(name) and "/" not in name and name.endswith(".gz")

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if not self._allowed():
            self.send_error(404)
            return
        super().do_GET()

    def do_HEAD(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if not self._allowed():
            self.send_error(404)
            return
        super().do_HEAD()

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "public, max-age=604800, immutable")
        super().end_headers()

    def log_request(self, code: int | str = "-", size: int | str = "-") -> None:
        row = {
            "time_ns": time.time_ns(),
            "client": self.client_address[0],
            "method": self.command,
            "target": self.path,
            "status": int(code) if isinstance(code, int) else code,
            "reported_bytes": size,
        }
        line = json.dumps(row, separators=(",", ":")) + "\n"
        with self.server.log_lock:  # type: ignore[attr-defined]
            self.server.log_stream.write(line)  # type: ignore[attr-defined]
            self.server.log_stream.flush()  # type: ignore[attr-defined]

    def log_message(self, format: str, *args: object) -> None:
        # log_request() above is the durable record.  Keep the multi-terabyte warm quiet.
        del format, args


class OriginServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128

    def __init__(self, address: tuple[str, int], directory: Path, access_log: Path) -> None:
        self.log_lock = threading.Lock()
        access_log.parent.mkdir(parents=True, exist_ok=True)
        self.log_stream = access_log.open("a", encoding="utf-8", buffering=1)

        def handler(*args: object, **kwargs: object) -> CorpusHandler:
            return CorpusHandler(
                *args,
                directory=os.fspath(directory),
                access_log=access_log,
                **kwargs,
            )

        super().__init__(address, handler)

    def server_close(self) -> None:
        try:
            super().server_close()
        finally:
            self.log_stream.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--access-log", required=True, type=Path)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18000)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    directory = args.directory.resolve()
    if not directory.is_dir():
        raise SystemExit(f"not a directory: {directory}")
    objects = list(directory.glob("*.gz"))
    if not objects:
        raise SystemExit(f"no *.gz objects in {directory}")
    server = OriginServer((args.bind, args.port), directory, args.access_log)
    print(
        f"origin listening on {args.bind}:{args.port}; "
        f"directory={directory}; objects={len(objects)}",
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
