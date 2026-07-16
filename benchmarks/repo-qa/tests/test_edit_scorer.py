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
    gate_outcomes="auto",
    tool_calls=3,
    input_tokens=100,
    output_tokens=20,
    duration=1.5,
):
    """One S3.4-shaped edit run record.

    Gates default to the runner's real behaviour: only a cell that reached a
    verdict (passed / gate_failed) carries gate outcomes -- an agent failure or
    an invalid patch never gets judged, so its gates are absent.
    """
    if gate_outcomes == "auto":
        gate_outcomes = (
            gates()
            if status in (edit_record.STATUS_PASSED, edit_record.STATUS_GATE_FAILED)
            else None
        )
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
        "gates": gate_outcomes,
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
        self.assertIsNone(raw["gates"])
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


# ---------------------------------------------------------------------------
# criterion 2 -- the denominator is the PLANNED cell set
# ---------------------------------------------------------------------------


class HeadlineDenominator(unittest.TestCase):
    def test_headline_is_all_pass_over_planned_cells(self):
        plan = planned(task_ids=("t1", "t2"))
        scores = [
            edit_scorer.score_run(record(task_id=t, arm=a))
            for t in ("t1", "t2")
            for a in ("treatment", "baseline")
        ]
        agg = edit_scorer.aggregate(scores, plan)
        self.assertEqual(agg["headline"]["metric"], "all_three_gates_pass_rate")
        self.assertEqual(agg["headline"]["denominator"], "planned_cells")
        self.assertTrue(agg["headline"]["complete"])
        for arm in ("treatment", "baseline"):
            self.assertEqual(agg["headline"]["arms"][arm]["planned"], 2)
            self.assertEqual(agg["headline"]["arms"][arm]["rate"], 1.0)

    def test_a_missing_cell_FAILS_completeness_and_does_not_inflate_headline(self):
        """The load-bearing anti-inflation test: a cell that never ran must not
        quietly leave the denominator and turn 1-of-2 into 1-of-1."""
        plan = planned(task_ids=("t1", "t2"))
        # t2/treatment never produced a record.
        scores = [
            edit_scorer.score_run(record(task_id="t1", arm="treatment")),
            edit_scorer.score_run(record(task_id="t1", arm="baseline")),
            edit_scorer.score_run(record(task_id="t2", arm="baseline")),
        ]
        agg = edit_scorer.aggregate(scores, plan)

        headline = agg["headline"]["arms"]["treatment"]
        self.assertEqual(headline["planned"], 2)
        self.assertEqual(headline["observed"], 1)
        self.assertEqual(headline["all_pass"], 1)
        # 1/2, NOT the 1/1 an observed-only denominator would have reported.
        self.assertEqual(headline["rate"], 0.5)

        self.assertFalse(agg["completeness"]["passed"])
        self.assertFalse(agg["headline"]["complete"])
        self.assertEqual(agg["completeness"]["missing_cells"], 1)
        self.assertEqual(
            agg["completeness"]["missing"],
            [{"task_id": "t2", "arm": "treatment", "seed": 7}],
        )
        # ...and the absent cell is bucketed, not dropped.
        self.assertEqual(
            agg["arms"]["treatment"]["failure_reasons"]["missing_cell"], 1
        )

    def test_an_unplanned_cell_fails_completeness(self):
        plan = planned(task_ids=("t1",))
        scores = [
            edit_scorer.score_run(record(task_id="t1", arm=a))
            for a in ("treatment", "baseline")
        ]
        scores.append(edit_scorer.score_run(record(task_id="rogue")))
        agg = edit_scorer.aggregate(scores, plan)
        self.assertFalse(agg["completeness"]["passed"])
        self.assertEqual(
            agg["completeness"]["unplanned"],
            [{"task_id": "rogue", "arm": "treatment", "seed": 7}],
        )

    def test_repetitions_are_distinct_planned_cells(self):
        plan = planned(seeds=(7, 8))
        scores = [
            edit_scorer.score_run(record(arm=a, seed=7))
            for a in ("treatment", "baseline")
        ]
        agg = edit_scorer.aggregate(scores, plan)
        self.assertEqual(agg["completeness"]["planned_cells"], 4)
        self.assertEqual(agg["completeness"]["missing_cells"], 2)
        self.assertEqual(agg["headline"]["arms"]["treatment"]["rate"], 0.5)

    def test_failure_buckets_plus_all_pass_partition_the_planned_cells(self):
        plan = planned(task_ids=("t1", "t2", "t3"))
        scores = [
            edit_scorer.score_run(record(task_id="t1", arm=a))
            for a in ("treatment", "baseline")
        ]
        scores.append(
            edit_scorer.score_run(
                record(
                    task_id="t2",
                    arm="treatment",
                    status=edit_record.STATUS_INVALID_PATCH,
                    reason="EMPTY_PATCH",
                )
            )
        )
        agg = edit_scorer.aggregate(scores, plan)
        for arm in ("treatment", "baseline"):
            summary = agg["arms"][arm]
            self.assertEqual(
                summary["all_pass"] + sum(summary["failure_reasons"].values()),
                summary["planned"],
            )


