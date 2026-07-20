import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/llm_oracle_adjudicator.py"
SPEC = importlib.util.spec_from_file_location("llm_oracle_adjudicator", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class LlmOracleAdjudicatorTest(unittest.TestCase):
    def test_queues_every_answered_heuristic_failure(self):
        cells = [
            {"status": "answered", "model": "m", "task_id": "t", "final_answer": "x/y.go:3"},
            {"status": "answered", "model": "m", "task_id": "t", "final_answer": "near line three"},
            {"status": "provider_error", "model": "m", "task_id": "t", "final_answer": ""},
        ]
        failures = module.provisional_failures(cells, {"m": ["x/y.go:3"]})
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["answer"], "near line three")

    def test_request_is_blinded_and_deterministic(self):
        body = module.request_body("opencode-go/glm-5.2", "prompt")
        self.assertEqual(body["model"], "glm-5.2")
        self.assertEqual(body["temperature"], 0)
        self.assertNotIn("arm", str(body).lower())

    def test_parses_plain_or_fenced_json_only(self):
        self.assertEqual(module.parse_json_answer('{"verdict":"correct"}')["verdict"], "correct")
        self.assertEqual(module.parse_json_answer('```json\n{"verdict":"incorrect"}\n```')["verdict"], "incorrect")
        with self.assertRaises(Exception):
            module.parse_json_answer("The answer is correct")


if __name__ == "__main__":
    unittest.main()
