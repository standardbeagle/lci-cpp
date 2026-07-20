#!/usr/bin/env python3
"""Build a blinded adjudication queue from the withheld-label meta-eval bank."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def build(document: dict) -> dict:
    queue = []
    ids = set()
    for case in document["cases"]:
        if case["case_id"] in ids:
            raise ValueError("duplicate case_id")
        ids.add(case["case_id"])
        truth = {"source_tool_output": case["tool_output"],
                 "evaluation_requirements": [
                     "Every factual assertion must be supported by the tool output.",
                     "Distinguish established evidence from a proposed next step.",
                     "Do not claim completion when the objective still requires unavailable evidence.",
                     "Recovery must respond to the observed result rather than inventing success."
                 ]}
        for repetition in (1, 2):
            queue.append({"cell_key": f"{case['case_id']}--r{repetition}",
                          "case_id": case["case_id"], "repetition": repetition,
                          "task_id": case["stratum"], "question": case["objective"],
                          "answer": case["candidate_response"], "truth": truth})
    return {"schema": "lci.evaluator-meta-queue.v1", "adjudication_queue": queue,
            "fixture_digest": "sha256:" + hashlib.sha256(
                json.dumps(document, sort_keys=True, separators=(",", ":")).encode()).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = build(json.loads(args.cases.read_text()))
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
