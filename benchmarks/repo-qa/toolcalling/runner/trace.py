"""Turn an opencode `run --format json` event stream into an ordered call trace.

Two event shapes exist in the wild and both are accepted: the top-level
`{"type": "tool_use", "part": {...}}` shape scripts/bench.py parses, and the
`{"part": {"type": "tool", ...}}` shape newer streams emit. Requiring a `tool`
key on the part keeps the union safe.

MCP tools arrive namespaced by the server key (`lci_callers`); natives do not.
The distinction is kept rather than collapsed, because a native call made
BEFORE any MCP call is itself a selection outcome.
"""

from __future__ import annotations

import json

MCP_PREFIX = "lci_"
_TOOL_KINDS = {"tool_use", "tool"}


def _kinds(event: dict, part: dict) -> set[str]:
    out = set()
    for value in (event.get("type"), part.get("type")):
        if isinstance(value, str):
            out.add(value.replace("-", "_"))
    return out


def parse_trace(lines) -> list[dict]:
    """Ordered tool calls, position 1-based over the whole trace."""
    calls: list[dict] = []
    for line in lines:
        line = line.strip() if isinstance(line, str) else ""
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(event, dict):
            continue
        part = event.get("part")
        if not isinstance(part, dict):
            continue
        name = part.get("tool")
        if not isinstance(name, str) or not name:
            continue
        if not _kinds(event, part) & _TOOL_KINDS:
            continue
        state = part.get("state") if isinstance(part.get("state"), dict) else {}
        args = state.get("input")
        native = not name.startswith(MCP_PREFIX)
        calls.append({
            "tool": name if native else name[len(MCP_PREFIX):],
            "raw_tool": name,
            "native": native,
            "args": args if isinstance(args, dict) else {},
            "status": state.get("status", "?"),
            "position": len(calls) + 1,
        })
    return calls
