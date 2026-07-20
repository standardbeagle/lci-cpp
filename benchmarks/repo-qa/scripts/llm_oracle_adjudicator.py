#!/usr/bin/env python3
"""Blindly adjudicate every deterministic-oracle miss and emit regression cases."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--failures", type=Path, required=True,
                        help="JSON array with task_id, answer, expected, and optional run_key")
    parser.add_argument("--model", required=True)
    parser.add_argument("--auth-file", type=Path, required=True)
    parser.add_argument("--header-profile", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    failures = json.loads(args.failures.read_text())
    if not isinstance(failures, list):
        raise ValueError("failures input must be an array")
    headers_doc = json.loads(args.header_profile.read_text())
    headers = headers_doc.get("request_headers", headers_doc.get("headers"))
    provider = OpenCodeZenProvider(args.auth_file, args.timeout)
    records = []
    for item in failures:  # Deliberately exhaustive: no sampling of failures.
        prompt = adjudication_prompt(task=item["task_id"], answer=item["answer"], expected=item["expected"])
        response = provider.complete(request_body(args.model, prompt), args.model, headers)
        record = {"run_key": item.get("run_key"), "task_id": item["task_id"],
                  "answer_digest": "sha256:" + hashlib.sha256(item["answer"].encode()).hexdigest(),
                  "adjudicator_model": args.model, "prompt_digest": "sha256:" + hashlib.sha256(prompt.encode()).hexdigest(),
                  "provider_status": response["status"], "adjudication": None,
                  "regression_candidate": None}
        if response["status"] == "answered":
            judgment = validate_adjudication(parse_json_answer(response["final_answer"]), item["expected"])
            record["adjudication"] = judgment
            if judgment["verdict"] == "correct":
                record["regression_candidate"] = {
                    "answer": item["answer"], "expected": item["expected"],
                    "required_outcome": "accepted", "proposed_gap": judgment.get("heuristic_gap"),
                }
        else:
            record["failure"] = response.get("failure")
        records.append(record)
    output = {"schema": "lci.oracle-adjudications.v1", "policy": "every-provisional-failure",
              "records": records,
              "promotion_policy": "Regression candidates require deterministic rule implementation plus positive and adversarial tests; LLM verdicts never directly override primary scores."}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
