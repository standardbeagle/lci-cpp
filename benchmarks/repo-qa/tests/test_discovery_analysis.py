"""Hermetic tests for the D5 separation analyzer.

Every fixture here is synthetic: the analyzer must be provable without gopls,
an MCP server, a provider, or a corpus. The two discrimination cases the task
names explicitly are `test_gap_inside_run_to_run_variance_is_not_separation`
and `test_family_with_no_rows_is_reported_not_run`.
"""

import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(
    0,
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"),
)

import analyze_discovery as ad  # noqa: E402


def _row(family, arm, level, slug, f1, *, status="ok", precision=None, recall=None,
         role="hypothesis", task_shape="exhaustive_callers", threshold=None,
         tool_calls=1, tokens=None, wall=0.1):
    score = {
        "precision": f1 if precision is None else precision,
        "recall": f1 if recall is None else recall,
        "f1": f1,
        "correct": f1 == 1.0,
        "ungradable_reason": None if status == "ok" else "dnf",
        "reported_empty": False,
        "cited_total": 1,
        "answer_set_total": 1,
    }
    if status != "ok":
        score = {k: (None if k in ("precision", "recall", "f1", "correct") else v)
                 for k, v in score.items()}
    return {
        "schema_version": "discovery_sweep_row_v1",
        "family": family, "role": role, "task_shape": task_shape,
        "threshold": threshold, "arm": arm, "level": level, "slug": slug,
        "status": status, "dnf_reason": None if status == "ok" else "timeout",
        "answer": "", "score": score, "gradable": True, "answer_set_total": 1,
        "tool_calls": tool_calls, "tokens": tokens, "wall_seconds": wall,
    }


class AnalyzerFixture(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="d5-analysis-")
        self.addCleanup(shutil.rmtree, self.root)

    def write_run(self, name, rows, plan=None):
        run_dir = os.path.join(self.root, name)
        os.makedirs(run_dir, exist_ok=True)
        if plan is not None:
            with open(os.path.join(run_dir, "_plan.json"), "w", encoding="utf-8") as fh:
                json.dump(plan, fh)
        for row in rows:
            fname = "%s__%s__%s__%s.json" % (
                row["family"], row["arm"], row["level"], row["slug"])
            with open(os.path.join(run_dir, fname), "w", encoding="utf-8") as fh:
                json.dump(row, fh)
        return run_dir

    def registry(self, families):
        return {"registry_id": "test-registry", "families": families}

    def cohorts(self, symbols):
        return {"corpus_commit": "deadbeef", "symbols": symbols, "cohorts": {}}


THRESHOLD_F1 = {"metric": "mean_f1", "comparison": "treatment_minus_baseline",
                "operator": ">=", "value": 0.15}
THRESHOLD_PARITY = {"metric": "mean_f1", "comparison": "abs_treatment_minus_baseline",
                    "operator": "<=", "value": 0.05}


def family(fid, prediction_outcome, threshold=THRESHOLD_F1, n=2, role="hypothesis"):
    return {
        "id": fid, "role": role, "task_shape": "exhaustive_callers",
        "threshold": threshold,
        "cohort": {"n": n, "selector": {"kind": "full_pool"}},
        "prediction": {"outcome": prediction_outcome, "confidence": 0.6,
                       "mechanism": "test"},
    }


