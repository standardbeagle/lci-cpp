import importlib.util
import base64
import json
import tempfile
import unittest
import urllib.error
from email.message import Message
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "benchmarks/repo-qa/scripts/opencode_zen_provider.py"
spec = importlib.util.spec_from_file_location("opencode_zen_provider", SCRIPT)
zen = importlib.util.module_from_spec(spec); spec.loader.exec_module(zen)


class Response:
    def __init__(self, body, status=200, headers=None):
        self.body = body; self.status = status; self.headers = headers or {}
    def __enter__(self): return self
    def __exit__(self, *_): pass
    def getcode(self): return self.status
    def read(self): return self.body


class ZenProviderTest(unittest.TestCase):
    def test_endpoint_allowlist(self):
        self.assertEqual(zen.endpoint_for_model("opencode/x"), "https://opencode.ai/zen/v1/chat/completions")
        self.assertEqual(zen.endpoint_for_model("opencode-go/x"), "https://opencode.ai/zen/go/v1/chat/completions")
        with self.assertRaisesRegex(ValueError, "not allowlisted"): zen.endpoint_for_model("evil/x")

    def test_auth_shapes_are_read_without_returning_document(self):
        for value in ({"key": "secret"}, {"type": "api", "apiKey": "secret"}, {"oauth": {"access": "ignored", "access_token": "secret"}}):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "auth.json"; path.write_text(json.dumps({"opencode": value}))
                self.assertEqual(zen.load_auth("opencode/x", path), "secret")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "auth.json"; path.write_text('{}')
            with self.assertRaisesRegex(ValueError, "no credential"): zen.load_auth("opencode/x", path)
            with self.assertRaisesRegex(ValueError, "no credential"): zen.load_auth("opencode-go/x", path)

    def test_streaming_sse_handles_comments_crlf_multiline_usage_and_done(self):
        body = (b': ping\r\ndata: {"choices":[{"delta":{"content":"hello "}}]}\r\n\r\n'
                b'data: {"choices":[{"delta":{"content":[{"type":"text","text":"world"}]}}],\r\n'
                b'data: "usage":{"prompt_tokens":3,"completion_tokens":2}}\r\n\r\ndata: [DONE]\r\n\r\n')
        result = zen.parse_response(body, "text/event-stream; charset=utf-8")
        self.assertEqual(result["status"], "answered"); self.assertEqual(result["final_answer"], "hello world")
        self.assertEqual(result["usage"]["prompt_tokens"], 3)

    def test_nonstream_and_failure_parsing(self):
        parsed = zen.parse_response(b'{"choices":[{"message":{"content":"answer"}}],"usage":{"total_tokens":4}}', "application/json")
        self.assertEqual((parsed["status"], parsed["final_answer"]), ("answered", "answer"))
        self.assertEqual(zen.parse_response(b'data: nope\n\n', "text/event-stream")["status"], "malformed_provider_stream")
        self.assertEqual(zen.parse_response(b'data: [DONE]\n\n', "text/event-stream")["status"], "empty_answer")
        self.assertEqual(zen.parse_response(b'\xff', "application/json")["status"], "malformed_provider_stream")

    def test_http_capture_redacts_request_headers_and_keeps_raw_stream(self):
        seen = {}
        def opener(request, timeout):
            seen["request"] = request; seen["timeout"] = timeout
            return Response(b'data: {"choices":[{"delta":{"content":"ok"}}],"usage":{"total_tokens":1}}\n\ndata: [DONE]\n\n',
                            headers={"Content-Type": "text/event-stream", "X-Request-Id": "req", "Set-Cookie": "secret"})
        with tempfile.TemporaryDirectory() as directory:
            auth = Path(directory) / "auth.json"; auth.write_text('{"opencode":{"key":"secret"}}')
            ticks = iter((10.0, 10.25))
            result = zen.OpenCodeZenProvider(auth, 9, opener=opener, clock=lambda: next(ticks)).complete({"model":"x"}, "opencode/x")
        self.assertEqual(result["status"], "answered"); self.assertEqual(result["latency_seconds"], .25)
        self.assertIn('data: {"choices"', result["raw_provider_stream"])
        self.assertEqual(base64.b64decode(result["raw_provider_stream_base64"]),
                         result["raw_provider_stream"].encode())
        self.assertTrue(result["raw_provider_stream_digest"].startswith("sha256:"))
        self.assertEqual(result["response_headers"], {"content-type": "text/event-stream", "x-request-id": "req"})
        serialized = json.dumps(result); self.assertNotIn("secret", serialized); self.assertNotIn("authorization", serialized.lower())
        self.assertEqual(seen["request"].get_header("Authorization"), "Bearer secret")

    def test_timeout_transport_quota_5xx_and_other_http(self):
        with tempfile.TemporaryDirectory() as directory:
            auth = Path(directory) / "auth.json"; auth.write_text('{"opencode":{"key":"s"}}')
            def run(error):
                ticks = iter((1.0, 2.0))
                def opener(*_args, **_kwargs): raise error
                return zen.OpenCodeZenProvider(auth, 1, opener=opener, clock=lambda: next(ticks)).complete({}, "opencode/x")
            self.assertEqual(run(TimeoutError())["status"], "provider_timeout")
            self.assertEqual(run(urllib.error.URLError("down"))["status"], "transport_error")
            for code, expected in ((429, "provider_quota"), (503, "provider_5xx"), (401, "provider_error")):
                headers = Message(); headers["Set-Cookie"] = "secret"; headers["Retry-After"] = "2"
                error = urllib.error.HTTPError("https://fixed", code, "failure", headers, None)
                error.read = lambda: b'{"error":{"message":"detail"}}'
                result = run(error); self.assertEqual(result["status"], expected)
                self.assertEqual(result["response_headers"], {"retry-after": "2"})


if __name__ == "__main__": unittest.main()
