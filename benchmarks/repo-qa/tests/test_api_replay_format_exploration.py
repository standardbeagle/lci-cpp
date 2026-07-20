import copy
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/api_replay_format_exploration.py"
FIXTURE = ROOT / "benchmarks/repo-qa/api-replay/format-exploration/fixture-glm-search.json"
MANIFEST = ROOT / "benchmarks/repo-qa/api-replay/format-exploration/manifest.json"
SCHEDULE = ROOT / "benchmarks/repo-qa/api-replay/format-exploration/schedule.json"
spec = importlib.util.spec_from_file_location("api_replay_format_exploration", SCRIPT)
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)


class FakeProvider:
    def __init__(self): self.calls = []
    def complete(self, request, model, request_headers):
        self.calls.append((request, model, request_headers))
        return {"status": "answered", "final_answer": "examples/base/main.go:119"}


class FormatExplorationTest(unittest.TestCase):
    def setUp(self):
        self.fixture = json.loads(FIXTURE.read_text())
        self.fixture["request_headers"] = {
            "accept":"*/*", "content-type":"application/json", "user-agent":"captured-agent",
            "x-opencode-client":"cli", "x-opencode-project":"project",
            "x-opencode-request":"request", "x-opencode-session":"session",
        }
        self.manifest = json.loads(MANIFEST.read_text())
        self.schedule = json.loads(SCHEDULE.read_text())

    def test_all_arms_exactly_invert_and_only_tool_content_changes(self):
        report = replay.validate_fixture(self.fixture)
        self.assertEqual(set(report["arms"]), set(replay.ARMS))
        baseline = replay.build_request(self.fixture, replay.ARMS[0])
        index, source = replay.tool_content_pointer(baseline, self.fixture["tool_call_id"])
        for arm in replay.ARMS:
            request = replay.build_request(self.fixture, arm)
            _, recovered = replay.transform(source, arm)
            self.assertEqual(replay.canonical(recovered), replay.canonical(json.loads(source)))
            expected = [] if arm == replay.ARMS[0] else [f"/messages/{index}/content"]
            self.assertEqual(replay.diff_pointers(baseline, request), expected)

    def test_validator_detects_request_drift(self):
        request = replay.build_request(self.fixture, replay.ARMS[1])
        request["temperature"] = 0.75
        baseline = replay.build_request(self.fixture, replay.ARMS[0])
        self.assertEqual(replay.diff_pointers(baseline, request), ["/messages/3/content", "/temperature"])

    def test_frozen_schedule_is_opaque_deterministic_and_balanced(self):
        replay.validate_protocol(self.manifest, self.schedule)
        replay.validate_frozen_files(self.manifest, MANIFEST)
        self.assertTrue(all(arm.startswith("fmt_") for arm in replay.ARMS))
        rows = [self.schedule["arm_order_rows"][block["arm_order_row"] - 1] for block in self.schedule["blocks"]]
        for position in range(4):
            for arm in replay.ARMS: self.assertEqual(sum(row[position] == arm for row in rows), 2)

    def test_grid_requires_a_genuine_fixture_for_each_model(self):
        model = self.fixture["recorded_model"]
        with self.assertRaisesRegex(ValueError, "missing genuine captured fixture"):
            replay.planned_grid([self.fixture], self.manifest, self.schedule)
        with self.assertRaisesRegex(ValueError, "model does not match"):
            replay.cell_identity(self.fixture, "another/provider", replay.ARMS[0], 1, 1)

    def test_complete_schedule_is_64_cells_with_per_model_captures(self):
        second = copy.deepcopy(self.fixture)
        second["recorded_model"] = self.manifest["models"][0]["id"]
        second["request"]["model"] = second["recorded_model"].split("/", 1)[-1]
        jobs = replay.planned_grid([second, self.fixture], self.manifest, self.schedule)
        self.assertEqual(len(jobs), 64)
        expected = [(block["block"], model, arm, order)
                    for block in self.schedule["blocks"]
                    for model in block["model_order"]
                    for order, arm in enumerate(self.schedule["arm_order_rows"][block["arm_order_row"] - 1], 1)]
        self.assertEqual([(rep, model, arm, order) for _, model, arm, rep, order in jobs], expected)

    def test_provider_is_disabled_by_default_and_records_are_immutable(self):
        provider = FakeProvider(); model = self.fixture["recorded_model"]
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            with self.assertRaisesRegex(RuntimeError, "disabled by default"):
                replay.execute_cell(provider, self.fixture, model, replay.ARMS[0], 1, 1, out)
            self.assertEqual(provider.calls, [])
            record = replay.execute_cell(provider, self.fixture, model, replay.ARMS[0], 1, 1, out, allow_provider=True)
            self.assertEqual(record["status"], "answered")
            self.assertEqual(record["final_answer"], "examples/base/main.go:119")
            self.assertEqual(record["attempt"], 1)
            self.assertEqual(len(list((out / "attempts").glob("*.attempt-001.json"))), 1)
            with self.assertRaisesRegex(RuntimeError, "immutable"):
                replay.execute_cell(provider, self.fixture, model, replay.ARMS[0], 1, 1, out, allow_provider=True)
            self.assertEqual(len(provider.calls), 1)
            self.assertEqual(provider.calls[0][2], self.fixture["request_headers"])

    def test_fixture_headers_are_explicit_safe_and_arm_invariant(self):
        expected = {"accept", "content-type", "user-agent", "x-opencode-client",
                    "x-opencode-project", "x-opencode-request", "x-opencode-session"}
        self.assertEqual(set(replay.fixture_request_headers(self.fixture)), expected)
        for arm in replay.ARMS:
            self.assertEqual(replay.fixture_request_headers(self.fixture), self.fixture["request_headers"])
            replay.build_request(self.fixture, arm)
        for forbidden in ("authorization", "cookie", "proxy-authorization", "x-api-key"):
            broken = copy.deepcopy(self.fixture); broken["request_headers"][forbidden] = "secret"
            with self.assertRaisesRegex(ValueError, "non-allowlisted"):
                replay.validate_fixture(broken)

    def test_codec_failure_cannot_reach_provider(self):
        broken = copy.deepcopy(self.fixture)
        _, content = replay.tool_content_pointer(broken["request"], broken["tool_call_id"])
        broken["request"]["messages"][3]["content"] = content[:-1]
        provider = FakeProvider()
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(json.JSONDecodeError):
                replay.execute_cell(provider, broken, broken["recorded_model"], replay.ARMS[1], 1, 1, Path(directory), allow_provider=True)
        self.assertEqual(provider.calls, [])

    def test_retryable_attempts_are_preserved_and_first_answer_binds_cell(self):
        class SequenceProvider:
            def __init__(self): self.responses = iter((
                {"status": "provider_5xx", "failure": "HTTP 503", "raw_provider_stream": "one"},
                {"status": "answered", "final_answer": "ok", "raw_provider_stream": "two"},
            ))
            def complete(self, request, model, request_headers): return next(self.responses)
        provider = SequenceProvider(); model = self.fixture["recorded_model"]
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            first = replay.execute_cell(provider, self.fixture, model, replay.ARMS[0], 1, 1, out, allow_provider=True)
            second = replay.execute_cell(provider, self.fixture, model, replay.ARMS[0], 1, 1, out, allow_provider=True)
            self.assertEqual((first["attempt"], second["attempt"]), (1, 2))
            attempts = sorted((out / "attempts").glob("*.json")); self.assertEqual(len(attempts), 2)
            self.assertEqual(json.loads(attempts[0].read_text())["raw_provider_stream"], "one")
            final = next(path for path in out.glob("*.json"))
            self.assertEqual(json.loads(final.read_text())["bound_attempt"], 2)

    def test_provider_execution_requires_frozen_git_revision(self):
        for revision in (None, "UNFROZEN-provider-execution-prohibited", "sha256:" + "a" * 64, "a" * 39):
            with self.assertRaisesRegex(RuntimeError, "frozen git commit"):
                replay.require_frozen_analysis_revision({"analysis_revision": revision})
        replay.require_frozen_analysis_revision({"analysis_revision": "git:" + "a" * 40})

    def test_grid_resume_runs_missing_before_retries_and_skips_terminal_cells(self):
        model = self.fixture["recorded_model"]
        jobs = [(self.fixture, model, replay.ARMS[index], 1, index + 1) for index in range(4)]
        class Provider:
            def __init__(self): self.calls = []; self.counts = {}
            def complete(self, request, model, request_headers):
                _, content = replay.tool_content_pointer(request, self_fixture["tool_call_id"])
                arm = next(arm for arm in replay.ARMS if replay.build_request(self_fixture, arm)["messages"][3]["content"] == content)
                self.calls.append(arm); self.counts[arm] = self.counts.get(arm, 0) + 1
                if arm == replay.ARMS[0] and self.counts[arm] == 1: return {"status": "provider_5xx"}
                if arm == replay.ARMS[1]: return {"status": "malformed_provider_stream"}
                if arm == replay.ARMS[2]: return {"status": "provider_timeout"}
                return {"status": "answered", "final_answer": "ok"}
        self_fixture = self.fixture
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory); provider = Provider()
            records, counts = replay.run_provider_grid(provider, jobs, out)
            self.assertEqual(provider.calls[:4], list(replay.ARMS))
            self.assertEqual(provider.calls[4:], [replay.ARMS[0], replay.ARMS[2], replay.ARMS[2]])
            self.assertEqual(counts, {"executed": 7, "retried": 3, "skipped": 0})
            calls = len(provider.calls)
            _, resumed = replay.run_provider_grid(provider, jobs, out)
            self.assertEqual(len(provider.calls), calls)
            self.assertEqual(resumed, {"executed": 0, "retried": 0, "skipped": 4})
            self.assertEqual(len(records), 7)


if __name__ == "__main__": unittest.main()