class SeparationTests(AnalyzerFixture):
    def test_gap_inside_run_to_run_variance_is_not_separation(self):
        """A mean gap smaller than the spread ACROSS runs must not be separation."""
        rows_a, rows_b = [], []
        # run A: treatment 0.90 / baseline 0.50 ; run B: treatment 0.50 / baseline 0.90
        for slug, ta, ba, tb, bb in (("s1", 0.9, 0.5, 0.5, 0.9), ("s2", 0.9, 0.5, 0.5, 0.9)):
            rows_a += [_row("fam", "treatment", "tool", slug, ta, threshold=THRESHOLD_F1),
                       _row("fam", "baseline", "tool", slug, ba, threshold=THRESHOLD_F1)]
            rows_b += [_row("fam", "treatment", "tool", slug, tb, threshold=THRESHOLD_F1),
                       _row("fam", "baseline", "tool", slug, bb, threshold=THRESHOLD_F1)]
        self.write_run("r1", rows_a)
        self.write_run("r2", rows_b)
        out = ad.analyze(self.registry([family("fam", "lci_wins")]),
                         self.cohorts([]),
                         [os.path.join(self.root, "r1"), os.path.join(self.root, "r2")])
        level = out["families"]["fam"]["levels"]["tool"]
        self.assertEqual(level["separation"], "none")
        self.assertEqual(level["cells_observed"], 2)
        self.assertTrue(level["within_variance"])
        self.assertAlmostEqual(level["delta_mean"], 0.0, places=6)
        self.assertGreater(level["delta_spread"], 0.0)

    def test_gap_above_variance_and_threshold_is_separation(self):
        runs = []
        for i, jitter in enumerate((0.0, 0.01, -0.01)):
            rows = []
            for slug in ("s1", "s2"):
                rows += [
                    _row("fam", "treatment", "tool", slug, 0.9 + jitter,
                         threshold=THRESHOLD_F1),
                    _row("fam", "baseline", "tool", slug, 0.3, threshold=THRESHOLD_F1),
                ]
            runs.append(self.write_run("r%d" % i, rows))
        out = ad.analyze(self.registry([family("fam", "lci_wins")]),
                         self.cohorts([]), runs)
        fam = out["families"]["fam"]
        level = fam["levels"]["tool"]
        self.assertEqual(level["separation"], "treatment")
        self.assertFalse(level["within_variance"])
        self.assertTrue(level["threshold_met"])
        self.assertEqual(fam["observed_outcome"]["tool"], "lci_wins")
        self.assertTrue(fam["prediction_held"]["tool"])

    def test_single_run_cannot_claim_separation(self):
        rows = []
        for slug in ("s1", "s2"):
            rows += [_row("fam", "treatment", "tool", slug, 1.0, threshold=THRESHOLD_F1),
                     _row("fam", "baseline", "tool", slug, 0.0, threshold=THRESHOLD_F1)]
        run = self.write_run("solo", rows)
        out = ad.analyze(self.registry([family("fam", "lci_wins")]),
                         self.cohorts([]), [run])
        level = out["families"]["fam"]["levels"]["tool"]
        self.assertTrue(level["variance_unmeasured"])
        self.assertFalse(level["separation_claimable"])
        self.assertEqual(level["separation"], "none")

    def test_prediction_miss_is_flagged(self):
        """Predicted parity, measured a treatment win => the prediction FAILED."""
        runs = []
        for i in range(2):
            rows = []
            for slug in ("s1", "s2"):
                rows += [_row("fam", "treatment", "tool", slug, 0.95,
                              threshold=THRESHOLD_PARITY),
                         _row("fam", "baseline", "tool", slug, 0.20,
                              threshold=THRESHOLD_PARITY)]
            runs.append(self.write_run("r%d" % i, rows))
        out = ad.analyze(
            self.registry([family("fam", "parity", threshold=THRESHOLD_PARITY)]),
            self.cohorts([]), runs)
        fam = out["families"]["fam"]
        self.assertEqual(fam["predicted_outcome"], "parity")
        self.assertEqual(fam["observed_outcome"]["tool"], "lci_wins")
        self.assertFalse(fam["prediction_held"]["tool"])
        self.assertIn("fam", [m["family"] for m in out["prediction_misses"]])


class NotRunTests(AnalyzerFixture):
    def test_family_with_no_rows_is_reported_not_run(self):
        run = self.write_run("r1", [_row("fam", "treatment", "tool", "s1", 1.0)] +
                             [_row("fam", "baseline", "tool", "s1", 1.0)])
        out = ad.analyze(
            self.registry([family("fam", "lci_wins"), family("ghost", "lci_wins")]),
            self.cohorts([]), [run],
            not_run_reasons={"ghost": "build_plan raises: cohort file lacks receiver_type"})
        ghost = out["families"]["ghost"]
        self.assertEqual(ghost["status"], "not_run")
        self.assertIn("receiver_type", ghost["not_run_reason"])
        self.assertEqual(ghost["levels"], {})
        self.assertIn("ghost", [e["family"] for e in out["not_run"]])
        # A NOT RUN family must never contribute a zero row to any aggregate.
        self.assertIsNone(ghost.get("observed_outcome"))
        self.assertNotIn("ghost", [m["family"] for m in out["prediction_misses"]])

    def test_not_run_without_a_stated_reason_still_reports_not_run(self):
        run = self.write_run("r1", [_row("fam", "treatment", "tool", "s1", 1.0)])
        out = ad.analyze(self.registry([family("ghost", "lci_wins")]),
                         self.cohorts([]), [run])
        self.assertEqual(out["families"]["ghost"]["status"], "not_run")
        self.assertTrue(out["families"]["ghost"]["not_run_reason"])


