"""The single agent adapter interface and its fake + real implementations.

One interface, `AgentAdapter.run(AgentRequest) -> AgentResult`, backs BOTH arms
and BOTH the unit tests and the live smoke command. The request carries the
whole configuration. Claim-validation arms differ only in `allowed_tools`;
legacy exploration also varies `tool_instructions` (see `runner.toolsets`).
Because the interface is injected,
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
    """Everything an adapter needs for one arm of a paired run."""

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

    Invokes `claude -p --output-format stream-json --verbose` with the arm's
    allowlist, shared model / appended system prompt / timeout, and cwd set to
    the clean checkout so every file tool is rooted there.
    """

    def __init__(self, claude_bin="claude", mcp_config=None, extra_args=(),
                 disallowed_tools=("Bash", "Edit", "Write")):
        self.claude_bin = claude_bin
        self.mcp_config = mcp_config
        self.extra_args = tuple(extra_args)
        # Exploration keeps the default (read-only: no shell, no mutation). The
        # edit-mode caller narrows this to just Bash so the agent MAY edit.
        self.disallowed_tools = tuple(disallowed_tools)

    def _argv(self, request):
        argv = [
            self.claude_bin,
            "-p",
            request.prompt,
            "--model",
            request.model,
            "--output-format",
            "stream-json",
            "--verbose",
            "--append-system-prompt",
            request.system_prompt + "\n\n" + request.tool_instructions,
            "--allowedTools",
            *request.allowed_tools,
            # Defence in depth: name-deny the escape hatches even if a future
            # default would expose them. The runner's gate re-checks regardless.
            "--disallowedTools",
            *self.disallowed_tools,
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
        events = []
        try:
            for line in stdout.splitlines():
                if line.strip():
                    events.append(json.loads(line))
        except (json.JSONDecodeError, TypeError):
            return AgentResult(
                "provider_error", None, (), 0, 0,
                {"error": "unparseable_cli_json", "stdout": stdout, "stderr": stderr},
            )
        assistant_events = [
            event for event in events
            if isinstance(event, dict) and event.get("type") == "assistant"
        ]
        results = [
            event for event in events
            if isinstance(event, dict) and event.get("type") == "result"
        ]
        if not assistant_events:
            return AgentResult(
                "provider_error", None, (), 0, 0,
                {"error": "missing_tool_history", "transcript": events,
                 "stderr": stderr},
            )
        if not results:
            return AgentResult(
                "provider_error", None, (), 0, 0,
                {"error": "missing_cli_result", "transcript": events,
                 "stderr": stderr},
            )
        try:
            tool_calls = tuple(_extract_tool_calls(events))
        except ValueError as exc:
            return AgentResult(
                "provider_error", None, (), 0, 0,
                {"error": "malformed_tool_history", "detail": str(exc),
                 "transcript": events, "stderr": stderr},
            )
        for payload in results:
            error = _result_validation_error(payload)
            if error:
                return AgentResult(
                    "provider_error", None, (), 0, 0,
                    {"error": "invalid_cli_result", "detail": error,
                     "transcript": events, "stderr": stderr},
                )
        payload = results[-1]
        usage = payload["usage"]
        return AgentResult(
            status_hint="ok",
            final_answer=payload.get("result"),
            tool_calls=tool_calls,
            input_tokens=int(usage.get("input_tokens", 0) or 0),
            output_tokens=int(usage.get("output_tokens", 0) or 0),
            transcript=events,
        )


def _result_validation_error(payload):
    """Return a reason when a stream-json result is not a valid success."""
    if payload.get("subtype") != "success":
        return "result subtype must be success"
    result = payload.get("result")
    if not isinstance(result, str) or not result:
        return "result must be a nonempty string"
    usage = payload.get("usage")
    if not isinstance(usage, dict):
        return "usage must be an object"
    for key, value in usage.items():
        if not key.endswith("_tokens"):
            continue
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            return f"usage.{key} must be a nonnegative integer"
    return None


def _extract_tool_calls(events):
    """Extract ordered tool calls from stream-json assistant events."""
    calls = []
    for event in events:
        if not isinstance(event, dict) or event.get("type") != "assistant":
            continue
        message = event.get("message")
        if not isinstance(message, dict):
            raise ValueError("assistant event message must be an object")
        content = message.get("content")
        if not isinstance(content, list):
            raise ValueError("assistant message content must be an array")
        for block in content:
            if not isinstance(block, dict) or not isinstance(block.get("type"), str):
                raise ValueError("assistant content blocks must be typed objects")
            if block["type"] != "tool_use":
                continue
            name = block.get("name")
            arguments = block.get("input")
            if not isinstance(name, str) or not name:
                raise ValueError("tool_use name must be a nonempty string")
            if not isinstance(arguments, dict):
                raise ValueError("tool_use input must be an object")
            calls.append(ToolCall(name, arguments))
    return calls


# --- opencode -------------------------------------------------------------
#
# The opencode surface names its tools and arguments differently from the
# Claude CLI, and the arms / gate are both written in the Claude vocabulary.
# Normalising at the adapter boundary keeps ONE allowlist and ONE gate for
# both providers: `runner.toolsets` stays the single source of arm truth and
# `gate.enforce` keeps re-checking what the agent actually emitted.
#
# Both tables below are derived from REAL recorded opencode streams (1,262
# tool calls across the 160 executed discovery cells under
# results/discovery/, opencode 1.18.x), never hand-imagined -- the `filePath`
# and `include` spellings are exactly the kind of drift a hand-written fixture
# reproduces wrongly (bench-harness-oracle-independence rule 9a).
#
# An UNMAPPED name or argument passes through verbatim so the gate rejects it.
# Never map an unknown tool onto an allowed one: that would convert a
# disjointness violation into a clean run.
_OPENCODE_NATIVE_TOOLS = {"read": "Read", "grep": "Grep", "glob": "Glob"}
_OPENCODE_MCP_PREFIX = "lci_"

# Observed counts in the recorded corpus, for the reader who wonders why only
# these two: read.filePath 340/340, grep.include 144/262 (`glob` in the Claude
# vocabulary). grep.flags (1/262) is deliberately NOT mapped -- it has no
# canonical equivalent, so it stays unknown and the gate flags it.
_OPENCODE_ARGUMENTS = {
    "Read": {"filePath": "file_path"},
    "Grep": {"include": "glob"},
}


def canonical_tool_name(name):
    """opencode tool id -> the allowlist/gate vocabulary (or verbatim)."""
    if name in _OPENCODE_NATIVE_TOOLS:
        return _OPENCODE_NATIVE_TOOLS[name]
    if name.startswith(_OPENCODE_MCP_PREFIX):
        return "mcp__lci__" + name[len(_OPENCODE_MCP_PREFIX):]
    return name


def attempted_tool_call(name, arguments):
    """opencode records a refused tool call as a pseudo-tool named `invalid`
    whose arguments carry the tool the model actually tried. Report the call
    under that tool so the gate names the real attempt (e.g. `bash`). The
    arguments are kept unchanged as evidence, so the gate verdict does not
    change: an attempt on a non-allowlisted tool is still `tool_not_allowed`.
    """
    if name == "invalid" and isinstance(arguments, dict) and isinstance(
            arguments.get("tool"), str) and arguments["tool"]:
        return arguments["tool"], arguments
    return name, arguments


def canonical_arguments(tool, arguments):
    """Rename an opencode call's arguments into the gate's schema vocabulary."""
    if not isinstance(arguments, dict):
        return arguments
    mapping = _OPENCODE_ARGUMENTS.get(tool, {})
    return {mapping.get(key, key): value for key, value in arguments.items()}


# Native opencode tool ids, verified against the installed opencode (1.18.x)
# and shared verbatim with scripts/bench.py NATIVE_TOOL_IDS. Anything not
# granted by the arm is denied explicitly, so neither arm can reach the
# other's mechanism (bench-harness-oracle-independence rule 12).
OPENCODE_NATIVE_TOOL_IDS = (
    "apply_patch", "bash", "edit", "glob", "grep", "lsp", "question", "read",
    "skill", "task", "todowrite", "webfetch", "websearch", "write",
)

_CANONICAL_TO_OPENCODE = {v: k for k, v in _OPENCODE_NATIVE_TOOLS.items()}


def opencode_tools_map(allowed_tools):
    """The opencode `tools` map for an arm's allowlist: deny-by-default.

    Every native tool the arm does not grant is explicitly disabled. MCP tools
    (`mcp__lci__*`) are not native and are governed by whether the server is
    registered at all, so they do not appear here.
    """
    granted = set()
    for tool in allowed_tools:
        if tool.startswith("mcp__"):
            continue
        native = _CANONICAL_TO_OPENCODE.get(tool)
        if native is None:
            raise ValueError(
                f"allowlist names native tool {tool!r} with no opencode id; refusing "
                f"to launch an arm whose surface cannot be enforced")
        granted.add(native)
    return {name: False for name in OPENCODE_NATIVE_TOOL_IDS if name not in granted}


def lci_mcp_tool_ids(lci_bin, timeout=60):
    """Every tool the LCI MCP server actually serves, asked of the binary.

    Registering the server exposes its WHOLE surface, which is wider than the
    arm's allowlist, so the denial list has to be built from what the server
    really serves. A committed snapshot would go stale silently the next time
    a tool is added (bench-harness-oracle-independence rule 8a), and a stale
    snapshot here would silently widen the treatment arm. There is no
    fallback: failing to enumerate raises.
    """
    request = "\n".join([
        json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                               "clientInfo": {"name": "exploration-runner", "version": "1"}}}),
        json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
        json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}),
    ]) + "\n"
    proc = subprocess.run([lci_bin, "mcp"], input=request, capture_output=True,
                          text=True, timeout=timeout)
    for line in proc.stdout.splitlines():
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        if message.get("id") == 2 and "result" in message:
            return sorted(t["name"] for t in message["result"]["tools"])
    raise ValueError(
        f"{lci_bin} mcp did not answer tools/list; cannot determine which LCI tools "
        f"to deny, and registering the server unchecked would widen the arm")


