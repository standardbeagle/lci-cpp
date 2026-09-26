#!/usr/bin/env python3
"""Controlled lci MCP fixture for the working-context tests.

This is the external boundary stand-in for the real ``lci`` MCP server.  The
working-context custom tools call ``lci.context``; this fixture answers those
calls from a JSON control file so the tests can drive resolved, unresolved,
error and indexing-unavailable paths deterministically.  Every tool call is
appended to a JSONL record so the tests can assert exactly what the tools sent.

It speaks newline-delimited JSON-RPC 2.0 over stdio (the slop-mcp stdio MCP
transport).  It is intentionally dependency-free.
"""

import json
import os
import sys


def _send(payload):
    sys.stdout.write(json.dumps(payload) + "\n")
    sys.stdout.flush()


def _control():
    path = os.environ.get("FIXTURE_CONTROL")
    if not path:
        return {}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return {}


def _record(entry):
    path = os.environ.get("FIXTURE_RECORD")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(json.dumps(entry) + "\n")


def _compact_ref(ref):
    out = {"f": ref.get("f")}
    if ref.get("s"):
        out["s"] = ref["s"]
    if ref.get("role"):
        out["role"] = ref["role"]
    if ref.get("x"):
        out["x"] = ref["x"]
    return out


def _handle_context(args, control):
    operation = args.get("operation")
    if operation == "save":
        if control.get("save_is_error"):
            envelope = {
                "success": False,
                "operation": "context",
                "error": control.get("save_error", "save failed"),
            }
            return {"text": json.dumps(envelope), "is_error": True}
        refs = args.get("refs", [])
        manifest = {
            "t": args.get("task", ""),
            "v": "1.0",
            "r": [_compact_ref(r) for r in refs],
        }
        files = {r.get("f") for r in refs}
        envelope = {
            "manifest": json.dumps(manifest, ensure_ascii=False),
            "stats": {
                "ref_count": len(refs),
                "file_count": len(files),
                "total_lines": 0,
            },
            "ref_count": len(refs),
            "file_count": len(files),
        }
        return {"text": json.dumps(envelope, ensure_ascii=False), "is_error": False}

    if operation == "load":
        if control.get("load_is_error"):
            envelope = {
                "success": False,
                "operation": "context",
                "error": control.get("load_error", "load failed"),
            }
            return {"text": json.dumps(envelope), "is_error": True}
        if control.get("load_unavailable"):
            envelope = {
                "operation": "context",
                "available": False,
                "reason": control.get("unavailable_reason", "indexing in progress"),
                "hint": control.get("unavailable_hint", "retry shortly"),
            }
            return {"text": json.dumps(envelope), "is_error": False}

        raw = args.get("from_string", "")
        try:
            manifest = json.loads(raw)
        except ValueError as exc:
            envelope = {
                "success": False,
                "operation": "context",
                "error": "failed to parse manifest string: %s" % exc,
            }
            return {"text": json.dumps(envelope), "is_error": True}

        index = control.get("index", {})
        refs = []
        unresolved = []
        for ref in manifest.get("r", []):
            path = ref.get("f")
            symbol = ref.get("s")
            available = index.get(path)
            if symbol is None:
                if available is None:
                    unresolved.append({"file": path, "reason": "not_found"})
                    continue
                refs.append({"file": path, "source": "// file %s" % path})
                continue
            if available is not None and symbol in available:
                refs.append(
                    {
                        "file": path,
                        "symbol": symbol,
                        "source": "// source of %s" % symbol,
                    }
                )
            else:
                unresolved.append(
                    {"file": path, "symbol": symbol, "reason": "not_found"}
                )

        envelope = {
            "task": manifest.get("t", ""),
            "refs": refs,
            "unresolved": unresolved,
            "stats": {
                "ref_count": len(refs) + len(unresolved),
                "tokens_approx": args.get("max_tokens", 0),
            },
            "budget": args.get("max_tokens", 0),
        }
        return {"text": json.dumps(envelope, ensure_ascii=False), "is_error": False}

    return {
        "text": json.dumps(
            {"success": False, "operation": "context", "error": "bad operation"}
        ),
        "is_error": True,
    }


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except ValueError:
            continue
        method = message.get("method")
        message_id = message.get("id")

        if method == "initialize":
            _send(
                {
                    "jsonrpc": "2.0",
                    "id": message_id,
                    "result": {
                        "protocolVersion": "2025-03-26",
                        "capabilities": {"tools": {}},
                        "serverInfo": {"name": "lci-fixture", "version": "0"},
                    },
                }
            )
        elif method == "notifications/initialized":
            pass
        elif method == "tools/list":
            _send(
                {
                    "jsonrpc": "2.0",
                    "id": message_id,
                    "result": {
                        "tools": [
                            {
                                "name": "context",
                                "description": "fixture context",
                                "inputSchema": {
                                    "type": "object",
                                    "properties": {},
                                },
                            }
                        ]
                    },
                }
            )
        elif method == "tools/call":
            params = message.get("params", {})
            name = params.get("name")
            args = params.get("arguments", {})
            _record({"tool": name, "args": args})
            if name == "context":
                outcome = _handle_context(args, _control())
            else:
                outcome = {
                    "text": json.dumps({"fixture": True, "tool": name}),
                    "is_error": False,
                }
            _send(
                {
                    "jsonrpc": "2.0",
                    "id": message_id,
                    "result": {
                        "content": [{"type": "text", "text": outcome["text"]}],
                        "isError": outcome["is_error"],
                    },
                }
            )
        elif method == "notifications/cancelled":
            pass
        else:
            if message_id is not None:
                _send(
                    {
                        "jsonrpc": "2.0",
                        "id": message_id,
                        "error": {"code": -32601, "message": "method not found"},
                    }
                )


if __name__ == "__main__":
    main()
