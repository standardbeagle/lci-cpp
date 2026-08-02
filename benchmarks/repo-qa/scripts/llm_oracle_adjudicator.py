#!/usr/bin/env python3
"""Blindly adjudicate every deterministic-oracle miss and emit regression cases."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from adjudicator_common import load_header_profile, request_body as build_request_body, strip_code_fence, write_checkpoint as write_checkpoint_document
from opencode_zen_provider import OpenCodeZenProvider
from semantic_location_oracle import adjudication_prompt, evaluate, validate_adjudication


def parse_json_answer(answer: str) -> dict:
    value = json.loads(strip_code_fence(answer))
    if not isinstance(value, dict):
        raise ValueError("adjudicator output is not an object")
    return value


def provisional_failures(cells: list[dict], expected_by_model: dict[str, list[str]]) -> list[dict]:
    failures = []
    for cell in cells:
        if cell.get("status") != "answered":
            continue
        expected = expected_by_model[cell["model"]]
        result = evaluate(cell["final_answer"], expected)
        if result["status"] == "needs_adjudication":
            failures.append({"run_key": cell.get("run_key") or cell.get("cell_key"),
                             "task_id": cell["task_id"], "answer": cell["final_answer"],
                             "expected": expected, "heuristic": result})
    return failures


def request_body(model: str, prompt: str) -> dict:
    return build_request_body(model, prompt, system="You are a strict, blinded benchmark adjudicator.")


def item_key(item: dict) -> str:
    """Resume key for an input item or an already-written record.

    run_key is optional in the failures input, so fall back to the pair the two
    shapes share: the task id and the digest of the answer under adjudication.
    """
    if item.get("run_key"):
        return str(item["run_key"])
    digest = item.get("answer_digest")
    if digest is None:
        digest = "sha256:" + hashlib.sha256(item["answer"].encode()).hexdigest()
    return f"{item['task_id']}|{digest}"


def write_checkpoint(path: Path, records: list[dict]) -> None:
    write_checkpoint_document(path, {
        "schema": "lci.oracle-adjudications.v1", "policy": "every-provisional-failure",
        "records": records,
        "promotion_policy": "Regression candidates require deterministic rule implementation plus positive and adversarial tests; LLM verdicts never directly override primary scores."})


def adjudicate_item(item: dict, complete, *, model: str, headers: dict,
                    max_format_attempts: int = 3) -> dict:
    """Retry malformed structure only; never retry or reinterpret a valid verdict."""
    prompt = adjudication_prompt(task=item["task_id"], answer=item["answer"], expected=item["expected"])
    record = {"run_key": item.get("run_key"), "task_id": item["task_id"],
              "answer_digest": "sha256:" + hashlib.sha256(item["answer"].encode()).hexdigest(),
              "adjudicator_model": model,
              "prompt_digest": "sha256:" + hashlib.sha256(prompt.encode()).hexdigest(),
              "provider_status": None, "adjudication": None, "regression_candidate": None}
    parse_error = None
    for attempt in range(1, max_format_attempts + 1):
        response = complete(request_body(model, prompt), model, headers)
        record["provider_status"] = response["status"]
        if response["status"] != "answered":
            record["failure"] = response.get("failure")
            return record
        try:
            judgment = validate_adjudication(parse_json_answer(response["final_answer"]), item["expected"])
        except (ValueError, TypeError, KeyError, json.JSONDecodeError) as error:
            parse_error = {"kind": "malformed_adjudication", "attempts": attempt, "error": str(error)}
            continue
        record["adjudication"] = judgment
        if judgment["verdict"] == "correct":
            record["regression_candidate"] = {
                "answer": item["answer"], "expected": item["expected"],
                "required_outcome": "accepted", "proposed_gap": judgment.get("heuristic_gap"),
            }
        return record
    # A judge that cannot emit valid structure is a recorded failure, not a crash.
    record["failure"] = parse_error
    return record


def drain(items: list[dict], records: list[dict], adjudicate, checkpoint) -> list[dict]:
    """Adjudicate every item not yet carrying a real verdict.

    A checkpointed record whose adjudication is null is a provider or format
    failure, not a completed judgment: it must be replaced on resume, never
    skipped, or a transient outage silently shrinks the adjudicated set.
    """
    completed = {item_key(record) for record in records if record.get("adjudication") is not None}
    for item in items:
        key = item_key(item)
        if key in completed:
            continue
        records = [record for record in records if item_key(record) != key]
        records.append(adjudicate(item))
        checkpoint(records)
    return records


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--failures", type=Path, required=True,
                        help="JSON array with task_id, answer, expected, and optional run_key")
    parser.add_argument("--model", required=True)
    parser.add_argument("--auth-file", type=Path, required=True)
    parser.add_argument("--header-profile", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--max-format-attempts", type=int, default=3)
    args = parser.parse_args()
    failures = json.loads(args.failures.read_text())
    if not isinstance(failures, list):
        raise ValueError("failures input must be an array")
    headers = load_header_profile(args.header_profile)
    provider = OpenCodeZenProvider(args.auth_file, args.timeout)
    records = []
    if args.out.exists():
        prior = json.loads(args.out.read_text())
        if any(record.get("adjudicator_model") != args.model for record in prior.get("records", [])):
            raise SystemExit("refusing to resume a run adjudicated by a different model")
        records = prior.get("records", [])
    records = drain(  # Deliberately exhaustive: no sampling of failures.
        failures, records,
        lambda item: adjudicate_item(item, provider.complete, model=args.model, headers=headers,
                                     max_format_attempts=args.max_format_attempts),
        lambda current: write_checkpoint(args.out, current))
    write_checkpoint(args.out, records)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
