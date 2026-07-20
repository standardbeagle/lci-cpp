import importlib.util
import copy
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

    def test_capture_promotion_requires_matching_real_tool_exchange(self):
        source = next(value for value in self.fixtures() if not value["null_control"])
        request = copy.deepcopy(source["request"])
        request["messages"][2]["tool_calls"][0]["function"]["name"] = "lci_search"
        capture = {"schema":"lci.opencode-provider-capture.v1","failure":None,
            "request":{"body":request,"body_digest":"sha256:req","headers":{
                "Accept":"*/*", "User-Agent":"captured-agent", "Authorization":"<redacted>",
                "Cookie":"<redacted>", "X-Unrelated":"drop-me"}},
            "response":{"status":200,"body_digest":"sha256:resp"}}
        spec = {"provider_api":"openai-compatible-chat-completions","recorded_model":"provider/model",
            "opencode_version":"1.4.3","corpus":"pocketbase","corpus_commit":"abc","tool":"search",
            "task_id":source["task_id"],"question":source["question"],"expected_answers":source["expected_answers"],
            "null_control":False,"renderer":source["renderer"]}
        fixture = replay.fixture_from_capture(capture,spec)
        self.assertEqual(fixture["capture_kind"],"sanitized_live_proxy")
        self.assertEqual(fixture["request_headers"], {"accept":"*/*", "user-agent":"captured-agent"})
        self.assertNotIn("redacted", json.dumps(fixture).casefold())
        capture["response"]["status"]=404
        with self.assertRaisesRegex(ValueError,"successful"): replay.fixture_from_capture(capture,spec)

    def test_capture_header_allowlist_rejects_invalid_safe_values(self):
        with self.assertRaisesRegex(ValueError, "invalid safe"):
            replay.captured_request_headers({"User-Agent": "ok\r\nAuthorization: secret"})


if __name__ == "__main__": unittest.main()
