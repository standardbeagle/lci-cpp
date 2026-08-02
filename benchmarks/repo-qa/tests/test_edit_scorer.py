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
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
for _p in (
    os.path.join(BENCH_ROOT, "edits", "scoring"),
    os.path.join(BENCH_ROOT, "edits", "runner"),
    os.path.join(BENCH_ROOT, "edits", "conformance"),
):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import jsonschema  # noqa: E402

import edit_record  # noqa: E402
import edit_scorer  # noqa: E402

# The REAL S3.2 gate. Imported so the rule-level tests below judge the shape the
# gate actually emits rather than a hand-written imitation of it -- a hand-copied
# diagnostic string would keep passing after S3.2 reformatted the real one.
import conformance_gate  # noqa: E402


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

    def test_bank_health_match_absent_is_an_oracle_failure_not_the_agents(self):
        # MATCH_ABSENT only ever arises from BANK exemplar anchors (the
        # agent-patch verdict speaks PATCH_*): a pristine-tree anchor whose
        # evidence no longer matches is OUR defective material, exactly like
        # MATCH_AMBIGUOUS, and must never be charged to the agent.
        self.assertEqual(
            self._bucket(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    conformance=conformance_outcome(False, "MATCH_ABSENT")
                ),
            ),
            edit_scorer.FAILURE_ORACLE,
        )

    def test_an_unevaluated_api_impact_half_is_a_harness_failure(self):
        # No impacted symbols declared -> the gate honestly reports it never
        # evaluated the API half. That is OUR harness's gap, never the patch's.
        self.assertEqual(
            self._bucket(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    oracle=oracle_outcome(
                        api_impact=(False, "API_IMPACT_NOT_EVALUATED")
                    )
                ),
            ),
            edit_scorer.FAILURE_HARNESS,
        )

    def test_a_reasonless_failure_defaults_to_harness_never_the_agent(self):
        # An empty reason set on a failing attempt is a gap in OUR reporting;
        # the fail-closed default must charge us, not the patch.
        derived = {
            "behavior": {"passed": False, "judged": True, "reasons": []},
            "convention": {"passed": True, "judged": True, "reasons": []},
            "blast": {"passed": True, "judged": True, "reasons": []},
        }
        self.assertEqual(
            edit_scorer._classify_failure({"status": "edit_gate_failed"}, derived),
            edit_scorer.FAILURE_HARNESS,
        )

    def test_passing_codes_are_stripped_from_a_failing_gates_reasons_too(self):
        # SUITE_RED + a green discrimination: the failing gate's story must name
        # only the fault, not dilute it with the passing sibling's code.
        score = edit_scorer.score_run(
            record(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(
                    oracle=oracle_outcome(existing_suite=(False, "SUITE_RED"))
                ),
            )
        )
        self.assertEqual(score["gates"]["behavior"]["reasons"], ["SUITE_RED"])

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
# criterion 4 (rule-level half) -- OUR broken rule is never charged to the agent
# ---------------------------------------------------------------------------


def real_rule_level_outcome(reason):
    """A REAL conformance_gate_v1 outcome for a rule-level failure.

    Sourced from the live S3.2 gate, never hand-written: these outcomes are the
    scorer's actual input shape, and the leading-token contract the scorer parses
    is S3.2's to change. Hermetic -- the two rule shapes never touch the disk,
    and MANIFEST_ABSENT looks for a manifest under an empty temp dir.
    """
    if reason == "RULE_MALFORMED":
        # no convention.rule_id at all
        return conformance_gate.evaluate_task({"id": "t1"}, None, None)
    if reason == "RULE_UNSUPPORTED":
        return conformance_gate.evaluate_task(
            {"id": "t1", "convention": {"rule_id": "no-such-rule"}}, None, None
        )
    if reason == "MANIFEST_ABSENT":
        with tempfile.TemporaryDirectory() as empty_corpus:
            return conformance_gate.evaluate_task_in_corpus(
                {
                    "id": "t1",
                    "corpus": "next.js",
                    "manifest_ref": {"seed": 7},
                    "convention": {"rule_id": "ts-throw-typed-error"},
                },
                empty_corpus,
            )
    raise AssertionError("unknown rule-level reason " + reason)


RULE_LEVEL_REASONS = ("RULE_MALFORMED", "RULE_UNSUPPORTED", "MANIFEST_ABSENT")