# ---------------------------------------------------------------------------
# criterion 3 -- the full matrix, gate rates, deltas, process metrics
# ---------------------------------------------------------------------------


class ReportSurface(unittest.TestCase):
    def test_matrix_has_all_eight_cells_zero_filled_and_sums_to_observed(self):
        plan = planned(task_ids=("t1", "t2"))
        scores = [
            edit_scorer.score_run(record(task_id=t, arm=a))
            for t in ("t1", "t2")
            for a in ("treatment", "baseline")
        ]
        agg = edit_scorer.aggregate(scores, plan)
        matrix = agg["arms"]["treatment"]["matrix"]
        self.assertEqual(len(matrix), 8)
        self.assertEqual(
            sorted(matrix),
            sorted(["TTT", "TTF", "TFT", "TFF", "FTT", "FTF", "FFT", "FFF"]),
        )
        self.assertEqual(matrix["TTT"], 2)
        self.assertEqual(matrix["FFF"], 0)
        self.assertEqual(sum(matrix.values()), agg["arms"]["treatment"]["observed"])

    def test_a_behaviour_only_patch_lands_in_the_TFF_matrix_cell(self):
        plan = planned(task_ids=("t1",))
        behaviour_only = record(
            task_id="t1",
            status=edit_record.STATUS_GATE_FAILED,
            reason="GATE_FAILED",
            gate_outcomes=gates(
                oracle=oracle_outcome(changed_path=(False, "PATH_OUT_OF_SCOPE")),
                conformance=conformance_outcome(False, "MATCH_ABSENT"),
            ),
        )
        scores = [
            edit_scorer.score_run(behaviour_only),
            edit_scorer.score_run(record(task_id="t1", arm="baseline")),
        ]
        agg = edit_scorer.aggregate(scores, plan)
        treatment = agg["arms"]["treatment"]
        self.assertEqual(treatment["matrix"]["TFF"], 1)
        self.assertEqual(treatment["matrix"]["TTT"], 0)
        self.assertEqual(treatment["all_pass"], 0)
        self.assertEqual(agg["headline"]["arms"]["treatment"]["rate"], 0.0)
        # the individual behaviour gate still gets its credit -- the matrix is
        # preserved, not flattened into the headline
        self.assertEqual(treatment["gates"]["behavior"]["rate"], 1.0)
        self.assertEqual(treatment["gates"]["convention"]["rate"], 0.0)
        self.assertEqual(treatment["gates"]["blast"]["rate"], 0.0)

    def test_gate_rates_use_the_planned_denominator_too(self):
        plan = planned(task_ids=("t1", "t2"))
        scores = [
            edit_scorer.score_run(record(task_id="t1", arm=a))
            for a in ("treatment", "baseline")
        ]
        agg = edit_scorer.aggregate(scores, plan)
        behavior = agg["arms"]["treatment"]["gates"]["behavior"]
        self.assertEqual(behavior["passed"], 1)
        self.assertEqual(behavior["judged"], 1)
        self.assertEqual(behavior["rate"], 0.5)

    def test_process_metrics_and_paired_deltas_are_reported(self):
        plan = planned(task_ids=("t1",))
        scores = [
            edit_scorer.score_run(
                record(arm="treatment", tool_calls=2, input_tokens=100, duration=1.0)
            ),
            edit_scorer.score_run(
                record(
                    arm="baseline",
                    tool_calls=6,
                    input_tokens=400,
                    duration=3.0,
                    status=edit_record.STATUS_GATE_FAILED,
                    reason="GATE_FAILED",
                    gate_outcomes=gates(
                        conformance=conformance_outcome(False, "MATCH_ABSENT")
                    ),
                )
            ),
        ]
        agg = edit_scorer.aggregate(scores, plan)
        self.assertEqual(agg["arms"]["treatment"]["tool_calls"]["mean"], 2)
        self.assertEqual(agg["arms"]["treatment"]["input_tokens"]["total"], 100)
        self.assertEqual(agg["arms"]["treatment"]["wall_clock_seconds"]["max"], 1.0)

        self.assertEqual(agg["pairing"]["paired_count"], 1)
        deltas = agg["deltas"]
        self.assertEqual(deltas["all_three_gates_pass_rate"], 1.0)
        self.assertEqual(deltas["convention_rate"], 1.0)
        self.assertEqual(deltas["tool_calls_mean"], -4)
        self.assertEqual(deltas["input_tokens_mean"], -300)
        self.assertEqual(deltas["wall_clock_mean"], -2.0)

    def test_aggregate_validates_against_the_versioned_schema(self):
        plan = planned(task_ids=("t1",))
        scores = [
            edit_scorer.score_run(record(arm=a)) for a in ("treatment", "baseline")
        ]
        agg = edit_scorer.aggregate(scores, plan)
        self.assertEqual(agg["schema"], edit_scorer.AGGREGATE_SCHEMA)
        edit_scorer.validate_aggregate(agg)

    def test_mixed_models_are_refused_unless_explicitly_grouped(self):
        plan = planned(task_ids=("t1",))
        a = record(arm="treatment")
        b = record(arm="baseline")
        b["model"] = "other-model"
        scores = [edit_scorer.score_run(a), edit_scorer.score_run(b)]
        with self.assertRaises(edit_scorer.IncompatibleRuns):
            edit_scorer.aggregate(scores, plan)
        edit_scorer.aggregate(scores, plan, group_by=["model"])


