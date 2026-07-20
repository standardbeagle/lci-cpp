import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/analyze_all_tools_replay.py"
SPEC = importlib.util.spec_from_file_location("analyze_all_tools_replay", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class AnalyzeAllToolsReplayTest(unittest.TestCase):
    def test_failure_is_queued_with_claim_reasons_and_blocks_selection(self):
        matrix = {"cases": [{"tool": "search", "scenario": "success", "output": "{}"}]}
        schema = {"task_id": "search--success", "tool": "search", "scenario": "success",
                  "question": "where", "source_output_digest": "sha256:x",
                  "required_claims": [{"id": "site", "kind": "location", "value": "a.go:1"}],
                  "forbidden_patterns": []}
        cell = {"cell_key": "c", "task_id": "search--success", "status": "answered",
                "final_answer": "somewhere", "model": "m", "arm": "a", "repetition": 1}
        report = module.analyze([cell], matrix, {"schemas": [schema]})
        self.assertEqual(report["analysis_status"], "pending_adjudication")
        self.assertEqual(report["adjudication_queue"][0]["deterministic_result"]["failed_claims"], ["site"])
        self.assertEqual(report["adjudication_queue"][0]["truth"]["source_tool_output"], "{}")
        self.assertFalse(report["selection_allowed"])


if __name__ == "__main__":
    unittest.main()