class DnfAndLevelTests(AnalyzerFixture):
    def test_dnf_rate_is_reported_and_not_scored_as_zero(self):
        rows = [
            _row("fam", "treatment", "agent", "s1", 1.0),
            _row("fam", "treatment", "agent", "s2", None, status="dnf"),
            _row("fam", "baseline", "agent", "s1", None, status="dnf"),
            _row("fam", "baseline", "agent", "s2", None, status="dnf"),
        ]
        run = self.write_run("r1", rows)
        out = ad.analyze(self.registry([family("fam", "lci_wins")]),
                         self.cohorts([]), [run])
        agent = out["families"]["fam"]["levels"]["agent"]
        self.assertEqual(agent["arms"]["treatment"]["dnf_rate_pct"], 50.0)
        self.assertEqual(agent["arms"]["baseline"]["dnf_rate_pct"], 100.0)
        # the surviving graded treatment cell is 1.0, NOT 0.5 -- DNF is not a zero
        self.assertEqual(agent["arms"]["treatment"]["mean_f1"], 1.0)
        self.assertIsNone(agent["arms"]["baseline"]["mean_f1"])

    def test_tool_correct_without_agent_lift_is_named(self):
        runs = []
        for i in range(2):
            rows = []
            for slug in ("s1", "s2"):
                rows += [
                    _row("fam", "treatment", "tool", slug, 0.95, threshold=THRESHOLD_F1),
                    _row("fam", "baseline", "tool", slug, 0.30, threshold=THRESHOLD_F1),
                    _row("fam", "treatment", "agent", slug, 0.60, threshold=THRESHOLD_F1),
                    _row("fam", "baseline", "agent", slug, 0.60, threshold=THRESHOLD_F1),
                ]
            runs.append(self.write_run("r%d" % i, rows))
        out = ad.analyze(self.registry([family("fam", "lci_wins")]),
                         self.cohorts([]), runs)
        self.assertIn("fam", out["tool_correct_no_agent_lift"])

    def test_levels_are_never_collapsed_into_one_number(self):
        rows = [_row("fam", "treatment", "tool", "s1", 1.0),
                _row("fam", "baseline", "tool", "s1", 0.0),
                _row("fam", "treatment", "agent", "s1", 0.0),
                _row("fam", "baseline", "agent", "s1", 1.0)]
        run = self.write_run("r1", rows)
        out = ad.analyze(self.registry([family("fam", "lci_wins")]),
                         self.cohorts([]), [run])
        levels = out["families"]["fam"]["levels"]
        self.assertEqual(set(levels), {"tool", "agent"})
        self.assertNotEqual(levels["tool"]["delta_mean"], levels["agent"]["delta_mean"])


