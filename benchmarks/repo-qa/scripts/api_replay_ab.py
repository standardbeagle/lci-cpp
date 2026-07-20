#!/usr/bin/env python3
"""Validate and build paired provider requests from recorded OpenCode traffic."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "benchmarks/repo-qa/api-replay"
ARMS = ("trace_17", "trace_42")


def canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def digest(value: object) -> str:
    text = value if isinstance(value, str) else canonical(value)
    return "sha256:" + hashlib.sha256(text.encode()).hexdigest()


def tool_content_pointer(request: dict, tool_call_id: str) -> tuple[int, str]:
    matches = [
        (index, message["content"])
        for index, message in enumerate(request.get("messages", []))
        if message.get("role") == "tool" and message.get("tool_call_id") == tool_call_id
    ]
    if len(matches) != 1 or not isinstance(matches[0][1], str):
        raise ValueError("request must contain exactly one string tool result for tool_call_id")
    return matches[0]


def render_candidate(source: str, renderer: str) -> str:
    if renderer == "identity-v1":
        return source
    if renderer == "result-envelope-v1":
        return canonical({"result": json.loads(source)})
    raise ValueError(f"unknown renderer: {renderer}")


def invert_candidate(rendered: str, renderer: str) -> str:
    if renderer == "identity-v1":
        return rendered
    value = json.loads(rendered)
    if renderer == "result-envelope-v1" and isinstance(value, dict) and set(value) == {"result"}:
        return canonical(value["result"])
    raise ValueError("candidate is not invertible under renderer")


def build_request(fixture: dict, arm: str) -> dict:
    if arm not in ARMS:
        raise ValueError(f"unknown arm: {arm}")
    request = copy.deepcopy(fixture["request"])
    index, source = tool_content_pointer(request, fixture["tool_call_id"])
    if arm == "trace_42":
        request["messages"][index]["content"] = render_candidate(source, fixture["renderer"])
    return request


def diff_pointers(left: object, right: object, path: str = "") -> list[str]:
    if type(left) is not type(right):
        return [path or "/"]
    if isinstance(left, dict):
        pointers = []
        for key in sorted(set(left) | set(right)):
            child = f"{path}/{key}"
            if key not in left or key not in right:
                pointers.append(child)
            else:
                pointers.extend(diff_pointers(left[key], right[key], child))
        return pointers
    if isinstance(left, list):
        if len(left) != len(right):
            return [path or "/"]
        return [pointer for index, pair in enumerate(zip(left, right)) for pointer in diff_pointers(*pair, f"{path}/{index}")]
    return [] if left == right else [path or "/"]


def validate_fixture(fixture: dict) -> dict:
    if fixture.get("schema") != "lci.api-replay.fixture.v1":
        raise ValueError("wrong fixture schema")
    current = build_request(fixture, "trace_17")
    candidate = build_request(fixture, "trace_42")
    index, source = tool_content_pointer(current, fixture["tool_call_id"])
    _, rendered = tool_content_pointer(candidate, fixture["tool_call_id"])
    if canonical(json.loads(invert_candidate(rendered, fixture["renderer"]))) != canonical(json.loads(source)):
        raise ValueError("candidate does not round-trip to the recorded tool result")
    differences = diff_pointers(current, candidate)
    expected = [] if fixture["null_control"] else [f"/messages/{index}/content"]
    if differences != expected:
        raise ValueError(f"request treatment isolation failed: {differences} != {expected}")
    return {"task_id": fixture["task_id"], "diff_pointers": differences, "current_digest": digest(current), "candidate_digest": digest(candidate)}


def grade_answers(extracted: list[str], expected: list[str]) -> dict:
    norm = lambda values: {" ".join(value.casefold().split()) for value in values}
    predicted, truth = norm(extracted), norm(expected)
    tp = len(predicted & truth)
    return {
        "exact": predicted == truth,
        "precision": tp / len(predicted) if predicted else (1.0 if not truth else 0.0),
        "recall": tp / len(truth) if truth else (1.0 if not predicted else 0.0),
        "false_positives": sorted(predicted - truth),
        "false_negatives": sorted(truth - predicted),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixtures", type=Path, default=BASE / "fixtures")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    fixtures = [json.loads(path.read_text()) for path in sorted(args.fixtures.glob("*.json"))]
    report = {"schema": "lci.api-replay.validation.v1", "fixtures": [validate_fixture(value) for value in fixtures]}
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"fixtures": len(fixtures), "valid": True}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
