import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/semantic_location_oracle.py"
SPEC = importlib.util.spec_from_file_location("semantic_location_oracle", SCRIPT)
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)
EXPECTED = ["examples/base/main.go:119"]


class SemanticLocationOracleTest(unittest.TestCase):
    def test_accepts_common_equivalent_phrasings(self):
        answers = [
            "examples/base/main.go:119",
            "`examples/base/main.go` at line 119",
            "line 119 in examples/base/main.go",
            "examples/base/main.go (line 119)",
            "[callsite](examples/base/main.go#L119)",
        ]
        for answer in answers:
            with self.subTest(answer=answer):
                self.assertTrue(oracle.evaluate(answer, EXPECTED)["exact"])

    def test_rejects_omission_extra_wrong_and_negated_locations(self):
        for answer in ("No callsite", "pocketbase.go:166", "not examples/base/main.go:119"):
            self.assertFalse(oracle.evaluate(answer, EXPECTED)["exact"])
        self.assertFalse(oracle.evaluate("examples/base/main.go:119 and fake/x.go:7", EXPECTED)["exact"])

    def test_failures_are_queued_not_declared_incorrect(self):
        result = oracle.evaluate("It is in examples/base/main.go near 119.", EXPECTED)
        self.assertEqual(result["status"], "needs_adjudication")
        self.assertFalse(result["exact"])

    def test_adjudication_is_structured_and_self_consistent(self):
        prompt = oracle.adjudication_prompt(task="find callsite", answer="at the expected site", expected=EXPECTED)
        self.assertEqual(json.loads(prompt.split("\n", 1)[1])["candidate_answer"], "at the expected site")
        good = {"verdict": "correct", "asserted_locations": EXPECTED,
                "unsupported_locations": [], "reason": "same location", "heuristic_gap": "at-line"}
        self.assertEqual(oracle.validate_adjudication(good, EXPECTED)["verdict"], "correct")
        bad = dict(good, asserted_locations=[])
        with self.assertRaisesRegex(ValueError, "contradicts"):
            oracle.validate_adjudication(bad, EXPECTED)

    def test_empty_truth_stratum_scores_without_dividing_by_zero(self):
        # The `semantic_annotations` control stratum has an empty answer key.
        empty = oracle.evaluate("No Go source line carries an @lci annotation.", [])
        self.assertTrue(empty["exact"])
        self.assertEqual(empty["status"], "accepted")
        self.assertEqual(empty["precision"], 1.0)
        self.assertEqual(empty["recall"], 1.0)
        invented = oracle.evaluate("See forms/record_upsert.go:12", [])
        self.assertFalse(invented["exact"])
        self.assertEqual(invented["precision"], 0.0)
        self.assertEqual(invented["recall"], 0.0)
        self.assertEqual(oracle.evaluate("nothing here", EXPECTED)["precision"], 0.0)

    def test_versioned_regression_bank(self):
        bank = json.loads((ROOT / "benchmarks/repo-qa/oracle-v2/regressions.json").read_text())
        for case in bank["cases"]:
            with self.subTest(case=case["id"]):
                self.assertEqual(oracle.evaluate(case["answer"], case["expected"])["status"],
                                 case["required_outcome"])


if __name__ == "__main__":
    unittest.main()