class RuntimeConventionVerdict(unittest.TestCase):
    """The convention gate scores the AGENT-PATCH verdict (conformance_gate_v2).

    Scoring `conformance.passed` when that only meant "the bank's exemplars still
    conform" made a third of the headline metric independent of the agent. These
    pin that the scorer reads the v2 shape, and -- critically -- that the two
    components keep their separate provenance: a patch we rejected is
    patch_rejected, a bank WE authored wrong is not.
    """

    @staticmethod
    def _v2(agent_passed, agent_reason, bank_passed=True, bank_reason="CONFORMS"):
        return {
            "schema": "conformance_gate_v2",
            "task_id": "t1",
            "rule_id": "rule-1",
            "kind": "structural",
            "passed": agent_passed and bank_passed,
            "diagnostic": "synthetic",
            "agent_patch": {
                "schema": "conformance_agent_patch_v1",
                "passed": agent_passed,
                "reason": agent_reason,
                "diagnostic": f"{agent_reason}: synthetic",
                "regions": [
                    {
                        "path": "src/a.ts", "lines": [4, 9],
                        "passed": agent_passed, "reason": agent_reason,
                        "evidence": "f", "detail": "",
                    }
                ],
            },
            "bank_health": conformance_outcome(
                passed=bank_passed, reason=bank_reason
            ),
        }

    def test_conforming_patch_scores_the_convention_gate_green(self):
        derived = edit_scorer._gates_of(
            record(gate_outcomes=gates(
                conformance=self._v2(True, "PATCH_CONFORMS")
            ))
        )
        self.assertTrue(derived[edit_scorer.GATE_CONVENTION]["passed"])
        self.assertTrue(derived[edit_scorer.GATE_CONVENTION]["judged"])

    def test_non_conforming_patch_is_charged_to_the_agent(self):
        rec = record(
            status=edit_record.STATUS_GATE_FAILED,
            gate_outcomes=gates(
                conformance=self._v2(False, "PATCH_NONCONFORMING")
            ),
        )
        score = edit_scorer.score_run(rec)
        self.assertFalse(score["gates"][edit_scorer.GATE_CONVENTION]["passed"])
        self.assertEqual(
            score["gates"][edit_scorer.GATE_CONVENTION]["reasons"],
            ["PATCH_NONCONFORMING"],
        )
        self.assertEqual(score["failure_class"], edit_scorer.FAILURE_PATCH_REJECTED)

    def test_ungoverned_patch_is_charged_to_the_agent(self):
        score = edit_scorer.score_run(record(
            status=edit_record.STATUS_GATE_FAILED,
            gate_outcomes=gates(conformance=self._v2(False, "PATCH_UNGOVERNED")),
        ))
        self.assertEqual(score["failure_class"], edit_scorer.FAILURE_PATCH_REJECTED)

    def test_defective_bank_is_never_charged_to_the_agent(self):
        # The patch conformed; OUR exemplars are broken. That must not read as
        # evidence the model was wrong.
        score = edit_scorer.score_run(record(
            status=edit_record.STATUS_GATE_FAILED,
            gate_outcomes=gates(conformance=self._v2(
                True, "PATCH_CONFORMS",
                bank_passed=False, bank_reason="ANCHOR_MALFORMED",
            )),
        ))
        self.assertFalse(score["gates"][edit_scorer.GATE_CONVENTION]["passed"])
        self.assertEqual(
            score["gates"][edit_scorer.GATE_CONVENTION]["reasons"],
            ["ANCHOR_MALFORMED"],
        )
        self.assertEqual(score["failure_class"], edit_scorer.FAILURE_ORACLE)

    def test_absent_component_fails_closed(self):
        outcome = self._v2(False, "PATCH_NONCONFORMING")
        del outcome["agent_patch"]
        score = edit_scorer.score_run(record(
            status=edit_record.STATUS_GATE_FAILED,
            gate_outcomes=gates(conformance=outcome),
        ))
        self.assertIn(
            edit_scorer.REASON_GATE_ABSENT,
            score["gates"][edit_scorer.GATE_CONVENTION]["reasons"],
        )
        self.assertEqual(score["failure_class"], edit_scorer.FAILURE_HARNESS)

    def test_legacy_v1_outcome_is_still_scored_by_its_anchors(self):
        # Archived records predate v2; dispatch is on the schema field, so they
        # keep scoring exactly as before rather than silently reading as absent.
        score = edit_scorer.score_run(record(
            status=edit_record.STATUS_GATE_FAILED,
            gate_outcomes=gates(
                conformance=conformance_outcome(passed=False, reason="MATCH_ABSENT")
            ),
        ))
        self.assertEqual(
            score["gates"][edit_scorer.GATE_CONVENTION]["reasons"], ["MATCH_ABSENT"]
        )


