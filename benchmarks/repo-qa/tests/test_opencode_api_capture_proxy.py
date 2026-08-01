import http.server
import importlib.util
import json
import socket
import tempfile
import threading
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


class TruncatingUpstream(http.server.BaseHTTPRequestHandler):
    """Promise more body than it delivers, then hang up: a mid-stream failure."""
    protocol_version = "HTTP/1.1"
    def log_message(self, *_): return
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                         b"Transfer-Encoding: chunked\r\n\r\n"
                         b"f\r\ndata: partial\n\n\r\n")
        self.wfile.flush()
        self.close_connection = True


class MidStreamFailureTest(unittest.TestCase):
    def test_no_second_response_head_after_headers_are_sent(self):
        upstream = http.server.ThreadingHTTPServer(("127.0.0.1", 0), TruncatingUpstream)
        threading.Thread(target=upstream.serve_forever, daemon=True).start()
        with tempfile.TemporaryDirectory() as directory:
            server = proxy.CaptureServer(("127.0.0.1", 0), f"http://127.0.0.1:{upstream.server_port}",
                                         Path(directory), max_requests=1)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            body = b'{"model":"m"}'
            with socket.create_connection(("127.0.0.1", server.server_port), timeout=30) as client:
                client.sendall(b"POST /chat/completions HTTP/1.1\r\nHost: x\r\n"
                               b"Content-Type: application/json\r\n"
                               + f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
                client.settimeout(30)
                received = bytearray()
                while True:
                    chunk = client.recv(65536)
                    if not chunk: break
                    received.extend(chunk)
            server.server_close()
            captures = list(Path(directory).glob("capture-*.json"))
            upstream.shutdown(); upstream.server_close()
            self.assertEqual(bytes(received).count(b"HTTP/1."), 1, bytes(received))
            self.assertEqual(len(captures), 1)
            self.assertIsNotNone(json.loads(captures[0].read_text())["failure"])


if __name__ == "__main__": unittest.main()
