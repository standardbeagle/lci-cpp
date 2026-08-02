import importlib.util
import json
import unittest
import tempfile
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

    def test_validation_rejects_ambiguous_with_definite_unsupported_claims(self):
        value = {"verdict": "ambiguous", "supported_claims": [],
                 "unsupported_claims": ["invented behavior"], "missing_claims": [],
                 "reason": "r", "heuristic_gap": None}
        with self.assertRaisesRegex(ValueError, "ambiguous verdict contradicts"):
            module.validate(value)

    def test_prompt_allows_grounded_intermediate_continuation(self):
        text = module.prompt({"question": "solve", "answer": "need another tool", "truth": {}})
        self.assertIn("valid intermediate response", text)
        self.assertIn("appropriate next step", text)

    def test_checkpoint_is_valid_and_replaces_prior_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.json"
            module.write_checkpoint(path, [{"cell_key": "a"}])
            module.write_checkpoint(path, [{"cell_key": "a"}, {"cell_key": "b"}])
            self.assertEqual(len(json.loads(path.read_text())["records"]), 2)

    def test_retries_malformed_structure_and_preserves_attempts(self):
        responses = iter([
            {"status": "answered", "final_answer": "not json"},
            {"status": "answered", "final_answer": json.dumps({
                "verdict": "incorrect", "supported_claims": [], "unsupported_claims": [],
                "missing_claims": ["x"], "reason": "missing", "heuristic_gap": None})}])
        result = module.adjudicate_item(
            {"cell_key": "c", "task_id": "t", "question": "q", "answer": "a", "truth": {}},
            lambda *_: next(responses), model="provider/model", headers={})
        self.assertEqual(len(result["attempts"]), 2)
        self.assertIsNotNone(result["attempts"][0]["parse_error"])
        self.assertEqual(result["judgment"]["verdict"], "incorrect")


    def test_resume_readjudicates_provider_failed_records(self):
        """judgment null (provider failure) must not count as completed on resume."""
        failed = {"cell_key": "c1", "task_id": "t", "judgment": None,
                  "attempts": [{"attempt": 1, "provider_status": "provider_quota",
                                "raw_answer": "", "parse_error": None}]}
        calls = []
        def adjudicate(item):
            calls.append(item["cell_key"])
            return {"cell_key": item["cell_key"], "task_id": item.get("task_id"),
                    "judgment": {"verdict": "incorrect"}, "attempts": []}
        records = module.drain([{"cell_key": "c1", "task_id": "t"}], [failed],
                               adjudicate, lambda records: None)
        self.assertEqual(calls, ["c1"])
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["judgment"]["verdict"], "incorrect")

    def test_resume_skips_records_with_a_real_judgment(self):
        done = {"cell_key": "c1", "task_id": "t", "judgment": {"verdict": "correct"}, "attempts": []}
        def adjudicate(item):
            raise AssertionError("completed record must not be re-adjudicated")
        records = module.drain([{"cell_key": "c1", "task_id": "t"}], [done],
                               adjudicate, lambda records: None)
        self.assertEqual(records, [done])

    def test_header_profile_requires_the_committed_headers_key(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            path.write_text(json.dumps({"headers": {"x-a": "b"}}))
            self.assertEqual(module.load_header_profile(path), {"x-a": "b"})
            path.write_text(json.dumps({"request_headers": {"x-a": "b"}}))
            with self.assertRaisesRegex(SystemExit, "must carry a 'headers' object"):
                module.load_header_profile(path)


if __name__ == "__main__":
    unittest.main()