class RuleLevelConformance(unittest.TestCase):
    """The anchors:[] path -- a rule WE broke must never read as a bad patch.

    A rule-level conformance failure (malformed rule, unsupported rule, absent
    manifest) is OUR harness breaking before the patch was ever judged. It emits
    NO anchors, so its reason code exists only in the diagnostic's leading token
    and the scorer must recover it from there. If that recovery is lost the cell
    silently becomes `patch_rejected` -- the one bucket that blames the agent --
    and our broken rule reads as evidence the model was wrong.
    """

    def test_the_real_gate_emits_rule_level_failures_with_no_anchors(self):
        """The precondition the fallback exists for, pinned against the REAL gate.

        Every one of these carries its reason ONLY in the diagnostic. If S3.2
        ever grows a top-level reason field or starts emitting a synthetic
        anchor, this fails and the fallback should be revisited rather than left
        parsing prose.
        """
        for reason in RULE_LEVEL_REASONS:
            outcome = real_rule_level_outcome(reason)
            with self.subTest(reason=reason):
                self.assertEqual(outcome["schema"], "conformance_gate_v1")
                self.assertFalse(outcome["passed"])
                self.assertEqual(outcome["anchors"], [])
                self.assertNotIn("reason", outcome)

    def test_the_gate_diagnostic_leads_with_the_bare_reason_code(self):
        """Pin the S3.2 boundary the scorer's split(':', 1)[0] parse depends on.

        Asserted as an exact leading token -- not a substring -- so an S3.2
        diagnostic reformat (a prefix, a wrapped code, a different separator)
        breaks here LOUDLY instead of silently degrading every rule-level cell to
        REASON_UNCLASSIFIED. The proper fix is a top-level `reason` field on
        conformance_gate_v1; until S3.2 carries one, this test is the contract.
        """
        for reason in RULE_LEVEL_REASONS:
            diagnostic = real_rule_level_outcome(reason)["diagnostic"]
            with self.subTest(reason=reason):
                self.assertEqual(diagnostic.split(":", 1)[0], reason)
                # ...and the scorer's own parse agrees with the gate's emission
                self.assertEqual(
                    edit_scorer._reason_from_diagnostic(diagnostic), reason
                )

    def test_a_rule_level_conformance_failure_is_a_HARNESS_fault(self):
        """The discrimination test for the anchors:[] fallback.

        Fails if the diagnostic fallback is removed: with no anchors to read a
        reason from, the convention gate reports zero reasons, and a reasonless
        failure classifies as `patch_rejected` -- charging our own broken rule to
        the agent.
        """
        for reason in RULE_LEVEL_REASONS:
            score = edit_scorer.score_run(
                record(
                    status=edit_record.STATUS_GATE_FAILED,
                    reason="GATE_FAILED",
                    gate_outcomes=gates(
                        conformance=real_rule_level_outcome(reason)
                    ),
                )
            )
            with self.subTest(reason=reason):
                # the rule-level code survives the anchorless outcome...
                self.assertEqual(score["gates"]["convention"]["reasons"], [reason])
                # ...so the cell is bucketed as OUR breakage...
                self.assertEqual(
                    score["failure_class"], edit_scorer.FAILURE_HARNESS
                )
                # ...and never as evidence against the agent.
                self.assertNotEqual(
                    score["failure_class"], edit_scorer.FAILURE_PATCH_REJECTED
                )
                self.assertFalse(score["all_gates_passed"])

    def test_an_unrecognised_diagnostic_head_still_fails_to_HARNESS(self):
        """The fallback's own fail-safe: an unmappable code is OUR stale mapping,
        so it degrades to harness -- never to blaming the agent."""
        outcome = conformance_outcome(False, "MATCH_ABSENT")
        outcome["anchors"] = []
        outcome["diagnostic"] = "SOME_FUTURE_CODE: a reason this scorer predates"
        score = edit_scorer.score_run(
            record(
                status=edit_record.STATUS_GATE_FAILED,
                reason="GATE_FAILED",
                gate_outcomes=gates(conformance=outcome),
            )
        )
        self.assertEqual(
            score["gates"]["convention"]["reasons"],
            [edit_scorer.REASON_UNCLASSIFIED],
        )
        self.assertEqual(score["failure_class"], edit_scorer.FAILURE_HARNESS)


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


# ---------------------------------------------------------------------------
# criterion 5 -- goldens
# ---------------------------------------------------------------------------

GOLDEN_PATH = os.path.join(
    BENCH_ROOT, "edits", "scoring", "goldens", "three-gate-report.golden.json"
)


