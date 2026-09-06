"""Tests for the E2.4 selection-grid analyzer (scripts/analyze_selection.py).

Hermetic: every test builds synthetic ledgers in a temp dir. No provider calls,
no committed records, no network.

Per .claude/rules/bench-harness-oracle-independence.md the gates here are
DISCRIMINATION tests, not happy-path tests: each one re-injects the exact bad
input the gate exists to catch and asserts the gate fires.

  * a gap that sits inside the run-to-run spread is NOT a lift
  * a recall gain that arrives with a neighbor-precision loss is a REGRESSION
  * provider-side failures never enter a denominator
  * a predictions file committed AFTER the records fails the ordering gate
  * a ledger with neither per-record timestamps nor a meta sidecar fails closed
  * regenerating the report twice yields byte-identical JSON

    /usr/bin/python3 -m unittest discover -s benchmarks/repo-qa/tests -t . -v
"""

import copy
import importlib.util
import json
import os
import tempfile
import unittest

BENCH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(BENCH, "scripts", "analyze_selection.py")


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_selection", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


asel = load_module()


TASKS = [
    {"id": "t-callers", "correct_tool": "callers", "control": False,
     "confusable_neighbors": ["search", "inspect_symbol"]},
    {"id": "t-search", "correct_tool": "search", "control": False,
     "confusable_neighbors": ["callers", "find_files"]},
    {"id": "t-control", "correct_tool": "index_stats", "control": True,
     "confusable_neighbors": []},
]


def record(task_id, variant, model, first, outcome="graded", **extra):
    task = next(t for t in TASKS if t["id"] == task_id)
    correct = task["correct_tool"]
    graded = outcome == "graded" and not str(first).startswith("hallucinated:")
    rec = {
        "run_key": f"{task_id}|{variant}|{model}",
        "task_id": task_id, "variant": variant, "model": model,
        "tier": "control" if task["control"] else "confusable",
        "correct_tool": correct, "outcome": outcome,
        "first_called_tool": first,
        "selected_correct": bool(graded and first == correct),
        "counts_in_matrix": graded,
        "matrix_cell": [correct, first] if graded else None,
    }
    rec.update(extra)
    return rec


def write_ledger(directory, name, records, started_at="2026-09-06T21:00:00Z",
                 meta=True):
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as handle:
        for rec in records:
            handle.write(json.dumps(rec, sort_keys=True) + "\n")
    if meta:
        with open(path.replace(".jsonl", ".meta.json"), "w",
                  encoding="utf-8") as handle:
            json.dump({"started_at": started_at}, handle)
    return path