class DifficultyCorrelationTests(AnalyzerFixture):
    def test_wins_above_noise_ratio_buckets(self):
        symbols = [
            {"slug": "s_hi", "name": "Hi", "path": "a.go", "line": 1, "column": 1,
             "fan_in": 1, "grep_hit_count": 40, "def_count": 1, "impl_count": 0,
             "noise_ratio": 40.0, "kind": "func"},
            {"slug": "s_lo", "name": "Lo", "path": "b.go", "line": 1, "column": 1,
             "fan_in": 10, "grep_hit_count": 10, "def_count": 1, "impl_count": 0,
             "noise_ratio": 1.0, "kind": "func"},
        ]
        rows = [_row("fam", "treatment", "tool", "s_hi", 1.0, threshold=THRESHOLD_F1),
                _row("fam", "baseline", "tool", "s_hi", 0.1, threshold=THRESHOLD_F1),
                _row("fam", "treatment", "tool", "s_lo", 0.5, threshold=THRESHOLD_F1),
                _row("fam", "baseline", "tool", "s_lo", 0.9, threshold=THRESHOLD_F1)]
        run = self.write_run("r1", rows)
        out = ad.analyze(self.registry([family("fam", "lci_wins")]),
                         self.cohorts(symbols), [run])
        buckets = out["difficulty_correlation"]["call_site_noise"]["buckets"]
        by_floor = {b["floor"]: b for b in buckets}
        self.assertEqual(by_floor[8.0]["cells"], 1)
        self.assertEqual(by_floor[8.0]["treatment_win_rate"], 1.0)
        self.assertEqual(by_floor[1.0]["cells"], 2)
        self.assertEqual(by_floor[1.0]["treatment_win_rate"], 0.5)


class SignTestTests(AnalyzerFixture):
    def test_sign_test_over_discordant_cells(self):
        rows = []
        for slug in ("a", "b", "c", "d", "e", "f"):
            rows += [_row("fam", "treatment", "tool", slug, 1.0, threshold=THRESHOLD_F1),
                     _row("fam", "baseline", "tool", slug, 0.2, threshold=THRESHOLD_F1)]
        run = self.write_run("r1", rows)
        out = ad.analyze(self.registry([family("fam", "lci_wins", n=6)]),
                         self.cohorts([]), [run])
        sign = out["families"]["fam"]["levels"]["tool"]["sign_test"]
        self.assertEqual(sign["discordant"], 6)
        self.assertEqual(sign["treatment_wins"], 6)
        self.assertAlmostEqual(sign["two_sided_p"], 0.03125, places=5)


SIGN_THRESHOLD = {
    "metric": "mean_precision", "comparison": "treatment_minus_baseline",
    "operator": ">=", "value": 0.2,
    "additional_requirement": {"kind": "sign_test_p", "value": 0.05,
                               "description": "requires >= 11/13 wins"},
    "falsified_if": "delta < 0.20 OR the sign test does not reach p<0.05.",
}
UNANIMOUS_THRESHOLD = {
    "metric": "mean_recall", "comparison": "treatment_minus_baseline",
    "operator": ">=", "value": 0.2,
    "additional_requirement": {"kind": "unanimous_cells", "value": 3,
                               "description": "unanimous over all 3 cells"},
}


class RegistryVerdictTests(AnalyzerFixture):
    """A met effect size is NOT a confirmed prediction when the registry also
    pre-registered a significance gate. `falsified_if` names both."""

    def _run(self, threshold, per_cell):
        rows = []
        for slug, (t, b) in per_cell.items():
            rows += [_row("fam", "treatment", "tool", slug, t, threshold=threshold),
                     _row("fam", "baseline", "tool", slug, b, threshold=threshold)]
        runs = [self.write_run("r%d" % i, rows) for i in range(2)]
        return ad.analyze(
            self.registry([family("fam", "lci_wins", threshold=threshold,
                                  n=len(per_cell))]),
            self.cohorts([]), runs)["families"]["fam"]["levels"]["tool"]

    def test_effect_met_but_sign_test_missed_is_not_confirmed(self):
        # 4 treatment wins, 2 baseline wins: big mean gap, p = 0.6875
        level = self._run(SIGN_THRESHOLD, {
            "a": (1.0, 0.0), "b": (1.0, 0.0), "c": (1.0, 0.0), "d": (1.0, 0.0),
            "e": (0.0, 1.0), "f": (0.0, 1.0),
        })
        self.assertTrue(level["threshold_met"])
        self.assertFalse(level["additional_requirement"]["met"])
        self.assertEqual(level["registry_verdict"],
                         "effect_met_significance_not_reached")

    def test_effect_and_sign_test_both_met_is_confirmed(self):
        level = self._run(SIGN_THRESHOLD, {
            "a": (1.0, 0.0), "b": (1.0, 0.0), "c": (1.0, 0.0),
            "d": (1.0, 0.0), "e": (1.0, 0.0), "f": (1.0, 0.0),
        })
        self.assertTrue(level["additional_requirement"]["met"])
        self.assertEqual(level["registry_verdict"], "confirmed")

    def test_effect_missed_is_falsified_regardless_of_significance(self):
        level = self._run(SIGN_THRESHOLD, {
            "a": (0.5, 0.45), "b": (0.5, 0.45), "c": (0.5, 0.45),
            "d": (0.5, 0.45), "e": (0.5, 0.45), "f": (0.5, 0.45),
        })
        self.assertFalse(level["threshold_met"])
        self.assertEqual(level["registry_verdict"], "falsified")

    def test_unanimity_requirement_fails_on_one_dissenting_cell(self):
        level = self._run(UNANIMOUS_THRESHOLD, {
            "a": (1.0, 0.0), "b": (1.0, 0.0), "c": (0.0, 1.0),
        })
        self.assertFalse(level["additional_requirement"]["met"])
        self.assertEqual(level["additional_requirement"]["kind"], "unanimous_cells")