def _reference_headline(records, plan, arm):
    """Recompute the headline from the RAW records, independently of the scorer.

    Deliberately duplicates the conjunction by reading the gate outcomes
    straight out of the JSON: it shares no code with edit_scorer, so if the
    scorer's derivation drifts (a dropped sub-gate, a missing cell quietly
    leaving the denominator) this disagrees instead of moving with it. The
    denominator is the plan; an absent record is simply not a pass.
    """
    by_cell = {(r["task_id"], r["arm"], r["seed"]): r for r in records}
    cells = [c for c in plan if c["arm"] == arm]
    passed = 0
    for cell in cells:
        rec = by_cell.get((cell["task_id"], cell["arm"], cell["seed"]))
        gate_outcomes = (rec or {}).get("gates")
        if not gate_outcomes:
            continue
        oracle = gate_outcomes.get("oracle")
        conformance = gate_outcomes.get("conformance")
        if not oracle or not conformance:
            continue
        if (
            oracle["existing_suite"]["passed"]
            and oracle["discrimination"]["passed"]
            and oracle["changed_path"]["passed"]
            and oracle["api_impact"]["passed"]
            and conformance["passed"]
        ):
            passed += 1
    return passed, len(cells), (passed / len(cells) if cells else None)


class GoldenReport(unittest.TestCase):
    def setUp(self):
        with open(GOLDEN_PATH, encoding="utf-8") as handle:
            self.golden = json.load(handle)
        self.plan = self.golden["planned"]
        self.records = self.golden["records"]
        self.report = edit_scorer.aggregate(
            [edit_scorer.score_run(r) for r in self.records], self.plan
        )

    def test_golden_report_matches_the_hand_pinned_expectations(self):
        expected = self.golden["expected"]
        edit_scorer.validate_aggregate(self.report)
        for field, value in expected["completeness"].items():
            self.assertEqual(self.report["completeness"][field], value, msg=field)
        for arm, want in expected["headline"].items():
            self.assertEqual(self.report["headline"]["arms"][arm], want, msg=arm)
        for arm, want in expected["gate_rates"].items():
            got = {
                name: self.report["arms"][arm]["gates"][name]["rate"]
                for name in edit_scorer.GATE_NAMES
            }
            self.assertEqual(got, want, msg=arm)
        for arm, want in expected["matrix"].items():
            self.assertEqual(self.report["arms"][arm]["matrix"], want, msg=arm)
        for arm, want in expected["failure_reasons"].items():
            self.assertEqual(
                self.report["arms"][arm]["failure_reasons"], want, msg=arm
            )
        self.assertEqual(self.report["deltas"], expected["deltas"])

    def test_golden_headline_recomputes_independently_from_the_raw_records(self):
        """The headline is re-derived from the records by a separate path --
        never read back from a stored value."""
        for arm in ("treatment", "baseline"):
            passed, planned_n, rate = _reference_headline(self.records, self.plan, arm)
            headline = self.report["headline"]["arms"][arm]
            self.assertEqual(headline["all_pass"], passed, msg=arm)
            self.assertEqual(headline["planned"], planned_n, msg=arm)
            self.assertEqual(headline["rate"], rate, msg=arm)

    def test_golden_behaviour_only_patch_does_NOT_score_as_fully_correct(self):
        """The load-bearing golden: a patch that makes the tests green while
        breaking the convention and the blast radius is not a win."""
        trap = self.golden["expected"]["_behaviour_only_trap"]
        cell = trap["cell"]
        raw = next(
            r
            for r in self.records
            if (r["task_id"], r["arm"], r["seed"])
            == (cell["task_id"], cell["arm"], cell["seed"])
        )
        score = edit_scorer.score_run(raw)

        # it really does pass behaviour...
        self.assertTrue(score["gates"]["behavior"]["passed"])
        # ...and is still not fully correct
        self.assertFalse(score["all_gates_passed"])
        self.assertEqual(score["matrix_cell"], trap["matrix_cell"])

        # and it cannot reach the headline through the aggregate either
        baseline = self.report["arms"]["baseline"]
        self.assertEqual(self.report["headline"]["arms"]["baseline"]["rate"], 0.0)
        self.assertEqual(baseline["matrix"]["TFF"], 1)

        # the gap this metric exists to expose: a behaviour-only rate would have
        # reported this arm at 0.5 rather than 0.0
        self.assertEqual(baseline["gates"]["behavior"]["rate"], 0.5)
        self.assertGreater(
            baseline["gates"]["behavior"]["rate"],
            self.report["headline"]["arms"]["baseline"]["rate"],
        )

    def test_golden_report_is_byte_stable_across_record_order(self):
        shuffled = list(reversed(self.records))
        again = edit_scorer.aggregate(
            [edit_scorer.score_run(r) for r in shuffled], list(reversed(self.plan))
        )
        self.assertEqual(
            json.dumps(self.report, sort_keys=True),
            json.dumps(again, sort_keys=True),
        )


if __name__ == "__main__":
    unittest.main()
