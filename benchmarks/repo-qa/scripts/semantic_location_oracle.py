#!/usr/bin/env python3
"""Auditable staged oracle for source-location answers.

The deterministic stage extracts several ordinary file/line phrasings and records
which rule fired.  Non-exact answers are *provisional* failures: callers must queue
them for blinded adjudication rather than treating them as final model errors.
"""
from __future__ import annotations

import hashlib
import json
import re


PATH = r"[A-Za-z0-9_.@+\-]+(?:/[A-Za-z0-9_.@+\-]+)+"
RULES = (
    ("colon", re.compile(rf"(?<![\w./-])(?P<path>{PATH}):(?P<line>\d+)(?![\w/:-])", re.I)),
    ("at-line", re.compile(rf"(?<![\w./-])(?P<path>{PATH})[`'\"]?\s+(?:at\s+)?line\s+(?P<line>\d+)\b", re.I)),
    ("line-in", re.compile(rf"\bline\s+(?P<line>\d+)\s+(?:of|in)\s+[`'\"]?(?P<path>{PATH})", re.I)),
    ("paren-line", re.compile(rf"(?<![\w./-])(?P<path>{PATH})[`'\"]?\s*\(\s*(?:line\s+)?(?P<line>\d+)\s*\)", re.I)),
    ("markdown-anchor", re.compile(rf"\[[^\]]*\]\((?P<path>{PATH})#L(?P<line>\d+)\)", re.I)),
)
NEGATION = re.compile(r"\b(?:not|isn't|is not|never|no call(?:site)?)\b", re.I)


def normalize_location(path: str, line: str | int) -> str:
    return f"{path.casefold()}:{int(line)}"


def extract_locations(answer: str) -> dict:
    hits: dict[str, set[str]] = {}
    for rule, pattern in RULES:
        for match in pattern.finditer(answer):
            # Reject a locally negated assertion; ambiguous cases go to adjudication.
            clause = answer[max(0, match.start() - 48):match.start()]
            if NEGATION.search(clause.split(".")[-1]):
                continue
            location = normalize_location(match.group("path"), match.group("line"))
            hits.setdefault(location, set()).add(rule)
    return {"locations": sorted(hits),
            "evidence": {key: sorted(value) for key, value in sorted(hits.items())}}


def evaluate(answer: str, expected: list[str]) -> dict:
    extracted = extract_locations(answer)
    predicted, truth = set(extracted["locations"]), {item.casefold() for item in expected}
    tp = len(predicted & truth)
    exact = predicted == truth
    payload = {
        "stage": "deterministic-heuristic-v2",
        "status": "accepted" if exact else "needs_adjudication",
        "exact": exact,
        "extracted": sorted(predicted),
        "expected": sorted(truth),
        # Empty-set convention, identical to comprehension_ab.grade: a vacuous
        # side scores 1.0 only when the other side is also empty, so an invented
        # location against an empty answer key still scores 0.0.
        "precision": tp / len(predicted) if predicted else (1.0 if not truth else 0.0),
        "recall": tp / len(truth) if truth else (1.0 if not predicted else 0.0),
        "false_positives": sorted(predicted - truth),
        "false_negatives": sorted(truth - predicted),
        "evidence": extracted["evidence"],
    }
    payload["answer_digest"] = "sha256:" + hashlib.sha256(answer.encode()).hexdigest()
    return payload


def adjudication_prompt(*, task: str, answer: str, expected: list[str]) -> str:
    rubric = {
        "task": task,
        "candidate_answer": answer,
        "source_of_truth_locations": expected,
        "instructions": [
            "Judge semantic correctness, not wording or formatting.",
            "A correct answer must assert every source-of-truth callsite and no unsupported callsite.",
            "Definitions, examples, and explicitly negated locations are not asserted callsites.",
            "Use only the supplied answer and source of truth.",
        ],
        "output_schema": {
            "verdict": "correct|incorrect|ambiguous",
            "asserted_locations": ["path:line"],
            "unsupported_locations": ["path:line"],
            "reason": "short explanation",
            "heuristic_gap": "short deterministic rule proposal or null",
        },
    }
    return "Return exactly one JSON object and no markdown.\n" + json.dumps(rubric, sort_keys=True)


def validate_adjudication(value: object, expected: list[str]) -> dict:
    if not isinstance(value, dict) or value.get("verdict") not in {"correct", "incorrect", "ambiguous"}:
        raise ValueError("invalid adjudication verdict")
    for field in ("asserted_locations", "unsupported_locations"):
        if not isinstance(value.get(field), list) or not all(isinstance(x, str) for x in value[field]):
            raise ValueError(f"invalid adjudication field: {field}")
    if not isinstance(value.get("reason"), str) or not value["reason"].strip():
        raise ValueError("adjudication requires a reason")
    normalized = sorted({item.casefold() for item in value["asserted_locations"]})
    truth = sorted({item.casefold() for item in expected})
    # The structured claims must independently agree with a 'correct' verdict.
    if (value["verdict"] == "correct") != (normalized == truth and not value["unsupported_locations"]):
        raise ValueError("verdict contradicts structured location claims")
    return {**value, "asserted_locations": normalized, "expected": truth}
