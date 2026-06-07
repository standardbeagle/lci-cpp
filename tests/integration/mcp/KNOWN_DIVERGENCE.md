# MCP suite — golden coverage notes

The integration MCP suite pins the C++ MCP server's `tools/call` responses
against captured goldens (`tests/integration/mcp/<tool>/basic.spec.json` +
`tests/integration/goldens/mcp/<tool>/basic.json`), run as the
`All/IntegrationMcpSpec.MatchesGolden/mcp_<tool>_basic` cases.

Most tools are golden-pinned. **Three are envelope-only**, because their payload
embeds a wall-clock field that mutates every run.

## Why three tools can't be byte-pinned

The MCP envelope wraps the payload as a stringified JSON blob:

```
{"result":{"content":[{"type":"text","text":"<JSON-AS-STRING>"}]}}
```

`spec_diff` applies `tiers.ignore` to the outer JSON tree but does **not**
descend into the string-encoded inner blob, so any non-determinism inside it
leaks into the comparison as one opaque string. For most tools the inner payload
is deterministic on the fixed synthetic corpus. These three embed a
wall-clock/measured field directly in the inner JSON:

| Tool | Volatile inner field(s) |
| --- | --- |
| `debug_info` | `"timestamp"` (server local time) |
| `git_analysis` | `"analyzed_at"`, `"analysis_time_ms"` |
| `index_stats` | `"timestamp"` (server local time) |

They are verified at the envelope level (`content[].type` + exit) instead.

## Future tightening

Either path graduates the three into full golden coverage:

1. **Spec-engine extension (preferred):** teach `spec_diff/canonicalize.cpp` to
   recognize a tier path suffixed with `:json`, parse the matched value as JSON,
   and apply the rest of the tier rules to the parsed subtree. Unblocks every
   tool that embeds JSON as `result.content[].text`.
2. **Handler refactor:** move the volatile fields onto the outer `result.meta`
   object, where the existing `result.meta.elapsed_ms` / `result.meta.server_pid`
   ignore rules already apply.

Both are tracked as follow-ups.
