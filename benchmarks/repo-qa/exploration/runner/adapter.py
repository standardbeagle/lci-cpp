"""The single agent adapter interface and its fake + real implementations.

One interface, `AgentAdapter.run(AgentRequest) -> AgentResult`, backs BOTH arms
and BOTH the unit tests and the live smoke command. The request carries the
whole configuration; the arms differ only in `allowed_tools` and
`tool_instructions` (see `runner.toolsets`). Because the interface is injected,
unit tests substitute `FakeAgent` (a canned transcript) and never touch a
provider, while the guarded smoke path uses `ClaudeCliAdapter`.
"""

import json
import os
import shlex
import subprocess
from dataclasses import dataclass, field
from typing import Optional


@dataclass(frozen=True)
class ToolCall:
    """One tool invocation the agent emitted, in order."""

    name: str
    arguments: dict = field(default_factory=dict)


@dataclass(frozen=True)
class AgentRequest:
    """Everything an adapter needs. Shared fields are byte-identical across arms;
    only `allowed_tools` + `tool_instructions` vary."""

    model: str
    system_prompt: str
    timeout_seconds: int
    checkout_dir: str
    allowed_tools: tuple
    tool_instructions: str
    prompt: str


@dataclass(frozen=True)
class AgentResult:
    """The adapter's outcome. `status_hint` is the adapter's own read of what
    happened (`ok` / `timeout` / `provider_error`); the runner maps it, plus the
    tool-isolation gate, onto the final status taxonomy."""

    status_hint: str
    final_answer: Optional[str]
    tool_calls: tuple
    input_tokens: int
    output_tokens: int
    transcript: object


class FakeAgent:
    """Deterministic adapter for hermetic tests. Returns a canned result (or a
    per-request callable) and records every request it saw so parity/resume
    tests can assert the adapter was (not) invoked."""

    def __init__(self, result):
        self._result = result
        self.calls = []

    @property
    def result(self):
        return self._result

    def run(self, request):
        self.calls.append(request)
        if callable(self._result):
            return self._result(request)
        return self._result


class ClaudeCliAdapter:
    """Real adapter over the installed non-interactive Claude CLI.

    Invokes `claude -p --output-format json` with the arm's allowlist, the
    shared model / appended system prompt / timeout, and cwd set to the clean
    checkout so every file tool is rooted there. Never exercised by unit tests;
    only the guarded smoke command drives it.
    """

    def __init__(self, claude_bin="claude", mcp_config=None, extra_args=()):
        self.claude_bin = claude_bin
        self.mcp_config = mcp_config
        self.extra_args = tuple(extra_args)

    def _argv(self, request):
        argv = [
            self.claude_bin,
            "-p",
            request.prompt,
            "--model",
            request.model,
            "--output-format",
            "json",
            "--append-system-prompt",
            request.system_prompt + "\n\n" + request.tool_instructions,
            "--allowedTools",
            *request.allowed_tools,
            # Defence in depth: name-deny the escape hatches even if a future
            # default would expose them. The runner's gate re-checks regardless.
            "--disallowedTools",
            "Bash",
            "Edit",
            "Write",
            "--permission-mode",
            "default",
        ]
        if self.mcp_config:
            argv += ["--mcp-config", self.mcp_config]
        argv += list(self.extra_args)
        return argv

    def run(self, request):
        try:
            proc = subprocess.run(
                self._argv(request),
                cwd=request.checkout_dir,
                capture_output=True,
                text=True,
                timeout=request.timeout_seconds,
            )
        except subprocess.TimeoutExpired as exc:
            return AgentResult(
                "timeout", None, (), 0, 0,
                {"error": "timeout", "argv": shlex.join(self._argv(request)),
                 "stdout": exc.stdout or "", "stderr": exc.stderr or ""},
            )
        if proc.returncode != 0:
            return AgentResult(
                "provider_error", None, (), 0, 0,
                {"error": "cli_nonzero", "returncode": proc.returncode,
                 "stdout": proc.stdout, "stderr": proc.stderr},
            )
        return self._parse(proc.stdout, proc.stderr)

    @staticmethod
    def _parse(stdout, stderr):
        try:
            payload = json.loads(stdout)
        except json.JSONDecodeError:
            return AgentResult(
                "provider_error", None, (), 0, 0,
                {"error": "unparseable_cli_json", "stdout": stdout, "stderr": stderr},
            )
        usage = payload.get("usage", {}) or {}
        return AgentResult(
            status_hint="ok",
            final_answer=payload.get("result"),
            tool_calls=tuple(_extract_tool_calls(payload)),
            input_tokens=int(usage.get("input_tokens", 0) or 0),
            output_tokens=int(usage.get("output_tokens", 0) or 0),
            transcript=payload,
        )


def _extract_tool_calls(payload):
    """Ordered tool calls from a `claude -p --output-format json` transcript.

    The CLI's JSON carries the assistant turns under keys that have shifted
    across versions; read the messages defensively and preserve order.
    """
    calls = []
    messages = payload.get("messages") or payload.get("transcript") or []
    for message in messages:
        content = message.get("content") if isinstance(message, dict) else None
        if not isinstance(content, list):
            continue
        for block in content:
            if isinstance(block, dict) and block.get("type") == "tool_use":
                calls.append(ToolCall(block.get("name", ""), block.get("input", {}) or {}))
    return calls
