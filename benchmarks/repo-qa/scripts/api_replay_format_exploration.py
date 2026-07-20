#!/usr/bin/env python3
"""Hermetic four-arm builder for exploratory captured-request format replays.

This module deliberately contains no network provider implementation. Callers must
inject a provider and explicitly set ``allow_provider=True`` to execute cells.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
from types import ModuleType
from typing import Protocol

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "benchmarks/repo-qa/api-replay"
CANDIDATES = BASE / "candidates"
DEFAULT_OUT = BASE / "format-exploration/results"
DEFAULT_MANIFEST = BASE / "format-exploration/manifest.json"
DEFAULT_SCHEDULE = BASE / "format-exploration/schedule.json"

# Opaque labels prevent result records and execution order from advertising a
# presumed baseline or preferred treatment.
ARM_CODECS = {
    "fmt_07": None,
    "fmt_19": "readable_xml_v1.py",
    "fmt_31": "path_records_v1.py",
    "fmt_43": "tagged_blocks_v1.py",
}
ARMS = tuple(ARM_CODECS)


def canonical(value: object) -> str:
    return json.dumps(value, ensure_ascii=True, allow_nan=False, sort_keys=True, separators=(",", ":"))


def digest(value: object) -> str:
    text = value if isinstance(value, str) else canonical(value)
    return "sha256:" + hashlib.sha256(text.encode("utf-8", errors="surrogatepass")).hexdigest()


def _load_codec(filename: str) -> ModuleType:
    path = CANDIDATES / filename
    spec = importlib.util.spec_from_file_location("lci_format_" + path.stem, path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load codec: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not callable(getattr(module, "encode", None)) or not callable(getattr(module, "decode", None)):
        raise ValueError(f"invalid codec interface: {path}")
    return module


def tool_content_pointer(request: dict, tool_call_id: str) -> tuple[int, str]:
    found = [
        (index, message.get("content"))
        for index, message in enumerate(request.get("messages", []))
        if message.get("role") == "tool" and message.get("tool_call_id") == tool_call_id
    ]
    if len(found) != 1 or not isinstance(found[0][1], str):
        raise ValueError("request must contain exactly one string result for the selected tool call")
    return found[0]


def diff_pointers(left: object, right: object, path: str = "") -> list[str]:
    if type(left) is not type(right):
        return [path or "/"]
    if isinstance(left, dict):
        result: list[str] = []
        for key in sorted(set(left) | set(right)):
            child = f"{path}/{str(key).replace('~', '~0').replace('/', '~1')}"
            result.extend([child] if key not in left or key not in right else diff_pointers(left[key], right[key], child))
        return result
    if isinstance(left, list):
        if len(left) != len(right):
            return [path or "/"]
        return [item for index, pair in enumerate(zip(left, right)) for item in diff_pointers(*pair, f"{path}/{index}")]
    return [] if left == right else [path or "/"]


def transform(source: str, arm: str) -> tuple[str, object]:
    if arm not in ARM_CODECS:
        raise ValueError(f"unknown arm: {arm}")
    value = json.loads(source)
    filename = ARM_CODECS[arm]
    if filename is None:
        return source, value
    codec = _load_codec(filename)
    rendered = codec.encode(copy.deepcopy(value))
    if not isinstance(rendered, str):
        raise ValueError("codec returned non-string content")
    recovered = codec.decode(rendered)
    if canonical(recovered) != canonical(value):
        raise ValueError("codec inverse does not recover the canonical source value")
    return rendered, recovered


def build_request(fixture: dict, arm: str) -> dict:
    request = copy.deepcopy(fixture["request"])
    index, source = tool_content_pointer(request, fixture["tool_call_id"])
    request["messages"][index]["content"] = transform(source, arm)[0]
    return request


def validate_fixture(fixture: dict) -> dict:
    if fixture.get("schema") != "lci.api-replay.fixture.v1":
        raise ValueError("wrong fixture schema")
    if not isinstance(fixture.get("recorded_model"), str) or not fixture["recorded_model"]:
        raise ValueError("fixture must name its recorded model")
    if fixture.get("capture_kind") != "sanitized_live_proxy":
        raise ValueError("fixture must originate from a sanitized live provider capture")
    native_model = fixture.get("request", {}).get("model")
    if native_model != fixture["recorded_model"].split("/", 1)[-1]:
        raise ValueError("captured request model does not match recorded_model")
    identity = build_request(fixture, ARMS[0])
    index, source = tool_content_pointer(identity, fixture["tool_call_id"])
    source_value = json.loads(source)
    arms = {}
    for arm in ARMS:
        request = build_request(fixture, arm)
        _, rendered = tool_content_pointer(request, fixture["tool_call_id"])
        _, recovered = transform(source, arm)
        if canonical(recovered) != canonical(source_value):
            raise ValueError(f"canonical inverse mismatch for {arm}")
        differences = diff_pointers(identity, request)
        expected = [] if arm == ARMS[0] else [f"/messages/{index}/content"]
        if differences != expected:
            raise ValueError(f"request isolation failed for {arm}: {differences} != {expected}")
        arms[arm] = {"content_digest": digest(rendered), "request_digest": digest(request), "diff_pointers": differences}
    return {"task_id": fixture["task_id"], "recorded_model": fixture["recorded_model"], "fixture_digest": digest(fixture), "arms": arms}


def validate_protocol(manifest: dict, schedule: dict) -> None:
    if manifest.get("benchmark_id") != schedule.get("benchmark_id"):
        raise ValueError("manifest and schedule benchmark ids differ")
    manifest_arms = tuple(arm["id"] for arm in manifest.get("arms", []))
    if manifest_arms != ARMS:
        raise ValueError(f"manifest arms do not match runner arms: {manifest_arms} != {ARMS}")
    models = [model["id"] for model in manifest.get("models", [])]
    blocks = schedule.get("blocks", [])
    rows = schedule.get("arm_order_rows", [])
    if len(blocks) != manifest.get("repetitions") or len(blocks) != manifest.get("schedule", {}).get("blocks_per_model"):
        raise ValueError("manifest repetition count does not match schedule blocks")
    if any(sorted(row) != sorted(ARMS) for row in rows):
        raise ValueError("every schedule row must contain each declared arm exactly once")
    expected_blocks = list(range(1, len(blocks) + 1))
    if [block.get("block") for block in blocks] != expected_blocks:
        raise ValueError("schedule blocks must be consecutive and ordered")
    for block in blocks:
        if sorted(block.get("model_order", [])) != sorted(models):
            raise ValueError("every block must contain every declared model exactly once")
        row = block.get("arm_order_row")
        if not isinstance(row, int) or not 1 <= row <= len(rows):
            raise ValueError("invalid arm_order_row")
    total = len(blocks) * len(models) * len(ARMS)
    if total != schedule.get("total_cells") or total != manifest.get("schedule", {}).get("total_planned_cells"):
        raise ValueError("declared total cell count does not match the schedule")
    for position in range(len(ARMS)):
        counts = {arm: sum(rows[block["arm_order_row"] - 1][position] == arm for block in blocks) for arm in ARMS}
        if len(set(counts.values())) != 1:
            raise ValueError(f"schedule is not position-balanced at position {position + 1}")


def validate_frozen_files(manifest: dict, manifest_path: Path = DEFAULT_MANIFEST) -> None:
    for relative, expected in manifest.get("fixture_digests", {}).items():
        path = (manifest_path.parent / relative).resolve()
        actual = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"frozen file digest mismatch: {relative}")


def planned_grid(fixtures: list[dict], manifest: dict, schedule: dict) -> list[tuple[dict, str, str, int, int]]:
    validate_protocol(manifest, schedule)
    by_model_task = {(f["recorded_model"], f["task_id"]): f for f in fixtures}
    if len(by_model_task) != len(fixtures):
        raise ValueError("duplicate model/task fixture")
    tasks = sorted({f["task_id"] for f in fixtures})
    if len(tasks) != 1:
        raise ValueError("this preregistration requires exactly one shared task id across per-model captures")
    task_id = tasks[0]
    jobs = []
    rows = schedule["arm_order_rows"]
    for block in schedule["blocks"]:
        for model in block["model_order"]:
            fixture = by_model_task.get((model, task_id))
            if fixture is None:
                raise ValueError(f"missing genuine captured fixture for model={model!r}, task={task_id!r}")
            validate_fixture(fixture)
            row = rows[block["arm_order_row"] - 1]
            jobs.extend((fixture, model, arm, block["block"], order) for order, arm in enumerate(row, 1))
    return jobs


def cell_identity(fixture: dict, model: str, arm: str, repetition: int, order: int) -> dict:
    if model != fixture.get("recorded_model"):
        raise ValueError("model does not match the model that produced the captured request")
    request = build_request(fixture, arm)
    raw = {"task_id": fixture["task_id"], "model": model, "arm": arm, "repetition": repetition}
    return {
        "schema": "lci.api-replay.format-cell.v1",
        **raw,
        "order": order,
        "cell_key": hashlib.sha256(canonical(raw).encode()).hexdigest(),
        "fixture_digest": digest(fixture),
        "request_digest": digest(request),
    }


def write_immutable(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = (json.dumps(record, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode()
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    except FileExistsError as error:
        raise RuntimeError(f"refusing to replace immutable cell record: {path}") from error
    with os.fdopen(descriptor, "wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


class Provider(Protocol):
    def complete(self, request: dict, model: str) -> dict: ...


RETRYABLE_STATUSES = {"provider_timeout", "provider_quota", "provider_5xx", "transport_error"}
MAX_ATTEMPTS = 3


def prior_attempts(out: Path, cell_key: str) -> list[tuple[Path, dict]]:
    records = []
    for path in sorted((out / "attempts").glob(f"{cell_key}.attempt-*.json")):
        try:
            record = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise RuntimeError(f"corrupt immutable attempt record: {path}") from error
        records.append((path, record))
    return records


def cell_run_state(out: Path, identity: dict) -> str:
    cell_key = identity["cell_key"]
    final_path = out / f"{cell_key}.json"
    attempts = prior_attempts(out, cell_key)
    for number, (path, record) in enumerate(attempts, 1):
        if path.name != f"{cell_key}.attempt-{number:03d}.json" or record.get("attempt") != number:
            raise RuntimeError(f"non-consecutive immutable attempt history: {path}")
        if record.get("cell_key") != cell_key:
            raise RuntimeError(f"attempt identity mismatch: {path}")
    answered = [(path, record) for path, record in attempts if record.get("status") == "answered"]
    if len(answered) > 1:
        raise RuntimeError("multiple answered attempts exist for one logical cell")
    if final_path.exists():
        try:
            final = json.loads(final_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise RuntimeError(f"corrupt immutable cell record: {final_path}") from error
        if not answered or final.get("status") != "answered" or final.get("cell_key") != cell_key:
            raise RuntimeError("final cell record is not backed by exactly one answered attempt")
        if final.get("bound_attempt") != answered[0][1].get("attempt"):
            raise RuntimeError("final cell binding does not match answered attempt")
        return "bound"
    if answered:
        write_immutable(final_path, {**answered[0][1], "bound_attempt": answered[0][1]["attempt"]})
        return "bound"
    if not attempts:
        return "missing"
    if len(attempts) >= MAX_ATTEMPTS:
        return "exhausted"
    return "retryable" if attempts[-1][1].get("status") in RETRYABLE_STATUSES else "terminal"


def run_provider_grid(provider: Provider, jobs: list[tuple[dict, str, str, int, int]], out: Path) -> tuple[list[dict], dict]:
    records: list[dict] = []
    skipped_keys: set[str] = set()
    executed = 0
    retried = 0
    for job in jobs:
        identity = cell_identity(*job)
        state = cell_run_state(out, identity)
        if state == "missing":
            records.append(execute_cell(provider, *job, out, allow_provider=True))
            executed += 1
        elif state in {"bound", "exhausted", "terminal"}:
            skipped_keys.add(identity["cell_key"])
    for _replacement_round in range(2):
        eligible = []
        for job in jobs:
            identity = cell_identity(*job)
            if cell_run_state(out, identity) == "retryable":
                eligible.append(job)
        if not eligible:
            break
        for job in eligible:
            records.append(execute_cell(provider, *job, out, allow_provider=True))
            executed += 1
            retried += 1
    for job in jobs:
        identity = cell_identity(*job)
        if cell_run_state(out, identity) in {"bound", "exhausted", "terminal"} and not any(
                record["cell_key"] == identity["cell_key"] for record in records):
            skipped_keys.add(identity["cell_key"])
    return records, {"executed": executed, "retried": retried, "skipped": len(skipped_keys)}


def execute_cell(provider: Provider | None, fixture: dict, model: str, arm: str, repetition: int, order: int, out: Path, *, allow_provider: bool = False) -> dict:
    if not allow_provider:
        raise RuntimeError("provider execution is disabled by default; explicit allow_provider=True is required")
    if provider is None:
        raise ValueError("an injected provider is required")
    identity = cell_identity(fixture, model, arm, repetition, order)
    record_path = out / f"{identity['cell_key']}.json"
    if record_path.exists():
        raise RuntimeError(f"refusing to replace immutable cell record: {record_path}")
    attempts = prior_attempts(out, identity["cell_key"])
    if any(record.get("status") == "answered" for _, record in attempts):
        raise RuntimeError("refusing to retry an answered cell")
    if attempts and attempts[-1][1].get("status") not in RETRYABLE_STATUSES:
        raise RuntimeError("refusing to retry a non-retryable provider attempt")
    if len(attempts) >= MAX_ATTEMPTS:
        raise RuntimeError("provider attempt limit reached")
    attempt_number = len(attempts) + 1
    response = provider.complete(build_request(fixture, arm), model)
    status = response.get("status")
    if status not in {"answered", "provider_timeout", "provider_quota", "provider_5xx", "transport_error", "provider_error", "malformed_provider_stream", "empty_answer"}:
        raise ValueError(f"provider returned unknown status: {status!r}")
    record = {**identity, "attempt": attempt_number, "status": status, "final_answer": response.get("final_answer", ""), "failure": response.get("failure"),
              "http_status": response.get("http_status"), "response_headers": response.get("response_headers", {}),
              "raw_provider_stream": response.get("raw_provider_stream", ""), "usage": response.get("usage"),
              "raw_provider_stream_base64": response.get("raw_provider_stream_base64", ""),
              "raw_provider_stream_digest": response.get("raw_provider_stream_digest"),
              "latency_seconds": response.get("latency_seconds")}
    attempt_path = out / "attempts" / f"{identity['cell_key']}.attempt-{attempt_number:03d}.json"
    write_immutable(attempt_path, record)
    if status == "answered":
        write_immutable(record_path, {**record, "bound_attempt": attempt_number})
    return record


def require_frozen_analysis_revision(manifest: dict) -> None:
    revision = manifest.get("analysis_revision")
    if not isinstance(revision, str) or not re.fullmatch(r"(?:git:)?[0-9a-f]{40}", revision):
        raise RuntimeError("provider execution prohibited until analysis_revision is a frozen git commit")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixtures", nargs="+", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--schedule", type=Path, default=DEFAULT_SCHEDULE)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--run-provider", action="store_true")
    parser.add_argument("--auth-file", type=Path, default=Path.home() / ".local/share/opencode/auth.json")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--plan-out", type=Path)
    args = parser.parse_args()
    fixtures = [json.loads(path.read_text(encoding="utf-8")) for path in args.fixtures]
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    schedule = json.loads(args.schedule.read_text(encoding="utf-8"))
    validate_frozen_files(manifest, args.manifest)
    jobs = planned_grid(fixtures, manifest, schedule)
    if args.run_provider:
        require_frozen_analysis_revision(manifest)
        from opencode_zen_provider import OpenCodeZenProvider
        provider = OpenCodeZenProvider(args.auth_file, args.timeout)
        records, counts = run_provider_grid(provider, jobs, args.out)
        report = {"schema": "lci.api-replay.format-plan.v1", "provider_executed": True, "cells": len(jobs),
                  "recorded": len(records), **counts, "fixtures": [validate_fixture(f) for f in fixtures]}
    else:
        report = {"schema": "lci.api-replay.format-plan.v1", "provider_executed": False, "cells": len(jobs), "fixtures": [validate_fixture(f) for f in fixtures]}
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.plan_out:
        args.plan_out.parent.mkdir(parents=True, exist_ok=True)
        args.plan_out.write_text(rendered, encoding="utf-8")
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
