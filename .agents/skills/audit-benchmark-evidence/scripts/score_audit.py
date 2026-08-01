#!/usr/bin/env python3
"""Validate structured audit gates and derive a fail-closed outcome."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


STATUSES = {"pass", "fail", "inconclusive", "not_applicable"}
SEVERITIES = {"none", "minor", "major", "critical"}


def score(data: object) -> tuple[str, list[str]]:
    if not isinstance(data, dict) or data.get("schema") != "benchmark.audit.v1":
        return "inconclusive", ["schema must be benchmark.audit.v1"]
    gates = data.get("gates")
    if not isinstance(gates, list) or not gates:
        return "inconclusive", ["gates must be a non-empty list"]
    errors = []
    ids = []
    for index, gate in enumerate(gates):
        if not isinstance(gate, dict):
            errors.append(f"gate {index} must be an object")
            continue
        ids.append(gate.get("id"))
        if gate.get("status") not in STATUSES:
            errors.append(f"gate {index} has invalid status")
        if gate.get("severity") not in SEVERITIES:
            errors.append(f"gate {index} has invalid severity")
        # Status and severity must agree, or the outcome ladder cannot be
        # fail-closed: an unclassified failure would otherwise be indistinguishable
        # from a clean pass, and a severity on a passing gate would be ignored.
        elif gate.get("status") == "fail" and gate["severity"] == "none":
            errors.append(f"gate {index} fails but declares no severity")
        elif gate.get("status") in {"pass", "not_applicable"} and gate["severity"] != "none":
            errors.append(f"gate {index} does not fail but declares a severity")
        if not isinstance(gate.get("evidence"), list) or not gate.get("evidence"):
            errors.append(f"gate {index} requires artifact evidence")
    if len(set(ids)) != len(ids) or None in ids:
        errors.append("gate ids must be present and unique")
    if errors:
        return "inconclusive", errors
    hard = [gate for gate in gates if gate.get("hard") is True]
    if any(gate["status"] == "fail" for gate in hard):
        return "fail", []
    if any(gate["status"] == "inconclusive" for gate in hard):
        return "inconclusive", []
    if any(gate["status"] == "fail" and gate["severity"] in {"major", "critical"} for gate in gates):
        return "fail", []
    if any(gate["status"] == "inconclusive" for gate in gates):
        return "inconclusive", []
    # Only minor soft-gate failures survive to here: the rubric defines `minor` as
    # a defect that cannot change the supported conclusion.
    if any(gate["status"] == "fail" for gate in gates):
        return "pass_with_changes", []
    return "pass", []


def gate(identifier: str, *, hard: bool, status: str, severity: str) -> dict:
    return {"id": identifier, "hard": hard, "status": status, "severity": severity,
            "evidence": ["artifact"]}


def self_test() -> int:
    """Assertion-free so the checks still run under `python -O`."""
    hard_pass = gate("H", hard=True, status="pass", severity="none")
    cases = [
        ("hard pass alone is a pass", [hard_pass], "pass"),
        ("a failed hard gate fails at any severity",
         [gate("H", hard=True, status="fail", severity="none")], "inconclusive"),
        ("a failed hard gate outranks passing soft gates",
         [gate("H2", hard=True, status="fail", severity="minor"), hard_pass], "fail"),
        ("an inconclusive hard gate is inconclusive",
         [gate("H", hard=True, status="inconclusive", severity="none")], "inconclusive"),
        ("a major soft failure fails",
         [hard_pass, gate("S", hard=False, status="fail", severity="major")], "fail"),
        ("a critical soft failure fails",
         [hard_pass, gate("S", hard=False, status="fail", severity="critical")], "fail"),
        ("a minor soft failure is pass_with_changes",
         [hard_pass, gate("S", hard=False, status="fail", severity="minor")], "pass_with_changes"),
        ("a failure with no severity never reaches a pass",
         [hard_pass, gate("S", hard=False, status="fail", severity="none")], "inconclusive"),
        ("a passing gate may not carry a severity",
         [hard_pass, gate("S", hard=False, status="pass", severity="minor")], "inconclusive"),
        ("a soft inconclusive gate is inconclusive",
         [hard_pass, gate("S", hard=False, status="inconclusive", severity="none")], "inconclusive"),
        ("duplicate gate ids are rejected", [hard_pass, hard_pass], "inconclusive"),
    ]
    failures = []
    for label, gates, expected in cases:
        actual = score({"schema": "benchmark.audit.v1", "gates": gates})[0]
        if actual != expected:
            failures.append(f"{label}: expected {expected}, got {actual}")
    for label, data, expected in (("a non-object audit", [], "inconclusive"),
                                  ("a wrong schema", {"schema": "other", "gates": []}, "inconclusive"),
                                  ("an empty gate list", {"schema": "benchmark.audit.v1", "gates": []}, "inconclusive")):
        actual = score(data)[0]
        if actual != expected:
            failures.append(f"{label}: expected {expected}, got {actual}")
    if failures:
        raise SystemExit("score_audit self-test failed:\n" + "\n".join(failures))
    print(f"score_audit self-test: {len(cases) + 3} checks passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("audit", type=Path, nargs="?")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.audit is None:
        parser.error("audit is required unless --self-test is used")
    try:
        data = json.loads(args.audit.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(json.dumps({"outcome": "inconclusive", "errors": [str(exc)]}))
        return 2
    outcome, errors = score(data)
    print(json.dumps({"outcome": outcome, "errors": errors}, sort_keys=True))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
