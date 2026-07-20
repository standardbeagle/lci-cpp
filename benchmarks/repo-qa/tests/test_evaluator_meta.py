import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load("build_evaluator_meta_queue", ROOT / "benchmarks/repo-qa/scripts/build_evaluator_meta_queue.py")
analyzer = load("analyze_evaluator_meta", ROOT / "benchmarks/repo-qa/scripts/analyze_evaluator_meta.py")


class EvaluatorMetaTest(unittest.TestCase):
    def setUp(self):
        self.cases = json.loads((ROOT / "benchmarks/repo-qa/evaluator-meta-v1/cases.json").read_text())

    def test_queue_is_blinded_and_complete(self):
        queue = builder.build(self.cases)
        text = json.dumps(queue)
        self.assertEqual(len(queue["adjudication_queue"]), 32)
        self.assertNotIn("gold_verdict", text)
        self.assertNotIn('"defect"', text)

    def test_bank_is_balanced_and_paired(self):
        labels = [item["gold_verdict"] for item in self.cases["cases"]]
        self.assertEqual(labels.count("correct"), 8)
        self.assertEqual(labels.count("incorrect"), 8)
        strata = {item["stratum"] for item in self.cases["cases"]}
        self.assertEqual(len(strata), 8)
        self.assertTrue(all(sum(item["stratum"] == stratum for item in self.cases["cases"]) == 2
                            for stratum in strata))

    def test_analysis_exposes_false_accepts(self):
        records = []
        for case in self.cases["cases"]:
            verdict = "correct" if case["case_id"] == "empty-absence" else case["gold_verdict"]
            for repetition in (1, 2):
                records.append({"cell_key": f"{case['case_id']}--r{repetition}",
                                "judgment": {"verdict": verdict}})
        result = analyzer.score(self.cases, [{"judge_id": "j", "records": records}])["systems"]["j"]
        self.assertEqual(result["false_accepts"], 2)
        self.assertEqual(result["exact"], 30)


if __name__ == "__main__":
    unittest.main()
