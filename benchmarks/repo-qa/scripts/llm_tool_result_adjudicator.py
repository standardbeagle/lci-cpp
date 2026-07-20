#!/usr/bin/env python3
"""Exhaustively adjudicate heuristic misses from the all-tools replay."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from opencode_zen_provider import OpenCodeZenProvider


def prompt(item: dict) -> str:
    payload = {"question": item["question"], "candidate_answer": item["answer"],
               "independent_truth": item["expected"],
               "rubric": ["Judge semantics, not exact wording.",
                           "Reject invented results, omitted required facts, and confusing an error with an empty success.",
                           "Use only the supplied candidate and truth."],
               "output_schema": {"verdict": "correct|incorrect|ambiguous",
                                   "supported_claims": ["claim"], "unsupported_claims": ["claim"],
                                   "missing_claims": ["claim"], "reason": "short reason",
                                   "heuristic_gap": "deterministic improvement suggestion or null"}}
    return "Return exactly one JSON object and no markdown.\n" + json.dumps(payload, sort_keys=True)


def validate(value: object) -> dict:
    if not isinstance(value, dict) or value.get("verdict") not in {"correct", "incorrect", "ambiguous"}:
        raise ValueError("invalid verdict")
    for field in ("supported_claims", "unsupported_claims", "missing_claims"):
        if not isinstance(value.get(field), list) or not all(isinstance(x, str) for x in value[field]):
            raise ValueError(f"invalid {field}")
    if not isinstance(value.get("reason"), str) or not value["reason"].strip():
        raise ValueError("missing reason")
    if value["verdict"] == "correct" and (value["unsupported_claims"] or value["missing_claims"]):
        raise ValueError("correct verdict contradicts claim lists")
    return value


def parse(text: str) -> dict:
    stripped = text.strip()
    if stripped.startswith("```"):
        stripped = "\n".join(stripped.splitlines()[1:-1])
    return validate(json.loads(stripped))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--queue", type=Path, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--auth-file", type=Path, required=True)
    parser.add_argument("--header-profile", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    queue_doc = json.loads(args.queue.read_text())
    queue = queue_doc.get("adjudication_queue", queue_doc)
    headers = json.loads(args.header_profile.read_text())["headers"]
    provider = OpenCodeZenProvider(args.auth_file, args.timeout)
    records = []
    for item in queue:
        text = prompt(item)
        body = {"model": args.model.split("/", 1)[1], "temperature": 0, "stream": False,
                "messages": [{"role": "system", "content": "You are a blinded benchmark adjudicator."},
                             {"role": "user", "content": text}]}
        response = provider.complete(body, args.model, headers)
        judgment = parse(response["final_answer"]) if response["status"] == "answered" else None
        records.append({"cell_key": item["cell_key"], "provider_status": response["status"],
                        "adjudicator_model": args.model,
                        "prompt_digest": "sha256:" + hashlib.sha256(text.encode()).hexdigest(),
                        "judgment": judgment,
                        "regression_candidate": None if not judgment or judgment["verdict"] != "correct" else
                            {"answer": item["answer"], "expected": item["expected"],
                             "required_outcome": "accepted", "proposed_gap": judgment.get("heuristic_gap")}})
    result = {"schema": "lci.all-tools-adjudications.v1", "policy": "every queued failure",
              "records": records,
              "promotion_policy": "LLM judgments do not change scores directly; correct misses require deterministic regression promotion and full reanalysis."}
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
