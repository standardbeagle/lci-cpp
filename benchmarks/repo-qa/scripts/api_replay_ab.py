#!/usr/bin/env python3
"""Validate and build paired provider requests from recorded OpenCode traffic."""
from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from replay_common import (  # noqa: E402  (path bootstrap must precede the import)
    SAFE_REQUEST_HEADERS,
    canonical,
    diff_pointers,
    digest,
    tool_content_pointer,
)

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "benchmarks/repo-qa/api-replay"
ARMS = ("trace_17", "trace_42")


def captured_request_headers(headers: object) -> dict[str, str]:
    """Extract only replay-safe headers from a sanitized capture.

    Capture artifacts may contain redacted placeholders for credentials.  Those
    placeholders must never become fixture data or outgoing provider headers.
    """
    if not isinstance(headers, dict):
        raise ValueError("capture request headers must be an object")
    output: dict[str, str] = {}
    for key, value in headers.items():
        lowered = str(key).casefold()
        if lowered in SAFE_REQUEST_HEADERS:
            if not isinstance(value, str) or "\r" in value or "\n" in value:
                raise ValueError(f"invalid safe capture request header: {lowered}")
            output[lowered] = value
    return dict(sorted(output.items()))


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
    """Score extracted answers against the oracle under the harness-wide IR convention.

    Standard information-retrieval convention, shared with
    api_replay_analysis.quality so the two never disagree on the same cell:
    an empty side scores 1.0 only when the other side is empty too, so neither
    silence nor fabrication can outscore a correct answer.
    """
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


def fixture_from_capture(capture: dict, specification: dict) -> dict:
    if capture.get("schema") != "lci.opencode-provider-capture.v1":
        raise ValueError("wrong capture schema")
    if capture.get("failure") is not None or capture.get("response", {}).get("status") != 200:
        raise ValueError("capture must be a successful provider exchange")
    request = capture.get("request", {}).get("body")
    if not isinstance(request, dict):
        raise ValueError("capture request body must be JSON")
    tool_messages = [message for message in request.get("messages", []) if message.get("role") == "tool"]
    if len(tool_messages) != 1:
        raise ValueError("capture must contain exactly one tool result")
    tool_call_id = tool_messages[0].get("tool_call_id")
    calls = [call for message in request["messages"] if message.get("role") == "assistant" for call in message.get("tool_calls", [])]
    matching = [call for call in calls if call.get("id") == tool_call_id]
    if len(matching) != 1 or matching[0].get("function", {}).get("name") != "lci_" + specification["tool"]:
        raise ValueError("assistant tool call does not match the selected LCI tool result")
    fixture = {
        "schema": "lci.api-replay.fixture.v1",
        "capture_kind": "sanitized_live_proxy",
        "capture_note": "Captured from a genuine OpenCode provider continuation after an LCI tool call.",
        "provider_api": specification["provider_api"],
        "recorded_model": specification["recorded_model"],
        "opencode_version": specification["opencode_version"],
        "corpus": specification["corpus"],
        "corpus_commit": specification["corpus_commit"],
        "tool": specification["tool"],
        "task_id": specification["task_id"],
        "question": specification["question"],
        "expected_answers": specification["expected_answers"],
        "tool_call_id": tool_call_id,
        "request": request,
        "request_headers": captured_request_headers(capture.get("request", {}).get("headers")),
        "null_control": specification["null_control"],
        "renderer": specification["renderer"],
        "capture_request_digest": capture["request"]["body_digest"],
        "capture_response_digest": capture["response"]["body_digest"],
    }
    validate_fixture(fixture)
    return fixture


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixtures", type=Path, default=BASE / "fixtures")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--promote-capture", type=Path)
    parser.add_argument("--capture-spec", type=Path)
    parser.add_argument("--promote-out", type=Path)
    args = parser.parse_args()
    if args.promote_capture:
        if not args.capture_spec or not args.promote_out:
            parser.error("--promote-capture requires --capture-spec and --promote-out")
        fixture = fixture_from_capture(json.loads(args.promote_capture.read_text()), json.loads(args.capture_spec.read_text()))
        args.promote_out.parent.mkdir(parents=True, exist_ok=True)
        args.promote_out.write_text(json.dumps(fixture, indent=2, sort_keys=True, ensure_ascii=False) + "\n")
        print(json.dumps({"promoted": str(args.promote_out), "valid": True}, sort_keys=True))
        return 0
    fixtures = [json.loads(path.read_text()) for path in sorted(args.fixtures.glob("*.json"))]
    report = {"schema": "lci.api-replay.validation.v1", "fixtures": [validate_fixture(value) for value in fixtures]}
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"fixtures": len(fixtures), "valid": True}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
