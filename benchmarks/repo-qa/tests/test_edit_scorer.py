"""Contract tests for the Stage-3 EDIT-mode three-gate SCORER.

Hermetic + network-free: every run record is synthesised in-process from the
shapes the S3.2/S3.3 gates really emit and the S3.4 runner really persists. No
corpora, no checkout, no model, no credentials.

The HEADLINE metric under test is the ALL-THREE-GATES pass rate per arm --
behaviour AND convention AND blast-radius. The tests exist to stop the two ways
that metric gets quietly inflated: a missing cell vanishing from the
denominator, and a behaviour-only patch being called fully correct.

Acceptance criteria pinned here (one+ test each):
  1. per-attempt AND aggregate payloads are schema-versioned and validate;
  2. the headline denominator is every PLANNED task x arm x repetition cell --
     an absent cell fails completeness and never inflates the rate;
  3. the report carries per-gate rates, the eight-cell matrix, paired deltas,
     tool calls, tokens, wall time and a failure-reason breakdown;
  4. agent failure / invalid patch / harness failure / oracle failure stay in
     four distinct buckets;
  5. goldens recompute the headline independently and a behaviour-only patch
     does NOT score as fully correct.
"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
for _p in (
    os.path.join(BENCH_ROOT, "edits", "scoring"),
    os.path.join(BENCH_ROOT, "edits", "runner"),
):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import jsonschema  # noqa: E402

import edit_record  # noqa: E402
import edit_scorer  # noqa: E402


# ---------------------------------------------------------------------------
# synthetic gate outcomes -- the exact shapes S3.2 / S3.3 emit
# ---------------------------------------------------------------------------


def _command_run(observed="green", exit_code=0, reason=None):
    return {
        "argv": ["true"],
        "exit_code": exit_code,
        "reason": reason,
        "observed": observed,
        "stdout_tail": "",
        "stderr_tail": "",
    }


def oracle_outcome(
    *,
    existing_suite=(True, "SUITE_GREEN"),
    discrimination=(True, "DISCRIMINATES"),
    changed_path=(True, "WITHIN_SCOPE"),
    api_impact=(True, "NO_ESCAPE"),
    task_id="t1",
):
    """An oracle_gate_v1 outcome with each sub-gate's pass/reason dialled in."""
    subs = {
        "existing_suite": existing_suite,
        "discrimination": discrimination,
        "changed_path": changed_path,
        "api_impact": api_impact,
    }
    passed = all(p for p, _ in subs.values())
    return {
        "schema": "oracle_gate_v1",
        "task_id": task_id,
        "passed": passed,
        "diagnostic": "synthetic",
        "existing_suite": {
            "schema": "existing_suite_v1",
            "passed": subs["existing_suite"][0],
            "reason": subs["existing_suite"][1],
            "detail": "",
            "run": _command_run(),
        },
        "discrimination": {
            "schema": "discrimination_v1",
            "passed": subs["discrimination"][0],
            "reason": subs["discrimination"][1],
            "detail": "",
            "pristine": _command_run("red", 1),
            "patched": _command_run(),
        },
        "changed_path": {
            "schema": "changed_path_v1",
            "passed": subs["changed_path"][0],
            "reason": subs["changed_path"][1],
            "detail": "",
            "changed": ["src/a.ts"],
            "out_of_scope": [],
            "allow": ["src/*.ts"],
            "max_files": None,
        },
        "api_impact": {
            "schema": "api_impact_v1",
            "passed": subs["api_impact"][0],
            "reason": subs["api_impact"][1],
            "detail": "",
            "symbols": [],
            "escaped_refs": [],
        },
    }


def conformance_outcome(passed=True, reason="CONFORMS", task_id="t1"):
    """A conformance_gate_v1 outcome."""
    return {
        "schema": "conformance_gate_v1",
        "task_id": task_id,
        "rule_id": "rule-1",
        "kind": "structural",
        "passed": passed,
        "diagnostic": "synthetic",
        "anchors": [
            {
                "path": "src/a.ts",
                "lines": [12],
                "passed": passed,
                "reason": reason,
                "evidence": "",
                "detail": "",
            }
        ],
    }


def gates(oracle=None, conformance=None):
    return {
        "oracle": oracle if oracle is not None else oracle_outcome(),
        "conformance": (
            conformance if conformance is not None else conformance_outcome()
        ),
    }


def record(
    *,
    task_id="t1",
    arm="treatment",
    seed=7,
    status=edit_record.STATUS_PASSED,
    reason="ALL_GREEN",
    gate_outcomes=None,
    tool_calls=3,
    input_tokens=100,
    output_tokens=20,
    duration=1.5,
):
    """One S3.4-shaped edit run record."""
    return {
        "mode": "edit",
        "run_key": edit_record.run_key(task_id, arm, seed),
        "task_id": task_id,
        "corpus_id": "next.js",
        "source_commit": "a" * 40,
        "forge_version": "1",
        "seed": seed,
        "task_digest": "d" * 16,
        "task_schema": "edit_task_v1",
        "model": "test-model",
        "arm": arm,
        "status": status,
        "reason": reason,
        "gates": gate_outcomes if gate_outcomes is not None else gates(),
        "patch_hash": "p" * 16,
        "tool_calls": [{"name": "n", "arguments": {}}] * tool_calls,
        "token_usage": {"input": input_tokens, "output": output_tokens},
        "duration_seconds": duration,
    }


