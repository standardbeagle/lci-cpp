import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/api_replay_auth_probe.py"
SPEC = importlib.util.spec_from_file_location("api_replay_auth_probe", SCRIPT)
probe = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(probe)


class FakeProvider:
    def __init__(self, status="answered", http_status=200):
        self.status = status
        self.http_status = http_status
        self.calls = []

    def complete(self, request, model, headers):
        self.calls.append((request, model, headers))
        return {"status": self.status, "http_status": self.http_status, "raw_provider_stream": "raw",
                "raw_provider_stream_base64": "cmF3", "raw_provider_stream_digest": "sha256:x",
                "failure": None, "response_headers": {}, "usage": {}, "latency_seconds": 0.1}


class AuthProbeTest(unittest.TestCase):
    def fixture(self, model):
        return {"recorded_model": model, "task_id": "t", "tool_call_id": "c",
                "request_headers": {"accept": "*/*"},
                "request": {"model": model.split("/", 1)[1], "messages": [
                    {"role": "tool", "tool_call_id": "c", "content": "{\"x\":1}"}]}}

    def test_success_is_explicitly_excluded(self):
        provider = FakeProvider()
        result = probe.run_probes(provider, [self.fixture("opencode-go/a"), self.fixture("opencode-go/b")])
        self.assertTrue(result["all_succeeded"])
        self.assertTrue(all(item["excluded_from_outcomes"] for item in result["probes"]))
        self.assertEqual(len(provider.calls), 2)

    def test_non_200_fails_gate(self):
        result = probe.run_probes(FakeProvider("provider_error", 403), [self.fixture("opencode-go/a")])
        self.assertFalse(result["all_succeeded"])


if __name__ == "__main__":
    unittest.main()
