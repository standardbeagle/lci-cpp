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

    def test_grid_completeness_fails_closed(self):
        predictions = {"predictions": [{"tool": "only", "winner": "parity", "tier_sensitivity": "none"}]}
        variants = {"tools": [{"name": "only", "variants": [{"id": "annotated", "production_faithful": True}]}]}
        with self.assertRaises(ValueError):
            analysis.analyze([], predictions, variants, Counter(), 0)


if __name__ == "__main__":
    unittest.main()