# opencode's headerTimeout and chunkTimeout both default to 300000 ms, the
# same length as the cell deadline. A provider request that stops sending
# bytes was therefore never detected by opencode: the cell deadline killed the
# whole cell first. Both providers used by the pilot (cline-pass, opencode-go)
# produced such hung requests mid-session. At 60 s a hung request fails while
# the cell still has time left.
PROVIDER_HEADER_TIMEOUT_MS = 60_000
PROVIDER_CHUNK_TIMEOUT_MS = 60_000
# opencode's default MCP request timeout is 5000 ms. The first LCI call on a
# fresh checkout starts and indexes the per-root server, which can take longer.
LCI_MCP_TIMEOUT_MS = 60_000


def opencode_workspace_config(allowed_tools, lci_bin=None, model=None):
    """Full opencode config for one arm. The LCI MCP server is registered only
    when the arm's allowlist actually names LCI tools. When `model` is given,
    request timeouts are set for its provider (the id before the first `/`)."""
    wants_lci = any(t.startswith("mcp__lci__") for t in allowed_tools)
    if wants_lci and not lci_bin:
        raise ValueError("treatment arm requires lci_bin to register the MCP server")
    config = {
        "$schema": "https://opencode.ai/config.json",
        # A host-registered aggregator would re-expose denied tools.
        "mcp": {"slop-mcp": {"enabled": False}},
        # external_directory confines every file tool to opencode's project
        # root. That is only the checkout once the checkout is its own git
        # project (see own_git_project); otherwise opencode resolves the
        # enclosing repository and the whole of it counts as "inside".
        "permission": {"edit": "deny", "webfetch": "deny",
                       "external_directory": "deny"},
        "tools": opencode_tools_map(allowed_tools),
    }
    if model:
        provider = model.split("/", 1)[0]
        config["provider"] = {provider: {"options": {
            "headerTimeout": PROVIDER_HEADER_TIMEOUT_MS,
            "chunkTimeout": PROVIDER_CHUNK_TIMEOUT_MS,
        }}}
    if wants_lci:
        config["mcp"]["lci"] = {
            "type": "local", "command": [lci_bin, "mcp"], "enabled": True,
            "timeout": LCI_MCP_TIMEOUT_MS,
        }
        # The server serves its whole surface; the arm is narrower. Deny every
        # served tool the allowlist does not name, or the treatment arm gets
        # tools the registered arm never granted -- which the isolation gate
        # then rejects mid-run as a violation, losing the cell.
        granted = {t[len("mcp__lci__"):] for t in allowed_tools
                   if t.startswith("mcp__lci__")}
        config["tools"].update({
            f"lci_{name}": False
            for name in lci_mcp_tool_ids(lci_bin) if name not in granted
        })
    return config


