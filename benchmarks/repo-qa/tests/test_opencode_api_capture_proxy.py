import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/opencode_api_capture_proxy.py"
SPEC = importlib.util.spec_from_file_location("capture_proxy", SCRIPT)
proxy = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(proxy)


class CaptureProxyTest(unittest.TestCase):
    def test_proxy_forwards_secret_but_capture_redacts_it(self):
        request_headers = {"Authorization":"Bearer secret-value","Content-Type":"application/json"}
        body = json.dumps({"model":"m","messages":[]}).encode()
        artifact = proxy.build_capture(1, "/chat/completions", request_headers, body, 200,
            {"Content-Type":"text/event-stream"}, b"data: [DONE]\n\n", None)
        self.assertEqual(proxy.forward_headers(request_headers)["Authorization"], "Bearer secret-value")
        self.assertEqual(artifact["request"]["headers"]["authorization"], "<redacted>")
        self.assertNotIn("secret-value", json.dumps(artifact))
        self.assertEqual(artifact["request"]["body"], {"model":"m","messages":[]})
        self.assertEqual(artifact["response"]["status"], 200)

    def test_secret_header_vocabulary_is_case_insensitive(self):
        values = proxy.sanitized_headers({"X-API-Key":"secret","Cookie":"session=x","Accept":"application/json"})
        self.assertEqual(values["x-api-key"], "<redacted>")
        self.assertEqual(values["cookie"], "<redacted>")
        self.assertEqual(values["accept"], "application/json")


if __name__ == "__main__": unittest.main()