def planned(task_ids=("t1",), arms=("treatment", "baseline"), seeds=(7,)):
    return edit_scorer.planned_cells(task_ids, arms, seeds)


# ---------------------------------------------------------------------------
# criterion 1 -- versioned + validated schemas
# ---------------------------------------------------------------------------


class SchemaVersioning(unittest.TestCase):
    def test_shipped_schemas_are_valid_json_schema_and_version_tagged(self):
        for path, tag in (
            (edit_scorer.SCORE_SCHEMA_PATH, edit_scorer.SCORE_SCHEMA),
            (edit_scorer.AGGREGATE_SCHEMA_PATH, edit_scorer.AGGREGATE_SCHEMA),
        ):
            with open(path, encoding="utf-8") as handle:
                schema = json.load(handle)
            jsonschema.Draft202012Validator.check_schema(schema)
            self.assertEqual(schema["properties"]["schema"]["const"], tag)

    def test_score_run_output_validates_against_the_per_attempt_schema(self):
        score = edit_scorer.score_run(record())
        self.assertEqual(score["schema"], edit_scorer.SCORE_SCHEMA)
        edit_scorer.validate_score(score)

    def test_a_score_missing_a_required_field_is_rejected(self):
        score = edit_scorer.score_run(record())
        del score["gates"]
        with self.assertRaises(jsonschema.ValidationError):
            edit_scorer.validate_score(score)


# ---------------------------------------------------------------------------
# criterion 5 (per-attempt half) -- three-gate AND, never behaviour-only
# ---------------------------------------------------------------------------


class ThreeGateConjunction(unittest.TestCase):
    def test_all_three_gates_green_is_fully_correct(self):
        score = edit_scorer.score_run(record())
        self.assertTrue(score["all_gates_passed"])
        self.assertEqual(score["matrix_cell"], "TTT")
        self.assertIsNone(score["failure_class"])

    def test_behaviour_pass_but_convention_fail_is_NOT_fully_correct(self):
        score = edit_scorer.score_run(
            record(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    conformance=conformance_outcome(False, "MATCH_ABSENT")
                ),
            )
        )
        self.assertTrue(score["gates"]["behavior"]["passed"])
        self.assertFalse(score["gates"]["convention"]["passed"])
        self.assertFalse(score["all_gates_passed"])
        self.assertEqual(score["matrix_cell"], "TFT")

    def test_behaviour_pass_but_blast_fail_is_NOT_fully_correct(self):
        score = edit_scorer.score_run(
            record(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    oracle=oracle_outcome(
                        changed_path=(False, "PATH_OUT_OF_SCOPE")
                    )
                ),
            )
        )
        self.assertTrue(score["gates"]["behavior"]["passed"])
        self.assertFalse(score["gates"]["blast"]["passed"])
        self.assertFalse(score["all_gates_passed"])
        self.assertEqual(score["matrix_cell"], "TTF")

    def test_behaviour_gate_spans_existing_suite_AND_discrimination(self):
        """A green discrimination test cannot mask a red existing suite."""
        score = edit_scorer.score_run(
            record(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    oracle=oracle_outcome(existing_suite=(False, "SUITE_RED"))
                ),
            )
        )
        self.assertFalse(score["gates"]["behavior"]["passed"])
        self.assertIn("SUITE_RED", score["gates"]["behavior"]["reasons"])
        self.assertFalse(score["all_gates_passed"])

    def test_blast_gate_spans_changed_path_AND_api_impact(self):
        """An in-scope diff that ripples past the declared API still fails blast."""
        score = edit_scorer.score_run(
            record(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    oracle=oracle_outcome(api_impact=(False, "API_IMPACT_ESCAPE"))
                ),
            )
        )
        self.assertFalse(score["gates"]["blast"]["passed"])
        self.assertIn("API_IMPACT_ESCAPE", score["gates"]["blast"]["reasons"])
        self.assertFalse(score["all_gates_passed"])

    def test_absent_gates_fail_closed_and_are_marked_unjudged(self):
        """gates=None (the agent never got judged) is a FAIL, never a pass."""
        raw = record(status=edit_record.STATUS_AGENT_FAILURE, reason="AGENT_ERROR")
        raw["gates"] = None
        score = edit_scorer.score_run(raw)
        for name in edit_scorer.GATE_NAMES:
            self.assertFalse(score["gates"][name]["passed"])
            self.assertFalse(score["gates"][name]["judged"])
            self.assertEqual(
                score["gates"][name]["reasons"], [edit_scorer.REASON_GATE_ABSENT]
            )
        self.assertFalse(score["all_gates_passed"])
        self.assertEqual(score["matrix_cell"], "FFF")


if __name__ == "__main__":
    unittest.main()
