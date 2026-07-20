import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/api_replay_ab.py"
SPEC = importlib.util.spec_from_file_location("api_replay_ab", SCRIPT)
replay = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(replay)


class ApiReplayABTest(unittest.TestCase):
    def fixtures(self):
        return [json.loads(path.read_text()) for path in sorted((ROOT / "benchmarks/repo-qa/api-replay/fixtures").glob("*.json"))]

    def test_treatment_changes_exactly_the_tool_result_content(self):
        treatment = next(value for value in self.fixtures() if not value["null_control"])
        report = replay.validate_fixture(treatment)
        self.assertEqual(report["diff_pointers"], ["/messages/3/content"])

    def test_renderer_is_fact_preserving_and_invertible(self):
        treatment = next(value for value in self.fixtures() if not value["null_control"])
        current = replay.build_request(treatment, "trace_17")
        candidate = replay.build_request(treatment, "trace_42")
        _, source = replay.tool_content_pointer(current, treatment["tool_call_id"])
        _, rendered = replay.tool_content_pointer(candidate, treatment["tool_call_id"])
        self.assertEqual(json.loads(replay.invert_candidate(rendered, treatment["renderer"])), json.loads(source))

    def test_null_control_requests_are_byte_equivalent(self):
        control = next(value for value in self.fixtures() if value["null_control"])
        report = replay.validate_fixture(control)
        self.assertEqual(report["diff_pointers"], [])
        self.assertEqual(report["current_digest"], report["candidate_digest"])

    def test_validator_rejects_a_second_request_difference(self):
        treatment = next(value for value in self.fixtures() if not value["null_control"])
        candidate = replay.build_request(treatment, "trace_42")
        candidate["temperature"] = 0.5
        current = replay.build_request(treatment, "trace_17")
        self.assertEqual(replay.diff_pointers(current, candidate), ["/messages/3/content", "/temperature"])

    def test_oracle_accepts_good_and_rejects_wrong_or_incomplete(self):
        expected = ["examples/base/main.go:119"]
        self.assertTrue(replay.grade_answers(expected, expected)["exact"])
        self.assertFalse(replay.grade_answers(["pocketbase.go:166"], expected)["exact"])
        self.assertEqual(replay.grade_answers([], expected)["recall"], 0.0)


if __name__ == "__main__": unittest.main()
