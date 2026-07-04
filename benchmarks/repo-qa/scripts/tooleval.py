#!/usr/bin/env python3
"""Tool-oriented LCI eval: direct MCP calls with expected-result assertions.

Complements the task-oriented QA benchmark: instead of judging an agent's
final answer, this exercises each MCP tool against a real corpus and checks
the raw response — correctness of shape, expected hits, clean error paths,
and latency. A `pre` step lets a case chain (e.g. search -> object_id ->
get_context) the way agents do.

Case schema (tool-cases/<repo>.json):
  {"id": "...", "tool": "search", "args": {...},
   "pre": {"tool": "...", "args": {...}, "extract": {"var": "results.0.object_id"}},
   "expect": {"contains": ["mux.go"], "not_contains": [...],
              "min_results": 1, "results_key": "results", "max_results_len": 15,
              "is_error": false, "max_latency_ms": 500}}
Args may reference extracted vars as "$var".

Usage: tooleval.py --repo chi [--repo sinatra ...]
"""

import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlib as bl

CASES_DIR = os.path.join(bl.BENCH_ROOT, "tool-cases")


class McpSession:
    def __init__(self, lci_bin, cwd):
        self.proc = subprocess.Popen(
            [lci_bin, "mcp"], cwd=cwd, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.next_id = 1
        self._rpc("initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "tooleval", "version": "0"}})
        self._notify("notifications/initialized")

    def _send(self, obj):
        self.proc.stdin.write(json.dumps(obj) + "\n")
        self.proc.stdin.flush()

    def _notify(self, method):
        self._send({"jsonrpc": "2.0", "method": method})

    def _rpc(self, method, params):
        rid = self.next_id
        self.next_id += 1
        self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"MCP server died during {method}")
            msg = json.loads(line)
            if msg.get("id") == rid:
                return msg

    def call_tool(self, name, args):
        start = time.time()
        msg = self._rpc("tools/call", {"name": name, "arguments": args})
        latency_ms = (time.time() - start) * 1000
        if "error" in msg:
            return {"rpc_error": msg["error"]}, latency_ms
        content = msg["result"]["content"][0]["text"]
        return {"text": content}, latency_ms

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def dig(obj, path):
    for part in path.split("."):
        obj = obj[int(part)] if isinstance(obj, list) else obj[part]
    return obj


def resolve_args(args, variables):
    out = {}
    for k, v in args.items():
        if isinstance(v, str) and v.startswith("$"):
            out[k] = variables[v[1:]]
        else:
            out[k] = v
    return out


def response_is_error(payload):
    if "rpc_error" in payload:
        return True
    try:
        d = json.loads(payload["text"])
    except (json.JSONDecodeError, KeyError):
        return False
    return isinstance(d, dict) and (d.get("success") is False or "error" in d)


def run_case(session, case):
    variables = {}
    if "pre" in case:
        pre = case["pre"]
        payload, _ = session.call_tool(pre["tool"], resolve_args(pre["args"], variables))
        if "text" not in payload:
            return {"id": case["id"], "pass": False,
                    "detail": f"pre call failed: {payload}"}
        pre_json = json.loads(payload["text"])
        for var, path in pre["extract"].items():
            try:
                variables[var] = dig(pre_json, path)
            except (KeyError, IndexError, ValueError):
                return {"id": case["id"], "pass": False,
                        "detail": f"pre extract '{path}' missing from {list(pre_json)[:6]}"}

    payload, latency_ms = session.call_tool(case["tool"], resolve_args(case["args"], variables))
    exp = case.get("expect", {})
    failures = []

    if exp.get("is_error"):
        if not response_is_error(payload):
            failures.append("expected an error response, got success")
    elif response_is_error(payload):
        failures.append(f"unexpected error: {str(payload)[:200]}")

    text = payload.get("text", json.dumps(payload))
    for needle in exp.get("contains", []):
        if needle not in text:
            failures.append(f"missing '{needle}'")
    for needle in exp.get("not_contains", []):
        if needle in text:
            failures.append(f"unexpected '{needle}'")

    if "min_results" in exp or "max_results_len" in exp:
        try:
            arr = dig(json.loads(text), exp.get("results_key", "results"))
        except Exception:
            arr = None
        if not isinstance(arr, list):
            failures.append(f"no '{exp.get('results_key', 'results')}' array")
        else:
            if len(arr) < exp.get("min_results", 0):
                failures.append(f"only {len(arr)} results, wanted >= {exp['min_results']}")
            if "max_results_len" in exp and len(arr) > exp["max_results_len"]:
                failures.append(f"{len(arr)} results, cap {exp['max_results_len']}")

    if "max_latency_ms" in exp and latency_ms > exp["max_latency_ms"]:
        failures.append(f"latency {latency_ms:.0f}ms > {exp['max_latency_ms']}ms")

    return {"id": case["id"], "tool": case["tool"], "pass": not failures,
            "latency_ms": round(latency_ms, 1),
            "response_chars": len(text),
            "detail": "; ".join(failures)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", action="append", required=True)
    ap.add_argument("--out", default=None, help="JSON results path")
    args = ap.parse_args()

    cfg = bl.Config()
    all_results = []
    failed = 0
    for repo in args.repo:
        with open(os.path.join(CASES_DIR, f"{repo}.json")) as f:
            cases = json.load(f)["cases"]
        corpus = os.path.join(cfg.defaults["corpus-root"], cfg.repos[repo]["path"])
        session = McpSession(cfg.defaults["lci-bin"], corpus)
        try:
            for case in cases:
                r = run_case(session, case)
                r["repo"] = repo
                all_results.append(r)
                mark = "PASS" if r["pass"] else "FAIL"
                extra = f"  {r['detail']}" if r["detail"] else ""
                print(f"[{mark}] {repo}/{r['id']:34} {r.get('latency_ms', 0):>7}ms "
                      f"{r.get('response_chars', 0):>6}ch{extra}")
                failed += 0 if r["pass"] else 1
        finally:
            session.close()

    print(f"\n{len(all_results) - failed}/{len(all_results)} passed")
    if args.out:
        out = args.out if os.path.isabs(args.out) else os.path.join(bl.BENCH_ROOT, args.out)
        bl.write_json_atomic(out, {"results": all_results})
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
