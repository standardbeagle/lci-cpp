# MCP suite — known divergences

The integration MCP suite mirrors the parity descriptors under
`tests/parity/descriptors/mcp/` and pins the C++ MCP server's
`tools/call` responses against captured goldens. 14 of the 17 mcp
parity descriptors are migrated to deterministic spec.json + golden
pairs under `tests/integration/mcp/<tool>/basic.spec.json` and
`tests/integration/goldens/mcp/<tool>/basic.json`. Three are
**not migrated** because the MCP response payload mixes structural
data (which the integration suite is meant to pin) with non-stable
fields (timestamps, wall-clock-derived timing) that are nested
inside a stringified JSON blob the diff engine cannot
selectively ignore.

## Why three cases can't use the existing tier system

The MCP `tools/call` envelope returns a JSON-RPC response of the form

```
{"result":{"content":[{"type":"text","text":"<JSON-AS-STRING>"}]}}
```

The integration spec diff engine (`tests/lib/spec_diff/`) operates on
the outer JSON tree and supports `tiers.ignore` paths for keys it
should drop before comparison (`result.meta.elapsed_ms`,
`result.meta.server_pid`, etc — see any migrated mcp/* spec). It does
**not** descend into a string-encoded JSON blob to apply ignore-tier
rules, so any non-determinism inside the inner `text` payload propagates
into the canonicalized comparison value as a single opaque string,
producing a stable-tier mismatch on every run.

For 14 of the 17 mcp tools the inner payload is deterministic given a
fixed corpus (the parity-runner-managed `synthetic/multi-lang` checked-in
fixture), so the stringified blob is byte-identical between runs. The
three cases below embed a wall-clock or measured-time field directly in
the inner JSON, which mutates on every invocation:

| Tool | Non-stable inner field(s) | Source |
| --- | --- | --- |
| `debug_info` | `"timestamp":"YYYY-MM-DDTHH:MM:SS"` (server local time) | `mcp_handlers_explore_index.cpp` debug_info handler |
| `git_analysis` | `"analyzed_at":"…Z"`, `"analysis_time_ms":N` | `mcp_handlers_analysis.cpp` git_analysis handler |
| `index_stats` | `"timestamp":"…"` (server local time) | `mcp_handlers_explore_index.cpp` index_stats handler |

Migrating these three would require either (a) descending into
stringified JSON in the diff engine — a cross-suite change that affects
parity tests too, far outside this migration's scope — or (b) reshaping
the C++ MCP handlers to emit timestamps and timing through the outer
envelope's `result.meta` object (which the existing ignore-tier already
covers). Both are real refactors and are tracked separately rather than
landed under the mass-migration task.

## Decision: not migrated, parity-only

These three descriptors continue to exist under
`tests/parity/descriptors/mcp/{debug_info,git_analysis,index_stats}/`
and are exercised by the parity oracle (`ctest -L parity -R parity\.mcp`)
when an `LCI_GO_PATH` is configured. The parity oracle is the
authoritative cross-port check; it already declares the inner
timestamp / timing fields as ignore-tier in its descriptor (see the
`tiers.ignore` field on each parity descriptor — those rules apply to
the outer JSON the same way the integration spec rules do), so on the
parity side the same blob-comparison limitation exists, and the parity
descriptor's `tiers.stable` set is empty enough to not regress on
timestamp drift.

The integration suite does not add coverage on top of the parity oracle
for these three cases, so leaving them out does not reduce the
authoritative cross-port signal.

## Migrated (14)

```
tests/integration/mcp/browse_file/basic.spec.json
tests/integration/mcp/code_insight/basic.spec.json
tests/integration/mcp/context_manifest/basic.spec.json
tests/integration/mcp/find_files/basic.spec.json
tests/integration/mcp/get_context/basic.spec.json
tests/integration/mcp/grep/basic.spec.json
tests/integration/mcp/info/basic.spec.json
tests/integration/mcp/inspect_symbol/basic.spec.json
tests/integration/mcp/list_symbols/basic.spec.json
tests/integration/mcp/search/basic.spec.json
tests/integration/mcp/search_definitions/basic.spec.json
tests/integration/mcp/semantic_annotations/basic.spec.json
tests/integration/mcp/side_effects/basic.spec.json
tests/integration/mcp/tree/basic.spec.json
```

Each is paired with a captured golden under
`tests/integration/goldens/mcp/<tool>/basic.json`. They run as the
`All/IntegrationMcpSpec.MatchesGolden/mcp_<tool>_basic` parametrized
gtest cases inside `lci_integration_tests` (label `integration`).

## Not migrated (3)

### `mcp/debug_info`
- **Source:** `tests/parity/descriptors/mcp/debug_info/basic.parity.json`
- **Why no integration golden:** inner payload contains
  `"timestamp":"<server-local-time>"`. The text blob mutates per run.
- **Coverage:** the parity oracle (`ctest -L parity -R
  parity\.mcp\.debug_info`) covers this descriptor end-to-end with the
  Go reference binary when `LCI_GO_PATH` is set.

### `mcp/git_analysis`
- **Source:** `tests/parity/descriptors/mcp/git_analysis/basic.parity.json`
- **Why no integration golden:** inner payload contains
  `"analyzed_at":"<UTC-timestamp>Z"` and `"analysis_time_ms":<measured>`,
  both wall-clock-derived. The text blob mutates per run.
- **Coverage:** parity oracle path `parity.mcp.git_analysis.basic`.

### `mcp/index_stats`
- **Source:** `tests/parity/descriptors/mcp/index_stats/basic.parity.json`
- **Why no integration golden:** inner payload contains
  `"timestamp":"<server-local-time>"`. The text blob mutates per run.
- **Coverage:** parity oracle path `parity.mcp.index_stats.basic`.

## Future tightening (out of scope for migration 7/8)

Two paths can graduate these three into integration coverage:

1. **Spec-engine extension (preferred):** teach
   `tests/lib/spec_diff/canonicalize.cpp` to recognize a tier path
   suffixed with `:json` and parse the matched value as JSON before
   applying the rest of the tier rules to the parsed subtree. This
   single engine change unblocks all current and future MCP tools that
   embed JSON payloads as `result.content[].text` strings. The Go
   parity descriptors would gain the same expressiveness for free.
2. **Handler refactor:** move volatile fields (`timestamp`,
   `analyzed_at`, `analysis_time_ms`) from inside the stringified
   payload onto the outer `result.meta` object, where the existing
   `result.meta.elapsed_ms` / `result.meta.server_pid` ignore-tier
   rules already apply. This requires touching three handlers and
   adjusting any consumer that reads the field from the inner payload.

Both are tracked as follow-up tasks; this migration intentionally
keeps scope to the mass-migration mechanics.
