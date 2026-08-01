import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/api_replay_all_tools.py"
SPEC = importlib.util.spec_from_file_location("api_replay_all_tools", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)
BASE = ROOT / "benchmarks/repo-qa/api-replay/format-exploration-v2"


class ApiReplayAllToolsTest(unittest.TestCase):
    def test_derivation_replaces_complete_exchange_and_preserves_base(self):
        base = json.loads((BASE / "fixture-deepseek-search.json").read_text())
        original = json.loads(json.dumps(base))
        case = {"tool": "find_files", "scenario": "negative", "arguments": {"pattern": "missing"},
                "output": '{"results":[]}'}
        surface = {"question": "Which files?", "answer": ["a.go"]}
        fixture = module.derive_fixture(base, case, surface)
        self.assertEqual(base, original)
        self.assertEqual(fixture["task_id"], "find_files--negative")
        self.assertIn("lci_find_files", fixture["request"]["messages"][-2]["tool_calls"][0]["function"]["name"])
        self.assertEqual(fixture["request"]["messages"][-1]["content"], '{"results":[]}')
        self.assertNotIn("app.Start", json.dumps(fixture["request"]["messages"][-3:]))

    def test_full_plan_is_balanced_and_has_448_cells(self):
        bases = [json.loads((BASE / name).read_text()) for name in
                 ("fixture-deepseek-search.json", "fixture-glm-search.json")]
        matrix = {"case_count": 28, "cases": [
            {"tool": f"tool{i:02d}", "scenario": scenario, "arguments": {}, "output": "{}"}
            for i in range(14) for scenario in ("success", "negative")]}
        surface = {"tools": [{"name": f"tool{i:02d}", "question": "q", "answer": ["a"]}
                             for i in range(14)]}
        fixtures = module.build_fixtures(matrix, surface, bases)
        planned = module.jobs(fixtures)
        self.assertEqual(len(fixtures), 56)
        self.assertEqual(len(planned), 448)
        counts = {arm: sum(job[2] == arm for job in planned) for arm in module.replay.ARMS}
        self.assertEqual(set(counts.values()), {112})

    def test_derivation_resolves_the_issuing_assistant_under_parallel_tool_calls(self):
        base = json.loads((BASE / "fixture-deepseek-search.json").read_text())
        messages = base["request"]["messages"]
        issuing = messages[2]
        issuing["tool_calls"] = [dict(issuing["tool_calls"][0], id="call_sibling"),
                                 issuing["tool_calls"][0]]
        messages.insert(3, {"role": "tool", "tool_call_id": "call_sibling", "content": "{}"})
        case = {"tool": "find_files", "scenario": "negative", "arguments": {"pattern": "missing"},
                "output": '{"results":[]}'}
        fixture = module.derive_fixture(base, case, {"question": "q", "answer": ["a"]})
        derived = fixture["request"]["messages"]
        self.assertEqual(derived[2]["tool_calls"][0]["function"]["name"], "lci_find_files")
        self.assertEqual(derived[3]["role"], "tool")
        self.assertEqual(derived[-1]["content"], '{"results":[]}')

    def test_derivation_fails_fast_without_an_issuing_assistant(self):
        base = json.loads((BASE / "fixture-deepseek-search.json").read_text())
        base["request"]["messages"].pop(2)
        case = {"tool": "find_files", "scenario": "negative", "arguments": {"pattern": "missing"},
                "output": '{"results":[]}'}
        with self.assertRaisesRegex(ValueError, "assistant"):
            module.derive_fixture(base, case, {"question": "q", "answer": ["a"]})

    def test_text_protocol_roundtrips_through_every_arm(self):
        source = "LCF/1.0 mode=overview\nfiles=447"
        for arm in module.replay.ARMS:
            rendered, recovered = module.replay.transform(source, arm, allow_text=True)
            self.assertEqual(recovered, source)
            self.assertIsInstance(rendered, str)


if __name__ == "__main__":
    unittest.main()
