#!/usr/bin/env python3
"""Typed, source-grounded claims oracle for all LCI tool replay tasks."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path


LOCATION = re.compile(r"(?P<path>[\w.@+\-/]+)[`'\"]?(?::|\s+at\s+(?:line\s+)?)\s*(?P<line>\d+)\b", re.I)
SHA = re.compile(r"\b[0-9a-f]{40}\b", re.I)
NEGATIVE = re.compile(r"\b(?:no|none|zero|empty|not found|did not|failed?|error|invalid|unable)\b", re.I)


def digest(text: str) -> str:
    return "sha256:" + hashlib.sha256(text.encode()).hexdigest()


def normalize(text: str) -> str:
    return " ".join(text.casefold().split())


def _locations(text: str) -> set[str]:
    return {f"{m.group('path').casefold()}:{int(m.group('line'))}" for m in LOCATION.finditer(text)}


def evaluate_claim(answer: str, claim: dict) -> tuple[bool, dict]:
    folded = normalize(answer)
    kind = claim["kind"]
    if kind == "literal":
        found = claim["value"].casefold() in folded
    elif kind == "location":
        found = claim["value"].casefold() in _locations(answer)
    elif kind == "sha":
        found = claim["value"].casefold() in {item.casefold() for item in SHA.findall(answer)}
    elif kind == "integer":
        found = bool(re.search(rf"(?<!\d){int(claim['value'])}(?!\d)", answer))
    elif kind == "semantic":
        found = any(all(term.casefold() in folded for term in alternative)
                    for alternative in claim["alternatives"])
    elif kind == "absence":
        found = bool(NEGATIVE.search(answer))
    elif kind == "outcome":
        value = claim["value"]
        error = bool(re.search(r"\b(?:error|failed?|unable|invalid)\b", answer, re.I))
        not_found = bool(re.search(r"\b(?:not found|no (?:indexed )?(?:file|symbol))\b", answer, re.I))
        empty = bool(re.search(r"\b(?:no (?:files?|matches|semantic annotations)|zero (?:symbols|matches)|empty)\b", answer, re.I))
        no_data = bool(re.search(r"\b(?:no|without) (?:usable )?(?:data|summary|result)\b", answer, re.I))
        fallback = (bool(re.search(r"\b(?:summary data|index summary|usable .*data|ready status)\b", answer, re.I))
                    and not error and not no_data)
        corrective = (bool(re.search(r"\b(?:available tools?|corrective guidance|try|use)\b", answer, re.I))
                      and not bool(re.search(r"\b(?:details?|result) (?:for|of) (?:the )?nonexistent\b", answer, re.I)))
        found = {"error": error, "not_found": not_found, "empty": empty,
                 "fallback": fallback, "corrective": corrective}[value]
    else:
        raise ValueError(f"unknown claim kind: {kind}")
    return found, {"id": claim["id"], "kind": kind, "passed": found}


def evaluate(answer: str, schema: dict) -> dict:
    claims = [evaluate_claim(answer, claim)[1] for claim in schema["required_claims"]]
    forbidden = [pattern for pattern in schema.get("forbidden_patterns", [])
                 if re.search(pattern, answer, re.I)]
    passed = all(item["passed"] for item in claims) and not forbidden
    return {"schema": "lci.typed-claim-score.v1", "task_id": schema["task_id"],
            "status": "accepted" if passed else "needs_adjudication", "exact": passed,
            "claims": claims, "failed_claims": [item["id"] for item in claims if not item["passed"]],
            "forbidden_matches": forbidden, "answer_digest": digest(answer)}


def validate_bank(bank: dict, matrix: dict) -> None:
    schemas = bank.get("schemas", [])
    cases = {f"{item['tool']}--{item['scenario']}": item for item in matrix["cases"]}
    if len(schemas) != 28 or {item["task_id"] for item in schemas} != set(cases):
        raise ValueError("claim bank must cover all 28 tool/scenario cases exactly once")
    for schema in schemas:
        case = cases[schema["task_id"]]
        if schema.get("source_output_digest") != digest(case["output"]):
            raise ValueError(f"source output digest mismatch: {schema['task_id']}")
        if not schema.get("good_answers") or not schema.get("bad_answers"):
            raise ValueError(f"missing discrimination cases: {schema['task_id']}")
        if not all(evaluate(answer, schema)["exact"] for answer in schema["good_answers"]):
            raise ValueError(f"known-good discrimination failure: {schema['task_id']}")
        if not all(not evaluate(answer, schema)["exact"] for answer in schema["bad_answers"]):
            raise ValueError(f"known-bad discrimination failure: {schema['task_id']}")


def load_bank(path: Path, matrix_path: Path) -> dict:
    bank, matrix = json.loads(path.read_text()), json.loads(matrix_path.read_text())
    validate_bank(bank, matrix)
    return bank
