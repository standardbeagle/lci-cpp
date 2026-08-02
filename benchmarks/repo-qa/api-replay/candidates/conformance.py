#!/usr/bin/env python3
"""Check candidate codecs against the fixed adversarial JSON-value bank."""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
DEFAULT_BANK = HERE / "value-bank.json"


def canonical(value: object) -> str:
    """Return the comparison representation for JSON-compatible values."""
    return json.dumps(
        value,
        ensure_ascii=True,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    )


def digest_text(text: str) -> str:
    return "sha256:" + hashlib.sha256(text.encode("utf-8", errors="surrogatepass")).hexdigest()


def load_bank(path: Path = DEFAULT_BANK) -> dict:
    bank = json.loads(path.read_text(encoding="utf-8"))
    if bank.get("schema") != "lci.api-replay.candidate-value-bank.v1":
        raise ValueError("wrong value-bank schema")
    cases = bank.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError("value bank must contain cases")
    ids = [case.get("id") for case in cases]
    if any(not isinstance(case_id, str) or not case_id for case_id in ids):
        raise ValueError("every case requires a non-empty string id")
    if len(ids) != len(set(ids)):
        raise ValueError("value-bank case ids must be unique")
    for case in cases:
        if not isinstance(case.get("category"), str) or "value" not in case:
            raise ValueError(f"invalid case metadata: {case.get('id')}")
        canonical(case["value"])
    return bank


def load_candidate(path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        "lci_api_replay_candidate_"
        + hashlib.sha256(str(path.resolve()).encode()).hexdigest(),
        path,
    )
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load candidate module: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not callable(getattr(module, "encode", None)):
        raise ValueError("candidate must define encode(value) -> str")
    if not callable(getattr(module, "decode", None)):
        raise ValueError("candidate must define decode(text) -> object")
    return module


def evaluate_case(candidate: ModuleType, case: dict) -> dict:
    source = case["value"]
    source_text = canonical(source)
    try:
        first_input = copy.deepcopy(source)
        rendered = candidate.encode(first_input)
        if first_input != source or canonical(first_input) != source_text:
            raise ValueError("encode mutated its input")
        if not isinstance(rendered, str):
            raise TypeError("encode must return str")
        repeated = candidate.encode(copy.deepcopy(source))
        if repeated != rendered:
            raise ValueError("encode is not deterministic")
        recovered = candidate.decode(rendered)
        recovered_text = canonical(recovered)
        if recovered_text != source_text:
            raise ValueError("decoded value does not match canonical source JSON")
    except Exception as error:  # A malformed candidate is report data, not a harness crash.
        return {
            "category": case["category"],
            "id": case["id"],
            "status": "failed",
            "failure_type": type(error).__name__,
            "failure": str(error),
            "source_digest": digest_text(source_text),
        }
    return {
        "category": case["category"],
        "id": case["id"],
        "status": "passed",
        "source_digest": digest_text(source_text),
        "rendered_digest": digest_text(rendered),
        "source_utf8_bytes": len(source_text.encode("utf-8")),
        "rendered_utf8_bytes": len(rendered.encode("utf-8", errors="surrogatepass")),
        "rendered_characters": len(rendered),
        "rendered_lines": rendered.count("\n") + 1,
    }


def evaluate(candidate: ModuleType, bank: dict) -> dict:
    cases = [evaluate_case(candidate, case) for case in bank["cases"]]
    passed = [case for case in cases if case["status"] == "passed"]
    failed = [case for case in cases if case["status"] == "failed"]
    return {
        "schema": "lci.api-replay.candidate-conformance.v1",
        "bank_digest": digest_text(canonical(bank)),
        "summary": {
            "total": len(cases),
            "passed": len(passed),
            "failed": len(failed),
            "conformant": not failed,
            "measured_rendered_utf8_bytes": sum(case["rendered_utf8_bytes"] for case in passed),
            "measured_source_utf8_bytes": sum(case["source_utf8_bytes"] for case in passed),
        },
        "cases": cases,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--bank", type=Path, default=DEFAULT_BANK)
    args = parser.parse_args()
    report = evaluate(load_candidate(args.candidate), load_bank(args.bank))
    print(json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False))
    return 0 if report["summary"]["conformant"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