def own_git_project(checkout_dir):
    """Make the checkout its own git project for opencode.

    opencode identifies its project by walking up to the nearest git
    repository. The checkout has no .git, so opencode resolved the enclosing
    lci-cpp repository instead: its project root became the repo root, the
    answer keys under benchmarks/repo-qa became readable "inside" files, and
    every cell refreshed all of lci-cpp's sibling worktrees before its first
    turn. An empty root commit is enough for opencode's project identity and
    costs the same on every corpus size. The checkout is recopied fresh by
    corpus.prepare_checkout for every arm, so nothing carries between arms.
    """
    if os.path.isdir(os.path.join(checkout_dir, ".git")):
        return
    git_env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    subprocess.run(["git", "init", "-q"], cwd=checkout_dir, env=git_env, check=True)
    subprocess.run(
        ["git", "-c", "user.name=exploration-harness",
         "-c", "user.email=exploration-harness@invalid",
         "commit", "-q", "--allow-empty", "--no-verify", "-m", "exploration checkout"],
        cwd=checkout_dir, env=git_env, check=True,
    )


class OpencodeAdapter:
    """Real adapter over the installed `opencode` CLI.

    Mirrors ClaudeCliAdapter's contract so the runner, the gate and the record
    schema are provider-agnostic: same AgentRequest in, same AgentResult out.
    Three things differ from the Claude path and each is deliberate:

    * The arm's surface is enforced by an opencode `tools` map written per
      cell (deny-by-default), not by a CLI allowlist flag.
    * Tool names and arguments are normalised back into the Claude vocabulary
      so `gate.enforce` keeps working unchanged on what was actually emitted.
    * All opencode state (XDG config/state/data/cache) is redirected OUTSIDE
      the measured checkout, while PWD and cwd stay inside it. The config file
      itself also lives outside, so the corpus the arms are scored on is never
      mutated by the harness.
    """

    def __init__(self, opencode_bin="opencode", lci_bin=None, state_root=None):
        self.opencode_bin = opencode_bin
        self.lci_bin = lci_bin
        self.state_root = state_root

    def _state_dir(self, request):
        import tempfile
        parent = self.state_root or tempfile.gettempdir()
        os.makedirs(parent, exist_ok=True)
        return tempfile.mkdtemp(prefix="opencode-cell-", dir=parent)

    def run(self, request):
        import shutil
        import tempfile
        from runner import opencode_stream

        state_dir = self._state_dir(request)
        own_git_project(request.checkout_dir)
        outcome = None
        try:
            config_path = os.path.join(state_dir, "opencode.json")
            with open(config_path, "w") as handle:
                json.dump(
                    opencode_workspace_config(request.allowed_tools, self.lci_bin,
                                              request.model),
                    handle, indent=2, sort_keys=True,
                )
            environment = opencode_stream.isolated_environment(
                state_dir, config_path=config_path, pwd=request.checkout_dir,
            )
            prompt = (
                request.system_prompt + "\n\n" + request.tool_instructions
                + "\n\n" + request.prompt
            )
            outcome = opencode_stream.run_cell(
                self.opencode_bin, request.checkout_dir, environment,
                request.model, prompt, request.timeout_seconds,
            )
        finally:
            # A cell that did not answer keeps its state dir: opencode writes
            # its log there, and deleting it destroyed the only evidence of
            # why a cell stalled.
            if outcome is not None and outcome["status_hint"] == "ok":
                shutil.rmtree(state_dir, ignore_errors=True)
        if outcome["status_hint"] != "ok":
            outcome["transcript"]["state_dir"] = state_dir
            outcome["transcript"]["opencode_log_dir"] = os.path.join(
                state_dir, ".xdg-data-home", "opencode", "log")

        calls = tuple(
            ToolCall(canonical_tool_name(name),
                     canonical_arguments(canonical_tool_name(name), arguments))
            for name, arguments in (
                attempted_tool_call(name, arguments)
                for name, arguments in outcome["tool_calls"]
            )
        )
        return AgentResult(
            outcome["status_hint"], outcome["answer"] or None, calls,
            outcome["tokens"]["input"], outcome["tokens"]["output"],
            outcome["transcript"],
        )
