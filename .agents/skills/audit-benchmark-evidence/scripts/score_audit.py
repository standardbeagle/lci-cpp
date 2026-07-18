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
    if any(gate["status"] == "fail" or gate["severity"] == "minor" for gate in gates):
        return "pass_with_changes", []
    return "pass", []


def self_test() -> int:
    base = {"schema": "benchmark.audit.v1", "gates": [{"id": "G1", "hard": True, "status": "pass", "severity": "none", "evidence": ["artifact"]}]}
    assert score(base)[0] == "pass"
    failed = json.loads(json.dumps(base)); failed["gates"][0].update(status="fail", severity="major")
    assert score(failed)[0] == "fail"
    unknown = json.loads(json.dumps(base)); unknown["gates"][0]["status"] = "inconclusive"
    assert score(unknown)[0] == "inconclusive"
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
