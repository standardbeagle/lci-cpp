#!/usr/bin/env python3
"""Build and execute derived OpenCode continuation replays for every LCI tool."""
from __future__ import annotations

import argparse
import copy
import json
import hashlib
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import api_replay_format_exploration as replay
from replay_common import canonical, diff_pointers, digest, issuing_assistant, tool_content_pointer


def task_question(case: dict, surface_case: dict) -> str:
    if case["scenario"] == "success":
        return surface_case["question"]
    return (f"Did the {case['tool']} call produce the requested result? "
            "State whether it returned an error, an empty/not-found result, or usable data, "
            "and report any corrective guidance without inventing results.")


def derive_fixture(base: dict, case: dict, surface_case: dict) -> dict:
    fixture = copy.deepcopy(base)
    fixture["schema"] = "lci.api-replay.fixture.v1"
    fixture["capture_kind"] = "derived_from_sanitized_live_proxy"
    fixture["task_id"] = f"{case['tool']}--{case['scenario']}"
    fixture["tool"] = case["tool"]
    fixture["scenario"] = case["scenario"]
    fixture["tool_content_kind"] = "json" if case["output"].lstrip().startswith(("{", "[")) else "text_protocol"
    fixture["expected_answers"] = surface_case["answer"] if case["scenario"] == "success" else []
    messages = fixture["request"]["messages"]
    tool_index, _ = tool_content_pointer(fixture["request"], fixture["tool_call_id"])
    assistant = issuing_assistant(messages, tool_index)
    user_index = max(i for i in range(tool_index) if messages[i].get("role") == "user")
    arguments = canonical(case["arguments"])
    question = task_question(case, surface_case)
    messages[user_index]["content"] = (f'"Call lci_{case["tool"]} exactly once with only {arguments}. '
                                       f'Do not retry or call any other tool. Then answer: {question}"\n')
    assistant["content"] = ""
    assistant["reasoning_content"] = "I will make the requested single tool call and answer from its result."
    call = assistant["tool_calls"][0]
    call["function"] = {"name": f"lci_{case['tool']}", "arguments": arguments}
    messages[tool_index]["content"] = case["output"]
    return fixture


def build_fixtures(matrix: dict, surface: dict, bases: list[dict]) -> list[dict]:
    by_tool = {item["name"]: item for item in surface["tools"]}
    fixtures = [derive_fixture(base, case, by_tool[case["tool"]])
                for case in matrix["cases"] for base in bases]
    if len(fixtures) != matrix["case_count"] * len(bases):
        raise ValueError("fixture grid is incomplete")
    return fixtures


def jobs(fixtures: list[dict], repetitions: int = 2) -> list[tuple]:
    by_task_model = {(f["task_id"], f["recorded_model"]): f for f in fixtures}
    tasks = sorted({f["task_id"] for f in fixtures})
    models = sorted({f["recorded_model"] for f in fixtures})
    output = []
    row_index = 0
    arms = list(replay.ARMS)
    for repetition in range(1, repetitions + 1):
        for task in tasks:
            row = arms[row_index % len(arms):] + arms[:row_index % len(arms)]
            model_order = models if row_index % 2 == 0 else list(reversed(models))
            for model in model_order:
                fixture = by_task_model[(task, model)]
                output.extend((fixture, model, arm, repetition, order)
                              for order, arm in enumerate(row, 1))
            row_index += 1
    return output


def preflight(fixtures: list[dict], planned: list[tuple]) -> dict:
    checks = []
    for fixture in fixtures:
        # Derived fixtures must satisfy the same validation contract as live
        # captures; validate_fixture accepts the derived capture_kind explicitly.
        replay.validate_fixture(fixture)
        identity = replay.build_request(fixture, replay.ARMS[0])
        index, source = tool_content_pointer(identity, fixture["tool_call_id"])
        for arm in replay.ARMS:
            request = replay.build_request(fixture, arm)
            _, rendered = tool_content_pointer(request, fixture["tool_call_id"])
            recovered = replay.transform(
                source, arm, allow_text=fixture.get("tool_content_kind") == "text_protocol")[1]
            expected = [] if arm == replay.ARMS[0] else [f"/messages/{index}/content"]
            actual = diff_pointers(identity, request)
            if actual != expected:
                raise ValueError(f"request isolation failure: {fixture['task_id']} {arm}")
            checks.append({"task_id": fixture["task_id"], "model": fixture["recorded_model"],
                           "arm": arm, "diff_pointers": actual,
                           "canonical_digest": digest(recovered),
                           "content_digest": digest(rendered)})
    return {"schema": "lci.api-replay.all-tools-preflight.v1", "provider_executed": False,
            "fixture_count": len(fixtures), "planned_cells": len(planned),
            "all_equivalent": True, "checks": checks}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--surface", type=Path, required=True)
    parser.add_argument("--base-fixture", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--preflight", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--run-provider", action="store_true")
    parser.add_argument("--auth-file", type=Path)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    matrix = json.loads(args.matrix.read_text())
    manifest = json.loads(args.manifest.read_text())
    actual_matrix = "sha256:" + hashlib.sha256(args.matrix.read_bytes()).hexdigest()
    if manifest.get("frozen_inputs", {}).get("tool_output_matrix") != actual_matrix:
        raise ValueError("frozen tool-output matrix digest mismatch")
    surface = json.loads(args.surface.read_text())
    bases = [json.loads(path.read_text()) for path in args.base_fixture]
    fixtures = build_fixtures(matrix, surface, bases)
    planned = jobs(fixtures)
    report = preflight(fixtures, planned)
    args.preflight.parent.mkdir(parents=True, exist_ok=True)
    args.preflight.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if args.run_provider:
        if manifest.get("provider_execution_allowed") is not True or not re.fullmatch(
                r"[0-9a-f]{40}", str(manifest.get("analysis_revision", ""))):
            raise RuntimeError("provider execution remains locked until a frozen analysis revision")
        if args.auth_file is None:
            raise ValueError("--auth-file is required for provider execution")
        from opencode_zen_provider import OpenCodeZenProvider
        provider = OpenCodeZenProvider(args.auth_file, args.timeout)
        records, counts = replay.run_provider_grid(provider, planned, args.out)
        report.update(provider_executed=True, **counts, recorded=len(records))
    print(json.dumps({key: report[key] for key in report if key != "checks"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
