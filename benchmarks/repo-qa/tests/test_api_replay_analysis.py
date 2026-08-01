import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "benchmarks/repo-qa/api-replay/format-exploration"
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/api_replay_analysis.py"
SPEC = importlib.util.spec_from_file_location("api_replay_analysis", SCRIPT)
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)
MANIFEST = json.loads((BASE / "manifest.json").read_text())
SCHEDULE = json.loads((BASE / "schedule.json").read_text())
ORACLES = analysis.load_oracles(BASE)
GOOD = "examples/base/main.go:119"
PREFLIGHT = {"schema": "lci.api-replay.format-plan.v1", "provider_executed": False, "cells": 64,
    "fixtures": [{"task_id": "search-callsite-v2", "recorded_model": model["id"],
        "fixture_digest": ORACLES[model["id"]]["fixture_digest"],
        "arms": {arm["id"]: {"diff_pointers": [] if arm["id"] == "fmt_07" else ["/messages/3/content"],
                            "request_digest": "sha256:req", "content_digest": "sha256:content"}
                 for arm in MANIFEST["arms"]}}
        for index, model in enumerate(MANIFEST["models"], 1)]}


def complete_grid(answer_for=None):
    cells = []
    for block in SCHEDULE["blocks"]:
        row = SCHEDULE["arm_order_rows"][block["arm_order_row"] - 1]
        for model in block["model_order"]:
            for order, arm in enumerate(row, 1):
                answer = answer_for(model, arm, block["block"]) if answer_for else GOOD
                cells.append({"schema": "lci.api-replay.format-cell.v1", "task_id": "search-callsite-v2",
                    "model": model, "arm": arm, "repetition": block["block"], "order": order,
                    "attempt": 1, "status": "answered", "final_answer": answer, "failure": None})
    return cells


