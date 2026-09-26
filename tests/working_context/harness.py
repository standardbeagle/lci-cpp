"""Real-interpreter harness for the lci working-context Slop custom tools.

The tests never re-implement SLOP.  They launch the real ``slop-mcp`` server in
an isolated temporary project, register a controlled fixture MCP as ``lci``
(``fixture_mcp.py``), install the shipped pack through the real
``customize_tools`` meta-tool, and invoke the custom tools through the real
``execute_tool`` meta-tool.  Anything the custom tools send to the ``lci``
boundary is recorded by the fixture, and every failure mode (parse error,
MCP error envelope, schema drift) is surfaced as a test failure, never skipped.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
PACK_PATH = os.path.join(
    REPO_ROOT, ".slop-mcp-packs", "lci-working-context.json"
)
FIXTURE_PATH = os.path.join(HERE, "fixture_mcp.py")

CUSTOM_TOOL_NAMES = (
    "save_working_context",
    "open_files_context",
    "resume_working_context",
    "refresh_working_context",
)


def find_slop_mcp():
    """Return the real interpreter path or raise (tests must fail, not skip)."""
    override = os.environ.get("SLOP_MCP_BIN")
    if override:
        if not os.path.isfile(override):
            raise RuntimeError("SLOP_MCP_BIN does not exist: %s" % override)
        return override
    found = shutil.which("slop-mcp")
    if not found:
        raise RuntimeError(
            "the real slop-mcp interpreter is required for these tests; "
            "install it or set SLOP_MCP_BIN"
        )
    return found


def load_pack():
    with open(PACK_PATH, "r", encoding="utf-8") as handle:
        return json.load(handle)


class SlopSession:
    """An isolated slop-mcp server + controlled lci fixture."""

    def __init__(self, timeout=30.0):
        self.timeout = timeout
        self.binary = find_slop_mcp()
        self.root = tempfile.mkdtemp(prefix="lci-working-context-")
        self.home = os.path.join(self.root, "home")
        self.xdg = os.path.join(self.root, "xdg")
        self.project = os.path.join(self.root, "project")
        self.control_path = os.path.join(self.root, "control.json")
        self.record_path = os.path.join(self.root, "calls.jsonl")
        self.stderr_path = os.path.join(self.root, "serve.stderr")

        os.makedirs(os.path.join(self.xdg, "slop-mcp"), exist_ok=True)
        os.makedirs(self.project, exist_ok=True)
        self._write_control({})
        open(self.record_path, "w", encoding="utf-8").close()

        subprocess.run(
            ["git", "init", "-q"], cwd=self.project, check=True
        )
        with open(
            os.path.join(self.project, ".slop-mcp.kdl"), "w", encoding="utf-8"
        ) as handle:
            handle.write(
                'mcp "lci" {\n'
                '  type "stdio"\n'
                '  command "%s"\n'
                '  args "%s"\n'
                "  env {\n"
                '    "FIXTURE_CONTROL" "%s"\n'
                '    "FIXTURE_RECORD" "%s"\n'
                "  }\n"
                "}\n"
                % (sys.executable, FIXTURE_PATH, self.control_path, self.record_path)
            )

        env = dict(os.environ)
        env["HOME"] = self.home
        env["XDG_CONFIG_HOME"] = self.xdg
        self._stderr = open(self.stderr_path, "w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [self.binary, "serve"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self._stderr,
            cwd=self.project,
            env=env,
            text=True,
            bufsize=1,
        )
        self._lines = []
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self._next_id = 1
        self.initialize()

    # -- transport -----------------------------------------------------------

    def _read_loop(self):
        for line in self.proc.stdout:
            with self._lock:
                self._lines.append(line.rstrip("\n"))

    def _rpc(self, method, params):
        message_id = self._next_id
        self._next_id += 1
        payload = {
            "jsonrpc": "2.0",
            "id": message_id,
            "method": method,
            "params": params,
        }
        self.proc.stdin.write(json.dumps(payload) + "\n")
        self.proc.stdin.flush()
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            with self._lock:
                pending = list(self._lines)
            for line in pending:
                if not line.strip():
                    with self._lock:
                        self._lines.remove(line)
                    continue
                try:
                    message = json.loads(line)
                except ValueError:
                    continue
                if message.get("id") == message_id:
                    with self._lock:
                        self._lines.remove(line)
                    return message
            if self.proc.poll() is not None:
                raise RuntimeError(
                    "slop-mcp serve exited (code %s): %s"
                    % (self.proc.returncode, self._read_stderr())
                )
            time.sleep(0.02)
        raise RuntimeError(
            "timed out waiting for slop-mcp response to %s: %s"
            % (method, self._read_stderr())
        )

    def _read_stderr(self):
        try:
            with open(self.stderr_path, "r", encoding="utf-8") as handle:
                return handle.read()[-2000:]
        except OSError:
            return ""

    def initialize(self):
        self._rpc(
            "initialize",
            {
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": "working-context-tests", "version": "1"},
            },
        )
        self.proc.stdin.write(
            json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"})
            + "\n"
        )
        self.proc.stdin.flush()

    # -- meta tools ----------------------------------------------------------

    def call_tool(self, name, arguments):
        message = self._rpc(
            "tools/call", {"name": name, "arguments": arguments}
        )
        if "error" in message:
            raise AssertionError("MCP protocol error: %s" % message["error"])
        result = message.get("result", {})
        text = ""
        for block in result.get("content", []):
            if block.get("type") == "text":
                text += block.get("text", "")
        parsed = None
        if text:
            try:
                parsed = json.loads(text)
            except ValueError:
                parsed = None
        return {
            "text": text,
            "parsed": parsed,
            "is_error": bool(result.get("isError")),
        }

    def import_pack(self, scope="project", overwrite=False):
        with open(PACK_PATH, "r", encoding="utf-8") as handle:
            data = handle.read()
        return self.call_tool(
            "customize_tools",
            {
                "action": "import",
                "data": data,
                "scope": scope,
                "overwrite": overwrite,
            },
        )

    def list_custom(self, scope="project"):
        return self.call_tool(
            "customize_tools", {"action": "list_custom", "scope": scope}
        )

    def export_pack(self, scope="project"):
        return self.call_tool(
            "customize_tools", {"action": "export", "scope": scope}
        )

    def execute_custom(self, tool_name, parameters):
        return self.call_tool(
            "execute_tool",
            {
                "mcp_name": "_custom",
                "tool_name": tool_name,
                "parameters": parameters,
            },
        )

    # -- fixture control -----------------------------------------------------

    def _write_control(self, control):
        with open(self.control_path, "w", encoding="utf-8") as handle:
            json.dump(control, handle)

    def set_control(self, control):
        self._write_control(control)

    def clear_calls(self):
        open(self.record_path, "w", encoding="utf-8").close()

    def calls(self):
        out = []
        try:
            with open(self.record_path, "r", encoding="utf-8") as handle:
                for line in handle:
                    line = line.strip()
                    if line:
                        out.append(json.loads(line))
        except OSError:
            pass
        return out

    def context_calls(self):
        return [c for c in self.calls() if c.get("tool") == "context"]

    # -- lifecycle -----------------------------------------------------------

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass
        try:
            self._stderr.close()
        except Exception:
            pass
        shutil.rmtree(self.root, ignore_errors=True)


def assert_no_parse_error(testcase, outcome, label):
    """Fail when a custom tool body could not even parse."""
    if outcome["is_error"] and "parse error" in outcome["text"].lower():
        testcase.fail("%s: script parse error leaked: %s" % (label, outcome["text"]))
