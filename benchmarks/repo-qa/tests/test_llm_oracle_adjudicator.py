import importlib.util
import json
import tempfile
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


EXPECTED = ["x/y.go:3"]
ITEM = {"run_key": "r1", "task_id": "t", "answer": "near line three", "expected": EXPECTED}


def verdict(kind, asserted):
    return json.dumps({"verdict": kind, "asserted_locations": asserted,
                       "unsupported_locations": [], "reason": "r", "heuristic_gap": None})


class OracleAdjudicatorRobustnessTest(unittest.TestCase):
    def test_retries_malformed_structure_then_accepts_a_valid_verdict(self):
        responses = iter([
            {"status": "answered", "final_answer": "not json"},
            {"status": "answered", "final_answer": verdict("correct", EXPECTED)}])
        record = module.adjudicate_item(ITEM, lambda *_: next(responses),
                                        model="provider/m", headers={})
        self.assertEqual(record["adjudication"]["verdict"], "correct")
        self.assertEqual(record["regression_candidate"]["required_outcome"], "accepted")
        self.assertNotIn("failure", record)

    def test_exhausted_format_retries_are_recorded_not_raised(self):
        record = module.adjudicate_item(
            ITEM, lambda *_: {"status": "answered", "final_answer": "still not json"},
            model="provider/m", headers={}, max_format_attempts=2)
        self.assertIsNone(record["adjudication"])
        self.assertEqual(record["failure"]["kind"], "malformed_adjudication")
        self.assertEqual(record["failure"]["attempts"], 2)

    def test_provider_failure_is_not_retried_as_a_format_problem(self):
        calls = []
        def complete(*args):
            calls.append(args)
            return {"status": "provider_quota", "failure": "429"}
        record = module.adjudicate_item(ITEM, complete, model="provider/m", headers={})
        self.assertEqual(len(calls), 1)
        self.assertEqual(record["provider_status"], "provider_quota")
        self.assertEqual(record["failure"], "429")

    def test_checkpoint_keeps_completed_records_and_resumes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.json"
            first = module.adjudicate_item(
                ITEM, lambda *_: {"status": "answered", "final_answer": verdict("incorrect", [])},
                model="provider/m", headers={})
            module.write_checkpoint(path, [first])
            document = json.loads(path.read_text())
            self.assertEqual(document["schema"], "lci.oracle-adjudications.v1")
            self.assertEqual(len(document["records"]), 1)
            self.assertEqual(module.item_key(document["records"][0]), module.item_key(ITEM))

    def test_resume_key_survives_a_missing_run_key(self):
        without = {k: v for k, v in ITEM.items() if k != "run_key"}
        record = module.adjudicate_item(
            without, lambda *_: {"status": "answered", "final_answer": verdict("incorrect", [])},
            model="provider/m", headers={})
        self.assertEqual(module.item_key(record), module.item_key(without))

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
