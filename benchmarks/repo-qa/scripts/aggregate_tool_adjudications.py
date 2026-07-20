#!/usr/bin/env python3
"""Combine two independent adjudication files without overriding oracle scores."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def aggregate(left: dict, right: dict, queue: list[dict]) -> dict:
    if left.get("judge_id") == right.get("judge_id"):
        raise ValueError("judge_id values must be independent")
    by_queue = {item["cell_key"]: item for item in queue}
    a = {item["cell_key"]: item for item in left.get("records", [])}
    b = {item["cell_key"]: item for item in right.get("records", [])}
    records, candidates = [], []
    for key in sorted(by_queue):
        ja = a.get(key, {}).get("judgment")
        jb = b.get(key, {}).get("judgment")
        verdicts = [item.get("verdict") for item in (ja, jb) if item]
        resolved = len(verdicts) == 2 and verdicts[0] == verdicts[1] and verdicts[0] != "ambiguous"
        verdict = verdicts[0] if resolved else "unresolved"
        records.append({"cell_key": key, "verdict": verdict, "resolved": resolved,
                        "judge_verdicts": verdicts})
        if verdict == "correct":
            item = by_queue[key]
            candidates.append({"cell_key": key, "answer": item["answer"],
                               "truth": item.get("truth", item.get("expected")),
                               "required_outcome": "accepted"})
    return {"schema": "lci.all-tools-adjudication-consensus.v1",
            "score_override_allowed": False, "records": records,
            "regression_candidates": candidates,
            "unresolved_count": sum(not item["resolved"] for item in records)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--left", type=Path, required=True)
    parser.add_argument("--right", type=Path, required=True)
    parser.add_argument("--queue", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    queue_doc = json.loads(args.queue.read_text())
    result = aggregate(json.loads(args.left.read_text()), json.loads(args.right.read_text()),
                       queue_doc.get("adjudication_queue", queue_doc))
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
