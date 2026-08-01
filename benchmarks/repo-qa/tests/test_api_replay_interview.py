import importlib.util
import json
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "api_replay_interview.py"
SPEC = importlib.util.spec_from_file_location("api_replay_interview", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class InterviewPromptTest(unittest.TestCase):
    def test_prompt_blinds_oracle_and_preserves_actual_tool_call(self):
        fixture = json.loads(
            (Path(__file__).parents[1] / "api-replay/fixtures/live-search-callsite.json").read_text()
        )
        prompt = MODULE.build_prompt(fixture)
        self.assertNotIn("expected_answers", prompt)
        self.assertNotIn("examples/base/main.go:119", prompt)
        self.assertIn('\\"symbol_types\\":\\"function\\"', prompt)
        _, assistant, _ = MODULE.tool_exchange(fixture)
        self.assertNotIn("filter", assistant["tool_calls"][0]["function"]["arguments"])
        encoded_content = json.dumps(fixture["request"]["messages"][-1]["content"])[1:-1]
        self.assertIn(encoded_content, prompt)


    def test_prompt_does_not_include_recorded_final_response(self):
        fixture = {
            "tool_call_id": "call_1",
            "expected_answers": ["SECRET ORACLE"],
            "request": {"messages": [
                {"role": "user", "content": "question"},
                {"role": "assistant", "content": "", "tool_calls": [{"id": "call_1"}]},
                {"role": "tool", "tool_call_id": "call_1", "content": "{\"x\":1}"},
                {"role": "assistant", "content": "SECRET FINAL"},
            ]},
        }
        prompt = MODULE.build_prompt(fixture)
        self.assertNotIn("SECRET ORACLE", prompt)
        self.assertNotIn("SECRET FINAL", prompt)
        self.assertIn("{\\\"x\\\":1}", prompt)


    def test_parallel_tool_calls_resolve_the_issuing_assistant(self):
        fixture = {
            "tool_call_id": "call_2",
            "request": {"messages": [
                {"role": "user", "content": "question"},
                {"role": "assistant", "content": "",
                 "tool_calls": [{"id": "call_1"}, {"id": "call_2"}]},
                {"role": "tool", "tool_call_id": "call_1", "content": "{\"a\":1}"},
                {"role": "tool", "tool_call_id": "call_2", "content": "{\"b\":2}"},
            ]},
        }
        user, assistant, tool = MODULE.tool_exchange(fixture)
        self.assertEqual(user["content"], "question")
        self.assertEqual([call["id"] for call in assistant["tool_calls"]], ["call_1", "call_2"])
        self.assertEqual(tool["content"], "{\"b\":2}")

    def test_missing_issuing_assistant_fails_fast(self):
        fixture = {
            "tool_call_id": "call_1",
            "request": {"messages": [
                {"role": "user", "content": "question"},
                {"role": "tool", "tool_call_id": "call_1", "content": "{}"},
            ]},
        }
        with self.assertRaisesRegex(ValueError, "assistant"):
            MODULE.tool_exchange(fixture)


if __name__ == "__main__":
    unittest.main()
