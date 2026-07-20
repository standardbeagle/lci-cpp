import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/aggregate_tool_adjudications.py"
SPEC = importlib.util.spec_from_file_location("aggregate_tool_adjudications", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class AggregateToolAdjudicationsTest(unittest.TestCase):
    def test_only_agreement_resolves_and_never_overrides_score(self):
        left = {"judge_id": "a", "records": [
            {"cell_key": "x", "judgment": {"verdict": "correct"}},
            {"cell_key": "y", "judgment": {"verdict": "incorrect"}}]}
        right = {"judge_id": "b", "records": [
            {"cell_key": "x", "judgment": {"verdict": "correct"}},
            {"cell_key": "y", "judgment": {"verdict": "ambiguous"}}]}
        queue = [{"cell_key": "x", "answer": "a", "truth": {}},
                 {"cell_key": "y", "answer": "b", "truth": {}}]
        result = module.aggregate(left, right, queue)
        self.assertEqual(result["records"][0]["verdict"], "correct")
        self.assertEqual(result["records"][1]["verdict"], "unresolved")
        self.assertFalse(result["score_override_allowed"])
        self.assertEqual(len(result["regression_candidates"]), 1)

    def test_same_judge_id_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "independent"):
            module.aggregate({"judge_id": "a"}, {"judge_id": "a"}, [])


if __name__ == "__main__":
    unittest.main()
