#!/usr/bin/env python3
"""Run provider-authentication probes excluded from format-replay outcomes."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent


def _module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ImportError(path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = _module("api_replay_probe_runner", HERE / "api_replay_format_exploration.py")
replay_common = _module("api_replay_probe_replay_common", HERE / "replay_common.py")


def run_probes(provider, fixtures: list[dict]) -> dict:
    records = []
    for fixture in fixtures:
        model = fixture["recorded_model"]
        request = runner.build_request(fixture, "fmt_07")
        response = provider.complete(request, model, runner.fixture_request_headers(fixture))
        records.append({
            "model": model,
            "task_id": fixture["task_id"],
            "fixture_digest": runner.digest(fixture),
            "request_digest": runner.digest(request),
            "status": response.get("status"),
            "http_status": response.get("http_status"),
            "failure": response.get("failure"),
            "raw_provider_stream": response.get("raw_provider_stream", ""),
            "raw_provider_stream_base64": response.get("raw_provider_stream_base64", ""),
            "raw_provider_stream_digest": response.get("raw_provider_stream_digest"),
            "response_headers": response.get("response_headers", {}),
            "usage": response.get("usage"),
            "latency_seconds": response.get("latency_seconds"),
            "excluded_from_outcomes": True,
        })
    return {
        "schema": "lci.api-replay.auth-probes.v1",
        "purpose": "infrastructure-only; excluded from all format outcomes and inference",
        "all_succeeded": all(item["status"] == "answered" and item["http_status"] == 200 for item in records),
        "probes": records,
    }


def write_new(path: Path, value: dict) -> None:
    replay_common.write_immutable(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixtures", nargs="+", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--auth-file", type=Path, default=Path.home() / ".local/share/opencode/auth.json")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()
    provider_module = _module("opencode_zen_probe_provider", HERE / "opencode_zen_provider.py")
    fixtures = [json.loads(path.read_text()) for path in args.fixtures]
    result = run_probes(provider_module.OpenCodeZenProvider(args.auth_file, args.timeout), fixtures)
    write_new(args.out, result)
    print(json.dumps({"all_succeeded": result["all_succeeded"], "probes": len(result["probes"]),
                      "digest": "sha256:" + hashlib.sha256(args.out.read_bytes()).hexdigest()}, sort_keys=True))
    return 0 if result["all_succeeded"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
