import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/llm_tool_result_adjudicator.py"
SPEC = importlib.util.spec_from_file_location("llm_tool_result_adjudicator", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class LlmToolResultAdjudicatorTest(unittest.TestCase):
    def test_prompt_is_blind_and_contains_truth(self):
        text = module.prompt({"question": "q", "answer": "a", "expected": {"kind": "error"}})
        payload = json.loads(text.split("\n", 1)[1])
        self.assertEqual(payload["independent_truth"]["kind"], "error")
        self.assertNotIn("arm", text.casefold())

    def test_validation_rejects_self_contradictory_correct_verdict(self):
        value = {"verdict": "correct", "supported_claims": [], "unsupported_claims": ["x"],
                 "missing_claims": [], "reason": "r", "heuristic_gap": None}
        with self.assertRaisesRegex(ValueError, "contradicts"):
            module.validate(value)


if __name__ == "__main__":
    unittest.main()
