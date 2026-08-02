import importlib.util
import unittest
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/analyze_comprehension.py"
SPEC = importlib.util.spec_from_file_location("analyze_comprehension", SCRIPT)
analysis = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(analysis)


class ComprehensionAnalysisTest(unittest.TestCase):
    def test_winner_respects_noise_floor(self):
        self.assertEqual(analysis.winner(0.01, 0.0), "parity")
        self.assertEqual(analysis.winner(0.20, 0.05), "annotated")
        self.assertEqual(analysis.winner(-0.20, 0.05), "current")
        self.assertEqual(analysis.winner(0.04, 0.10), "parity")

    def test_metric_reports_effect_and_run_variance(self):
        measured = analysis.metric([0.0, 1.0])
        self.assertEqual(measured["n"], 2)
        self.assertEqual(measured["mean"], 0.5)
        self.assertEqual(measured["stdev"], 0.5)

    def test_noise_is_pooled_within_cells_not_across_format_arms(self):
        self.assertEqual(analysis.pooled_within_cell_stdev([[0.0, 0.0], [1.0, 1.0]]), 0.0)
        self.assertEqual(analysis.pooled_within_cell_stdev([[0.0, 1.0], [0.0, 1.0]]), 0.5)

    def test_duplicate_logical_grid_cell_fails_closed(self):
        predictions = {"predictions": [{"tool": "only", "winner": "parity", "tier_sensitivity": "none"}]}
        variants = {"tools": [{"name": "only", "variants": [{"id": "annotated", "production_faithful": True}]}]}
        row = {"tool": "only", "variant": "current", "model": "opencode/deepseek-v4-flash-free", "repetition": 1}
        with self.assertRaisesRegex(ValueError, "duplicate logical grid cell"):
            analysis.analyze([row, dict(row)], predictions, variants, Counter(), 0)

    def test_grid_completeness_fails_closed(self):
        predictions = {"predictions": [{"tool": "only", "winner": "parity", "tier_sensitivity": "none"}]}
        variants = {"tools": [{"name": "only", "variants": [{"id": "annotated", "production_faithful": True}]}]}
        with self.assertRaises(ValueError):
            analysis.analyze([], predictions, variants, Counter(), 0)


MODELS = {"opencode/deepseek-v4-flash-free": "weak", "opencode-go/glm-5.2": "strong"}


def _grid(exact_by_variant, drop=None):
    rows = []
    for variant, exact in exact_by_variant.items():
        for model, tier in MODELS.items():
            for rep in (1, 2):
                row = {"tool": "only", "variant": variant, "model": model, "repetition": rep,
                       "capability_tier": tier, "status": "answered",
                       "score": {"exact": exact, "precision": 1.0, "recall": 1.0,
                                 "false_positive_count": 0}}
                if drop == (variant, model, rep):
                    row["status"] = "provider_error"
                    row["score"] = None
                rows.append(row)
    return rows


PREDICTIONS = {"predictions": [{"tool": "only", "winner": "annotated", "tier_sensitivity": "none"}]}
VARIANTS = {"tools": [{"name": "only", "variants": [{"id": "annotated", "production_faithful": True}]}]}


class DerivedNarrativeTest(unittest.TestCase):
    """Report prose must be computed from the analyzed grid, not frozen from one run."""

    def test_generality_and_caveat_are_derived_from_this_grid(self):
        report = analysis.analyze(_grid({"current": 0.0, "annotated": 1.0}),
                                  PREDICTIONS, VARIANTS, Counter(), 0)
        verdict = report["surface_verdict"]
        self.assertIn("1 of 1", verdict["generality"])
        self.assertIn("only", verdict["generality"])
        for stale in ("fileblob", "browse_file", "4 of 14"):
            self.assertNotIn(stale, verdict["generality"])
        self.assertNotIn("code_insight", verdict["variance_caveat"])
        self.assertEqual(verdict["arms_with_unscored_repetitions"], [])

    def test_unscored_repetitions_are_reported_from_the_data(self):
        report = analysis.analyze(
            _grid({"current": 0.0, "annotated": 1.0},
                  drop=("annotated", "opencode-go/glm-5.2", 2)),
            PREDICTIONS, VARIANTS, Counter(), 0)
        verdict = report["surface_verdict"]
        self.assertEqual(verdict["arms_with_unscored_repetitions"], ["only/annotated/strong"])
        self.assertIn("only/annotated/strong", verdict["variance_caveat"])

    def test_control_investigation_is_not_pinned_to_one_tool(self):
        variants = {"tools": [{"name": "only", "control": True,
                               "variants": [{"id": "annotated", "production_faithful": True}]}]}
        report = analysis.analyze(_grid({"current": 0.0, "annotated": 1.0}),
                                  PREDICTIONS, variants, Counter(), 0)
        finding = report["findings"][0]
        self.assertTrue(finding["control_suspect"])
        self.assertIsNotNone(finding["control_investigation"])
        self.assertNotIn("info blob", finding["control_investigation"])


if __name__ == "__main__":
    unittest.main()