class OrderingGateTest(unittest.TestCase):
    """The registry is worthless unless it demonstrably predates the records."""

    def test_records_started_after_the_predictions_commit_pass(self):
        problems = asel.ordering_problems(
            "2026-09-06T20:00:00Z", [{"path": "rep1", "started_at": "2026-09-06T21:00:00Z"}])
        self.assertEqual(problems, [])

    def test_a_ledger_that_predates_the_predictions_commit_fails(self):
        problems = asel.ordering_problems(
            "2026-09-06T22:00:00Z", [{"path": "rep1", "started_at": "2026-09-06T21:00:00Z"}])
        self.assertEqual(len(problems), 1)
        self.assertIn("rep1", problems[0])

    def test_a_ledger_with_no_time_evidence_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_ledger(tmp, "rep1.jsonl",
                                [record("t-callers", "a", "weak", "callers")],
                                meta=False)
            with self.assertRaises(asel.LedgerError):
                asel.load_ledger(path)

    def test_per_record_timestamps_satisfy_the_gate_without_a_sidecar(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_ledger(
                tmp, "rep1.jsonl",
                [record("t-callers", "a", "weak", "callers",
                        timestamp="2026-09-06T21:05:00Z")], meta=False)
            self.assertEqual(asel.load_ledger(path)["started_at"],
                             "2026-09-06T21:05:00Z")


class DenominatorTest(unittest.TestCase):
    def test_provider_failures_are_excluded_from_the_denominator(self):
        records = [
            record("t-callers", "a", "weak", "callers"),
            record("t-callers", "a", "weak", None, outcome="provider_error"),
            record("t-callers", "a", "weak", None, outcome="exit_1"),
            record("t-callers", "a", "weak", "hallucinated:grep",
                   outcome="hallucinated_tool"),
        ]
        rates = asel.cell_rates(records)
        cell = rates[("a", "weak", "confusable")]
        self.assertEqual(cell["denominator"], 1)
        self.assertEqual(cell["correct"], 1)
        self.assertEqual(cell["excluded"],
                         {"provider_error": 1, "exit_1": 1, "hallucinated_tool": 1})

    def test_native_first_calls_stay_in_the_denominator_as_their_own_column(self):
        records = [record("t-callers", "a", "strong", "native:bash")]
        rates = asel.cell_rates(records)
        cell = rates[("a", "strong", "confusable")]
        self.assertEqual(cell["denominator"], 1)
        self.assertEqual(cell["correct"], 0)
        self.assertEqual(cell["columns"]["native:bash"], 1)


class LiftDecisionTest(unittest.TestCase):
    """A gap inside the run-to-run spread is not a result."""

    def _verdict(self, baseline_reps, treatment_reps, neighbor_delta=0.0):
        return asel.classify_delta(
            baseline_rates=baseline_reps, treatment_rates=treatment_reps,
            neighbor_precision_delta=neighbor_delta)

    def test_a_gap_inside_the_spread_is_not_a_lift(self):
        verdict = self._verdict([0.50, 0.70, 0.60], [0.68, 0.66, 0.70])
        self.assertEqual(verdict["verdict"], "no_effect")
        self.assertGreater(verdict["spread"], verdict["delta"])

    def test_a_gap_beyond_the_spread_is_a_lift(self):
        verdict = self._verdict([0.50, 0.52, 0.51], [0.80, 0.82, 0.81])
        self.assertEqual(verdict["verdict"], "lift")

    def test_a_recall_gain_with_a_neighbor_precision_loss_is_a_regression(self):
        verdict = self._verdict([0.50, 0.52, 0.51], [0.80, 0.82, 0.81],
                                neighbor_delta=0.30)
        self.assertEqual(verdict["verdict"], "regression")
        self.assertTrue(verdict["neighbor_precision_regression"])

    def test_a_single_rep_cannot_produce_a_lift(self):
        verdict = self._verdict([0.50], [0.90])
        self.assertEqual(verdict["verdict"], "no_effect")
        self.assertEqual(verdict["spread"], None)


class NeighborPressureTest(unittest.TestCase):
    def test_a_tool_is_charged_only_on_tasks_that_name_it_as_a_neighbor(self):
        records = [
            # search wrongly called on the callers task: search is a neighbor there
            record("t-callers", "a", "weak", "search"),
            record("t-callers", "a", "weak", "callers"),
            # a correct search call on its OWN task is not neighbor pressure
            record("t-search", "a", "weak", "search"),
        ]
        pressure = asel.neighbor_pressure(records, TASKS)
        self.assertEqual(pressure[("a", "weak")]["search"],
                         {"opportunities": 2, "wrong_calls": 1, "rate": 0.5})
        self.assertNotIn("index_stats", pressure[("a", "weak")])


class PredictionDiffTest(unittest.TestCase):
    def test_a_missed_prediction_is_named_with_the_observation(self):
        predictions = {
            "global_predictions": [
                {"id": "g3-control-null", "claim": "control shows no variant effect",
                 "falsified_if": "any variant moves the control"}],
            "per_task": [
                {"task_id": "t-callers", "correct_tool": "callers",
                 "tier": "confusable", "best_variant": "d"}],
        }
        observed = {
            "per_task_best_variant": {
                "t-callers": {"weak": {"best_variant": "b", "verdict": "lift"},
                              "strong": {"best_variant": "b", "verdict": "lift"}}},
            "global_claims": {"g3-control-null": {"held": False,
                                                  "observed": "variant d moved the control by 0.33"}},
        }
        diff = asel.diff_predictions(predictions, observed)
        ids = [m["id"] for m in diff["misses"]]
        self.assertIn("g3-control-null", ids)
        self.assertIn("per_task:t-callers", ids)
        for miss in diff["misses"]:
            self.assertTrue(miss["observed"])
            self.assertTrue(miss["hypothesis"])


class ReportTest(unittest.TestCase):
    def _reps(self, tmp):
        for rep in (1, 2, 3):
            records = []
            for variant in ("a", "b", "c", "d"):
                for model in ("weak", "strong"):
                    correct_first = "callers" if variant == "d" else "search"
                    records.append(record("t-callers", variant, model, correct_first))
                    records.append(record("t-search", variant, model, "search"))
                    records.append(record("t-control", variant, model, "index_stats"))
            records.append(record("t-callers", "a", "weak", None,
                                  outcome="provider_error"))
            write_ledger(tmp, f"rep{rep}.jsonl", records,
                         started_at=f"2026-09-06T2{rep}:00:00Z")
        return sorted(os.path.join(tmp, f"rep{r}.jsonl") for r in (1, 2, 3))

    def test_report_is_deterministic_across_regeneration(self):
        with tempfile.TemporaryDirectory() as tmp:
            ledgers = self._reps(tmp)
            predictions = {"global_predictions": [], "per_task": [],
                           "grid": {"reps_planned": 3}}
            first = asel.build_report(ledgers, TASKS, predictions,
                                      predictions_commit_time="2026-09-06T20:00:00Z")
            second = asel.build_report(ledgers, TASKS, predictions,
                                       predictions_commit_time="2026-09-06T20:00:00Z")
            self.assertEqual(json.dumps(first, sort_keys=True),
                             json.dumps(second, sort_keys=True))

    def test_report_carries_confusion_matrices_control_null_and_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            ledgers = self._reps(tmp)
            report = asel.build_report(
                ledgers, TASKS, {"global_predictions": [], "per_task": []},
                predictions_commit_time="2026-09-06T20:00:00Z")
            self.assertEqual(report["reps"], 3)
            self.assertTrue(report["confusion_matrices"])
            self.assertIn("control", report["control_null"])
            self.assertEqual(report["provider_failures"]["provider_error"], 3)
            self.assertIn("boundary", report)

    def test_report_refuses_records_older_than_the_predictions_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            ledgers = self._reps(tmp)
            with self.assertRaises(asel.OrderingError):
                asel.build_report(ledgers, TASKS, {"global_predictions": [],
                                                   "per_task": []},
                                  predictions_commit_time="2026-09-07T00:00:00Z")


if __name__ == "__main__":
    unittest.main()