class ApiReplayAnalysisTest(unittest.TestCase):
    def test_fixture_loader_uses_matching_v2_fixture_per_model(self):
        self.assertEqual(set(ORACLES), {item["id"] for item in MANIFEST["models"]})
        self.assertTrue(all(item["task_id"] == "search-callsite-v2" for item in ORACLES.values()))

    def test_oracle_discriminates_good_wrong_omitted_and_extra(self):
        base = complete_grid()[0]
        def scored(answer):
            raw = dict(base, final_answer=answer)
            return analysis.score_cell(raw, ORACLES)
        self.assertEqual((scored(GOOD)["exact"], scored(GOOD)["precision"], scored(GOOD)["recall"]),
                         (True, 1.0, 1.0))
        self.assertFalse(scored("pocketbase.go:166")["exact"])
        self.assertEqual(scored("No callsite.")["recall"], 0.0)
        extra = scored(GOOD + " and fake/other.go:7")
        self.assertEqual((extra["exact"], extra["precision"], extra["recall"]), (False, 0.5, 1.0))

    def test_quality_uses_the_standard_ir_empty_convention(self):
        both_empty = analysis.quality([], [])
        self.assertEqual((both_empty["precision"], both_empty["recall"]), (1.0, 1.0))
        empty_prediction = analysis.quality([], [GOOD])
        self.assertEqual((empty_prediction["precision"], empty_prediction["recall"]), (0.0, 0.0))

    def test_oracle_accepts_semantically_equivalent_file_at_line_wording(self):
        raw = dict(complete_grid()[0],
                   final_answer="Called in `examples/base/main.go` at line 119 inside main.")
        score = analysis.score_cell(raw, ORACLES)
        self.assertTrue(score["exact"])
        self.assertEqual(score["adjudication_status"], "accepted")

    def test_every_heuristic_failure_is_emitted_for_adjudication(self):
        cells = complete_grid()
        cells[0]["final_answer"] = "The call is near the end of the example."
        report = analysis.analyze({"cells": cells}, ORACLES, MANIFEST, SCHEDULE, PREFLIGHT)
        self.assertTrue(report["oracle_followup_required"])
        self.assertEqual(report["analysis_status"], "pending_adjudication")
        self.assertEqual(report["selection"]["decision"], "defer: pending adjudication")
        self.assertEqual(len(report["adjudication_queue"]), 1)
        self.assertEqual(report["adjudication_queue"][0]["answer"], cells[0]["final_answer"])

    def test_exact_binomial_intervals_and_mcnemar_known_values(self):
        self.assertAlmostEqual(analysis.exact_binomial_ci(0, 8)[1], 0.3694166476)
        self.assertAlmostEqual(analysis.exact_binomial_ci(8, 8)[0], 0.6305833524)
        self.assertEqual(analysis.exact_mcnemar(0, 0), 1.0)
        self.assertEqual(analysis.exact_mcnemar(0, 8), 0.0078125)

    def test_holm_is_step_down_monotone_and_family_aware(self):
        raw = {("m", str(i)): p for i, p in enumerate((0.001, 0.01, 0.02, 0.2, 0.5, 1.0))}
        adjusted = analysis.holm_adjust(raw)
        self.assertEqual([adjusted[("m", str(i))] for i in range(6)],
                         [0.006, 0.05, 0.08, 0.6000000000000001, 1.0, 1.0])

    def test_complete_grid_reports_six_contrasts_and_cycle_null(self):
        report = analysis.analyze({"cells": complete_grid()}, ORACLES, MANIFEST, SCHEDULE, PREFLIGHT)
        self.assertTrue(report["grid_complete"])
        self.assertEqual(report["observed_logical_cells"], 64)
        self.assertEqual(sum(len(value) for value in report["paired_contrasts_by_model"].values()), 6)
        self.assertTrue(all(value["passes"] for value in report["cycle_null"].values()))
        for model in report["by_model_and_arm"].values():
            for arm in model.values():
                self.assertEqual(arm["usable_n"], 8)
                self.assertIsNotNone(arm["exact_binomial_95_ci"])

    def test_failures_missing_and_retries_are_reported_without_zero_scoring(self):
        cells = complete_grid()
        failed = cells.pop()
        replacement = cells[-1]
        cells.append(dict(replacement, attempt=0, status="provider_timeout", final_answer="",
                          failure={"kind": "timeout"}))
        report = analysis.analyze({"cells": cells}, ORACLES, MANIFEST, SCHEDULE, PREFLIGHT)
        self.assertFalse(report["grid_complete"])
        self.assertEqual(len(report["missing_cells"]), 1)
        self.assertEqual(report["retry_attempts"], 1)
        self.assertEqual([item["status"] for item in report["retries"][0]["attempts"]],
                         ["answered", "provider_timeout"])
        score = next(s for s in report["scores"] if s["model"] == replacement["model"] and
                     s["arm"] == replacement["arm"] and s["repetition"] == replacement["repetition"])
        self.assertTrue(score["completed"])
        self.assertTrue(score["exact"])
        self.assertNotIn(failed, report["scores"])

    def test_unresolved_failure_is_unscored_and_in_completion_denominator(self):
        cells = complete_grid()
        cells[0].update(status="provider_quota", final_answer="", failure={"kind": "quota"})
        report = analysis.analyze({"cells": cells}, ORACLES, MANIFEST, SCHEDULE, PREFLIGHT)
        self.assertEqual(len(report["unusable_cells"]), 1)
        score = next(s for s in report["scores"] if not s["completed"])
        self.assertIsNone(score["exact"])
        summary = report["by_model_and_arm"][score["model"]][score["arm"]]
        self.assertEqual(summary["completion_rate"], 7 / 8)
        self.assertEqual(summary["usable_n"], 7)

    def test_frozen_selection_rule_is_applied_mechanically(self):
        strong = MANIFEST["models"][0]["id"]
        def answers(model, arm, block):
            if model == strong and arm == "fmt_19":
                return GOOD
            return "No callsite."
        report = analysis.analyze({"cells": complete_grid(answers)}, ORACLES, MANIFEST, SCHEDULE, PREFLIGHT)
        self.assertEqual(report["selection"]["advanced"], [])
        self.assertEqual(report["selection"]["decision"], "defer: pending adjudication")
        self.assertFalse(report["selection"]["production_recommendation"])
        broken = json.loads(json.dumps(PREFLIGHT))
        broken["fixtures"][0]["arms"]["fmt_19"]["diff_pointers"] = ["/temperature"]
        with self.assertRaisesRegex(analysis.AnalysisError, "request-isolation"):
            analysis.analyze({"cells": complete_grid(answers)}, ORACLES, MANIFEST, SCHEDULE, broken)
        incomplete = complete_grid(answers)[:-1]
        blocked = analysis.analyze({"cells": incomplete}, ORACLES, MANIFEST, SCHEDULE, PREFLIGHT)
        self.assertEqual(blocked["selection"]["advanced"], [])
        self.assertEqual(blocked["selection"]["decision"], "defer: pending adjudication")

    def test_wrong_task_order_and_duplicate_answer_fail_closed(self):
        cells = complete_grid()
        with self.assertRaisesRegex(analysis.AnalysisError, "schedule order mismatch"):
            analysis.analyze({"cells": [dict(cells[0], order=99)]}, ORACLES, MANIFEST, SCHEDULE, PREFLIGHT)
        with self.assertRaisesRegex(analysis.AnalysisError, "answered cell was retried"):
            analysis.analyze({"cells": [cells[0], dict(cells[0], attempt=2)]}, ORACLES, MANIFEST, SCHEDULE, PREFLIGHT)

    def test_result_directory_loads_immutable_attempts(self):
        with tempfile.TemporaryDirectory() as temporary:
            attempts = Path(temporary) / "attempts"
            attempts.mkdir()
            (attempts / "b.json").write_text(json.dumps({"attempt": 2}))
            (attempts / "a.json").write_text(json.dumps({"attempt": 1}))
            self.assertEqual(
                [item["attempt"] for item in analysis.load_cells(Path(temporary))["cells"]],
                [1, 2],
            )


if __name__ == "__main__":
    unittest.main()
