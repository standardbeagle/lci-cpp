import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/analyze_all_tools_replay.py"
SPEC = importlib.util.spec_from_file_location("analyze_all_tools_replay", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class AnalyzeAllToolsReplayTest(unittest.TestCase):
    def test_success_atoms_and_empty_answers(self):
        surface = {"answer": ["Execute", "Start"]}
        case = {"scenario": "success", "output": "{}", "is_error": False}
        self.assertTrue(module.score_answer("Start and Execute", case, surface)["heuristic_pass"])
        self.assertFalse(module.score_answer("Start", case, surface)["heuristic_pass"])
        self.assertTrue(module.score_answer("There are no annotations.", case, {"answer": []})["heuristic_pass"])

    def test_negative_classes_are_distinct(self):
        self.assertEqual(module.negative_class({"is_error": True, "output": "{}"}), "error")
        self.assertEqual(module.negative_class({"is_error": False, "output": '{"results":[]}'}),
                         "empty_or_not_found")
        self.assertEqual(module.negative_class({"is_error": False, "output": "LCF/1.0 data"}),
                         "corrective_or_fallback")

    def test_any_heuristic_failure_is_queued_and_blocks_completion(self):
        matrix = {"cases": [{"tool": "search", "scenario": "success", "output": "{}", "is_error": False}]}
        surface = {"tools": [{"name": "search", "question": "where", "answer": ["a.go:1"]}]}
        cell = {"cell_key": "c", "task_id": "search--success", "status": "answered",
                "final_answer": "somewhere", "model": "m", "arm": "a", "repetition": 1}
        report = module.analyze([cell], matrix, surface)
        self.assertEqual(report["analysis_status"], "pending_adjudication")
        self.assertEqual(len(report["adjudication_queue"]), 1)
        self.assertFalse(report["selection_allowed"])


if __name__ == "__main__":
    unittest.main()
