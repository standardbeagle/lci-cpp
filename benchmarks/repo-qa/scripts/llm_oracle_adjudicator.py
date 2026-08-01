#!/usr/bin/env python3
"""Blindly adjudicate every deterministic-oracle miss and emit regression cases."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from opencode_zen_provider import OpenCodeZenProvider
from semantic_location_oracle import adjudication_prompt, evaluate, validate_adjudication


def parse_json_answer(answer: str) -> dict:
    text = answer.strip()
    if text.startswith("```"):
        lines = text.splitlines()
        text = "\n".join(lines[1:-1])
    value = json.loads(text)
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
    return {"model": model.split("/", 1)[1], "temperature": 0, "stream": False,
            "messages": [{"role": "system", "content": "You are a strict, blinded benchmark adjudicator."},
                         {"role": "user", "content": prompt}]}


def load_header_profile(path: Path) -> dict:
    """Header profiles are committed with a `headers` key; anything else is a bug."""
    document = json.loads(path.read_text())
    headers = document.get("headers")
    if not isinstance(headers, dict):
        raise SystemExit(
            f"{path}: header profile must carry a 'headers' object, found keys {sorted(document)}"
        )
    return headers


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
    output = {"schema": "lci.oracle-adjudications.v1", "policy": "every-provisional-failure",
              "records": records,
              "promotion_policy": "Regression candidates require deterministic rule implementation plus positive and adversarial tests; LLM verdicts never directly override primary scores."}
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name, suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as handle:
            json.dump(output, handle, indent=2, sort_keys=True)
            handle.write("\n")
        os.replace(temporary, path)
    except BaseException:
        try: os.unlink(temporary)
        except FileNotFoundError: pass
        raise


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
    completed = {item_key(record) for record in records}
    for item in failures:  # Deliberately exhaustive: no sampling of failures.
        if item_key(item) in completed:
            continue
        records.append(adjudicate_item(item, provider.complete, model=args.model, headers=headers,
                                       max_format_attempts=args.max_format_attempts))
        write_checkpoint(args.out, records)
    write_checkpoint(args.out, records)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