# ---------------------------------------------------------------------------
# criterion 4 -- four distinct failure classes, never collapsed
# ---------------------------------------------------------------------------


class FailureClasses(unittest.TestCase):
    def _bucket(self, **kwargs):
        return edit_scorer.score_run(record(**kwargs))["failure_class"]

    def test_agent_failure_is_its_own_bucket(self):
        for status, reason in (
            (edit_record.STATUS_AGENT_FAILURE, "AGENT_ERROR"),
            (edit_record.STATUS_TIMEOUT, "TIMEOUT"),
            (edit_record.STATUS_TOOL_VIOLATION, "TOOL_ISOLATION"),
        ):
            self.assertEqual(
                self._bucket(status=status, reason=reason),
                edit_scorer.FAILURE_AGENT,
                msg=status,
            )

    def test_invalid_patch_is_its_own_bucket(self):
        for reason in ("EMPTY_PATCH", "PATCH_UNAPPLIABLE"):
            self.assertEqual(
                self._bucket(status=edit_record.STATUS_INVALID_PATCH, reason=reason),
                edit_scorer.FAILURE_INVALID_PATCH,
            )

    def test_harness_failure_is_its_own_bucket(self):
        self.assertEqual(
            self._bucket(
                status=edit_record.STATUS_CONFIG_ERROR, reason="CORPUS_ABSENT"
            ),
            edit_scorer.FAILURE_HARNESS,
        )
        # a gate that could not run its command is OUR breakage, not the agent's
        self.assertEqual(
            self._bucket(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    oracle=oracle_outcome(existing_suite=(False, "COMMAND_NOT_FOUND"))
                ),
            ),
            edit_scorer.FAILURE_HARNESS,
        )

    def test_oracle_failure_is_its_own_bucket(self):
        # a non-discriminating test proves nothing about the patch
        self.assertEqual(
            self._bucket(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    oracle=oracle_outcome(discrimination=(False, "NON_DISCRIMINATING"))
                ),
            ),
            edit_scorer.FAILURE_ORACLE,
        )
        # a decoy anchor is defective convention material, not a bad patch
        self.assertEqual(
            self._bucket(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    conformance=conformance_outcome(False, "ANCHOR_DECOY")
                ),
            ),
            edit_scorer.FAILURE_ORACLE,
        )

    def test_a_genuinely_wrong_patch_is_the_only_bucket_blaming_the_agent(self):
        self.assertEqual(
            self._bucket(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    oracle=oracle_outcome(changed_path=(False, "PATH_OUT_OF_SCOPE"))
                ),
            ),
            edit_scorer.FAILURE_PATCH_REJECTED,
        )

    def test_harness_breakage_outranks_a_rejected_patch(self):
        """A cell where our tooling broke yields no evidence about the patch,
        even when another gate also rejected it."""
        self.assertEqual(
            self._bucket(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    oracle=oracle_outcome(
                        existing_suite=(False, "TOOL_FAILURE"),
                        changed_path=(False, "PATH_OUT_OF_SCOPE"),
                    )
                ),
            ),
            edit_scorer.FAILURE_HARNESS,
        )

    def test_the_four_classes_stay_distinct_in_the_breakdown(self):
        plan = planned(task_ids=("t1", "t2", "t3", "t4"), arms=("treatment",))
        scores = [
            edit_scorer.score_run(
                record(
                    task_id="t1",
                    status=edit_record.STATUS_AGENT_FAILURE,
                    reason="AGENT_ERROR",
                )
            ),
            edit_scorer.score_run(
                record(
                    task_id="t2",
                    status=edit_record.STATUS_INVALID_PATCH,
                    reason="EMPTY_PATCH",
                )
            ),
            edit_scorer.score_run(
                record(
                    task_id="t3",
                    status=edit_record.STATUS_CONFIG_ERROR,
                    reason="CORPUS_ABSENT",
                )
            ),
            edit_scorer.score_run(
                record(
                    task_id="t4",
                    status=edit_record.STATUS_GATE_FAILED,
                    reason="GATE_FAILED",
                    gate_outcomes=gates(
                        oracle=oracle_outcome(
                            discrimination=(False, "NON_DISCRIMINATING")
                        )
                    ),
                )
            ),
        ]
        breakdown = edit_scorer.aggregate(scores, plan)["arms"]["treatment"][
            "failure_reasons"
        ]
        self.assertEqual(breakdown["agent_failure"], 1)
        self.assertEqual(breakdown["invalid_patch"], 1)
        self.assertEqual(breakdown["harness_failure"], 1)
        self.assertEqual(breakdown["oracle_failure"], 1)
        self.assertEqual(breakdown["patch_rejected"], 0)


# ---------------------------------------------------------------------------
# determinism
# ---------------------------------------------------------------------------


class Determinism(unittest.TestCase):
    def _scores(self):
        return [
            edit_scorer.score_run(record(task_id=t, arm=a))
            for t in ("t2", "t1")
            for a in ("baseline", "treatment")
        ]

    def test_aggregate_is_byte_stable_regardless_of_record_order(self):
        plan = planned(task_ids=("t1", "t2"))
        forward = self._scores()
        reverse = list(reversed(forward))
        self.assertEqual(
            json.dumps(edit_scorer.aggregate(forward, plan), sort_keys=True),
            json.dumps(edit_scorer.aggregate(reverse, plan), sort_keys=True),
        )

    def test_missing_cells_and_reasons_are_sorted(self):
        plan = planned(task_ids=("t2", "t1"), seeds=(8, 7))
        agg = edit_scorer.aggregate(
            [edit_scorer.score_run(record(arm=a)) for a in ("treatment", "baseline")],
            plan,
        )
        missing = agg["completeness"]["missing"]
        keys = [(c["task_id"], c["arm"], c["seed"]) for c in missing]
        self.assertEqual(keys, sorted(keys))


if __name__ == "__main__":
    unittest.main()