class TokenAccountingTests(AnalyzerFixture):
    """Fixtures here are copied from a REAL bench.py row, not from a model of it.

    Source row: `.work/discovery/agent-runA/
    callers_call_site_high__treatment__agent__core_base_go_OnRecordAuthRequest_L1067C21.json`
    emits `tokens` as FLAT keys -- input/output/reasoning/cache_read/cache_write
    (bench.py flattens opencode's nested `cache` block at the step_finish sink).
    An earlier fixture invented a nested `{"cache": {...}}` object bench.py never
    produces, so it could not catch cache traffic being counted as billable spend
    (bench-harness-oracle-independence rule 5).
    """

    REAL_ROW_TOKENS = {"cache_read": 191837, "cache_write": 0,
                       "input": 35819, "output": 1755, "reasoning": 6094}

    def _means(self, treatment_tokens, baseline_tokens):
        rows = [
            _row("fam", "treatment", "agent", "s1", 1.0, tokens=treatment_tokens),
            _row("fam", "baseline", "agent", "s1", 1.0, tokens=baseline_tokens),
        ]
        run = self.write_run("r1", rows)
        out = ad.analyze(self.registry([family("fam", "lci_wins")]),
                         self.cohorts([]), [run])
        arms = out["families"]["fam"]["levels"]["agent"]["arms"]
        return arms["treatment"]["mean_tokens"], arms["baseline"]["mean_tokens"]

    def test_flat_cache_counters_are_excluded_from_billable_tokens(self):
        treatment, baseline = self._means(
            dict(self.REAL_ROW_TOKENS),
            {"cache_read": 0, "cache_write": 0,
             "input": 200, "output": 20, "reasoning": 0},
        )
        self.assertEqual(treatment, 35819 + 1755 + 6094)
        self.assertEqual(baseline, 220)

    def test_cache_heavy_arm_is_not_reported_as_the_expensive_one(self):
        """Discrimination: cache traffic must not flip which arm reads cheaper."""
        cheap_but_cache_heavy = {"input": 100, "output": 10, "reasoning": 5,
                                 "cache_read": 500000, "cache_write": 1000}
        expensive_no_cache = {"input": 900, "output": 90, "reasoning": 10,
                              "cache_read": 0, "cache_write": 0}
        treatment, baseline = self._means(cheap_but_cache_heavy, expensive_no_cache)
        self.assertEqual(treatment, 115)
        self.assertEqual(baseline, 1000)
        self.assertLess(treatment, baseline)

    def test_unrecognised_token_key_fails_loud(self):
        """A new bench.py counter must not be silently summed as billable."""
        with self.assertRaises(ValueError) as caught:
            self._means({"input": 1, "output": 1, "reasoning": 0,
                         "cache_read": 0, "cache_write": 0, "thinking": 7},
                        dict(self.REAL_ROW_TOKENS))
        self.assertIn("thinking", str(caught.exception))

    def test_nested_cache_object_is_no_longer_accepted_silently(self):
        """The shape the old fixture invented is not a shape bench.py emits."""
        with self.assertRaises(ValueError):
            self._means({"input": 100, "output": 10, "reasoning": 5,
                         "cache": {"read": 999, "write": 0}},
                        dict(self.REAL_ROW_TOKENS))


if __name__ == "__main__":
    unittest.main()
