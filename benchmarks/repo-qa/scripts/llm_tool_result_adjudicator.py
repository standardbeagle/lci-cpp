#!/usr/bin/env python3
"""Exhaustively adjudicate heuristic misses from the all-tools replay."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import os
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from opencode_zen_provider import OpenCodeZenProvider


def prompt(item: dict) -> str:
    payload = {"question": item["question"], "candidate_answer": item["answer"],
               "independent_truth": item.get("truth", item.get("expected")),
               "response_stage": "This is a post-tool continuation. It may be a valid intermediate response, not a final answer.",
               "rubric": ["Judge semantics, not exact wording.",
                           "Reject invented results, omitted required facts, and confusing an error with an empty success.",
                           "Do not require the overall objective to be finished when the supplied evidence is insufficient and the candidate explicitly proposes an appropriate next step.",
                           "Do not penalize omission of tool-output details that are unnecessary for the stated objective or next step.",
                           "Use ambiguous only when the evidence genuinely cannot determine whether a candidate claim is supported; definite unsupported claims require incorrect.",
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
    if value["verdict"] == "ambiguous" and value["unsupported_claims"]:
        raise ValueError("ambiguous verdict contradicts definite unsupported claims")
    return value


def parse(text: str) -> dict:
    stripped = text.strip()
    if stripped.startswith("```"):
        stripped = "\n".join(stripped.splitlines()[1:-1])
    return validate(json.loads(stripped))


def write_checkpoint(path: Path, records: list[dict], *, judge_id: str = "unspecified") -> None:
    result = {"schema": "lci.all-tools-adjudications.v2", "policy": "every queued failure",
              "judge_id": judge_id,
              "records": records,
              "promotion_policy": "Judgments never change scores directly; only two-judge consensus may nominate a regression, which still requires a deterministic test and full reanalysis."}
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name, suffix=".tmp", dir=path.parent)
    with os.fdopen(fd, "w") as handle:
        json.dump(result, handle, indent=2, sort_keys=True)
        handle.write("\n")
    os.replace(temporary, path)


def adjudicate_item(item: dict, complete, *, model: str, headers: dict,
                    max_format_attempts: int = 3) -> dict:
    """Retry malformed structure only; never retry or reinterpret a valid verdict."""
    text = prompt(item)
    attempts = []
    judgment = None
    for number in range(1, max_format_attempts + 1):
        body = {"model": model.split("/", 1)[-1], "temperature": 0, "stream": False,
                "messages": [{"role": "system", "content": "You are a blinded benchmark adjudicator."},
                             {"role": "user", "content": text}]}
        response = complete(body, model, headers)
        attempt = {"attempt": number, "provider_status": response["status"],
                   "raw_answer": response.get("final_answer", ""), "parse_error": None}
        if response["status"] == "answered":
            try:
                judgment = parse(attempt["raw_answer"])
            except (ValueError, json.JSONDecodeError) as error:
                attempt["parse_error"] = str(error)
        attempts.append(attempt)
        if judgment is not None or response["status"] != "answered":
            break
    return {"cell_key": item["cell_key"], "task_id": item.get("task_id"),
            "adjudicator_model": model,
            "prompt_digest": "sha256:" + hashlib.sha256(text.encode()).hexdigest(),
            "judgment": judgment, "attempts": attempts}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--queue", type=Path, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--auth-file", type=Path, required=True)
    parser.add_argument("--header-profile", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--judge-id", required=True)
    parser.add_argument("--origin-model", help="only adjudicate failures produced by this model")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--max-format-attempts", type=int, default=3)
    args = parser.parse_args()
    queue_doc = json.loads(args.queue.read_text())
    queue = queue_doc.get("adjudication_queue", queue_doc)
    if args.origin_model:
        queue = [item for item in queue if item.get("origin_model") == args.origin_model]
    headers = json.loads(args.header_profile.read_text())["headers"]
    provider = OpenCodeZenProvider(args.auth_file, args.timeout)
    records = []
    if args.out.exists():
        prior = json.loads(args.out.read_text())
        if prior.get("judge_id") != args.judge_id:
            raise SystemExit("refusing to resume a different judge-id")
        records = prior.get("records", [])
    completed = {item["cell_key"] for item in records}
    for item in queue:
        if item["cell_key"] in completed:
            continue
        records.append(adjudicate_item(item, provider.complete, model=args.model, headers=headers,
                                       max_format_attempts=args.max_format_attempts))
        write_checkpoint(args.out, records, judge_id=args.judge_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
