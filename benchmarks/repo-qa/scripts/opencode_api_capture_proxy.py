#!/usr/bin/env python3
"""Forward OpenCode provider traffic while recording a secret-safe exchange."""
from __future__ import annotations

import argparse
import hashlib
import http.server
import json
import os
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

SECRET_HEADERS = {"authorization", "cookie", "proxy-authorization", "x-api-key", "api-key"}
HOP_HEADERS = {"connection", "host", "content-length", "transfer-encoding", "accept-encoding"}
SAFE_RESPONSE_HEADERS = {"content-type", "cache-control", "x-request-id", "request-id"}


def sanitized_headers(headers) -> dict[str, str]:
    output = {}
    for key, value in headers.items():
        lowered = key.casefold()
        if lowered in SECRET_HEADERS:
            output[lowered] = "<redacted>"
        elif lowered not in HOP_HEADERS:
            output[lowered] = value
    return dict(sorted(output.items()))


def forward_headers(headers) -> dict[str, str]:
    return {key: value for key, value in headers.items() if key.casefold() not in HOP_HEADERS}


def decode_body(data: bytes):
    try:
        return json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return {"encoding": "utf-8-replacement", "text": data.decode("utf-8", "replace")}


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name, suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as handle:
            json.dump(value, handle, indent=2, sort_keys=True, ensure_ascii=False)
            handle.write("\n")
        os.replace(temporary, path)
    except BaseException:
        try: os.unlink(temporary)
        except FileNotFoundError: pass
        raise


def build_capture(started: int, path: str, request_headers, request_body: bytes,
                  response_status: int, response_headers, response_body: bytes,
                  failure: str | None) -> dict:
    return {
        "schema": "lci.opencode-provider-capture.v1",
        "captured_at_unix_ns": started,
        "request": {
            "method": "POST", "path": path,
            "headers": sanitized_headers(request_headers),
            "body": decode_body(request_body),
            "body_digest": "sha256:" + hashlib.sha256(request_body).hexdigest(),
        },
        "response": {
            "status": response_status,
            "headers": sanitized_headers(response_headers),
            "body": response_body.decode("utf-8", "replace"),
            "body_digest": "sha256:" + hashlib.sha256(response_body).hexdigest(),
        },
        "failure": failure,
    }


class CaptureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, upstream: str, output: Path, max_requests: int = 0):
        super().__init__(address, CaptureHandler)
        self.upstream = upstream.rstrip("/")
        self.output = output
        self.max_requests = max_requests
        self.completed = 0


class CaptureHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        return

    def do_POST(self):
        started = time.time_ns()
        length = int(self.headers.get("Content-Length", "0"))
        request_body = self.rfile.read(length)
        upstream_url = self.server.upstream + self.path
        request = urllib.request.Request(
            upstream_url, data=request_body, method="POST",
            headers=forward_headers(self.headers),
        )
        response_body = bytearray()
        response_status = 502
        response_headers = {}
        failure = None
        try:
            try:
                upstream = urllib.request.urlopen(request, timeout=660)
            except urllib.error.HTTPError as exc:
                upstream = exc
            response_status = upstream.status
            response_headers = dict(upstream.headers.items())
            self.send_response(response_status)
            for key, value in response_headers.items():
                if key.casefold() in SAFE_RESPONSE_HEADERS:
                    self.send_header(key, value)
            self.send_header("Connection", "close")
            self.end_headers()
            while True:
                chunk = upstream.read(65536)
                if not chunk: break
                response_body.extend(chunk)
                self.wfile.write(chunk); self.wfile.flush()
            upstream.close()
        except BaseException as exc:
            failure = f"{type(exc).__name__}: {exc}"
            if not self.wfile.closed:
                try:
                    self.send_error(502, "provider forwarding failed")
                except OSError:
                    pass
        finally:
            capture = build_capture(started, self.path, self.headers, request_body,
                                    response_status, response_headers, bytes(response_body), failure)
            atomic_json(self.server.output / f"capture-{started}.json", capture)
            self.server.completed += 1
            if self.server.max_requests and self.server.completed >= self.server.max_requests:
                threading.Thread(target=self.server.shutdown, daemon=True).start()
            self.close_connection = True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", required=True, help="Provider base URL, for example https://host/v1")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--max-requests", type=int, default=0)
    args = parser.parse_args()
    server = CaptureServer((args.host, args.port), args.upstream, args.out, args.max_requests)
    print(json.dumps({"listen": f"http://{args.host}:{server.server_port}", "upstream": args.upstream}), flush=True)
    try: server.serve_forever()
    finally: server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
