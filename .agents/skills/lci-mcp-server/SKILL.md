---
name: lci-mcp-server
description: "Use when: working on `lci mcp` stdio transport, JSON-RPC framing/version negotiation, tools/list schema emit, add_tool/unknown-param guard, MCP error conventions, the HTTP /mcp bridge, info/index_stats/debug_info tools, tool-surface.json drift, or comparing lci's MCP layer to other code MCP servers."
---

# lci MCP server (protocol layer)

`lci mcp` runs a Model Context Protocol server over stdio so an AI assistant can query an
indexed codebase through 15 tools without loading whole files. This skill covers the protocol
layer: framing, lifecycle, tool registration and schema emit, dispatch, error shape, the
readiness gate, and the HTTP `/mcp` bridge that lets many stdio clients share one warmed index.
Individual tool semantics live in the sibling skills listed at the end.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| CLI `lci mcp` | `src/cli/mcp.cpp:86` (`run_mcp`) | bridge-first: reuses the per-root index server via POST /mcp; falls back to in-process index when `--config`, `--include`, `--exclude` or `LCI_ERROR_REPORT` is set (`mcp.cpp:99-115`) |
| stdio bridge loop | `src/cli/mcp.cpp:26` (`run_mcp_bridge`) | forwards each stdin line to `Client::mcp_dispatch`; 204 = notification, 200 = frame, else synthesizes -32603 |
| stdio transport | `src/mcp/server.cpp:109` (`read_message`), `:133` (`write_message`), `:697` (`run`) | newline-delimited JSON-RPC; unparseable lines dropped to stderr, never fatal |
| JSON-RPC methods | `src/mcp/server.cpp:146` (`handle_request`) | `initialize`, `tools/list`, `tools/call`, `ping`; anything else -32601; array/scalar frame -32600 (no batch) |
| protocolVersion negotiation | `include/lci/mcp/server.h:29-31`, `src/mcp/server.cpp:246` (`handle_initialize`) | supports `2024-11-05`, `2025-03-26`, `2025-06-18`; echoes requested version if supported, else newest |
| tools/list wire | `src/mcp/server.cpp:482` (`tools_list_wire`) | ordered_json, tools sorted by name, schema key order type/properties/required (Go jsonschema-go parity) |
| tools/call dispatch | `src/mcp/server.cpp:299` (`handle_tools_call`), `:522` (`execute_tool_call`), `:677` (`drain_calls`) | worker thread per server; readiness gate waited here; `concurrent_ok` tools share `tool_mu_`, others exclusive |
| Unknown-param guard | `src/mcp/server.cpp:308-345` | rejects keys not in `properties` or `aliases`; `search` exempt (own JSON-Schema validator) |
| HTTP `/mcp` bridge | `src/server/server.cpp:1239` (`svr_.Post("/mcp")`), `:378` (`set_mcp_dispatcher`) | 501 when no dispatcher, 204 for notifications; body cap 16 MiB (`server.cpp:1140`) |
| Bridge client | `src/server/client.cpp:409` (`Client::mcp_dispatch`) | 15 min read timeout so a first index build can finish behind one call |
| Wire entry shared by bridge | `src/mcp/server.cpp:560` (`dispatch_wire`) | same dispatch as stdio, returns the frame string; used by `lci mcp` host and `lci server` |
| Standalone server hosting MCP | `src/cli/server.cpp:345-364` | `lci server` also registers all handlers and installs the dispatcher |
| Registration | `src/mcp/runtime.cpp:91` (`register_all_handlers`), `src/mcp/server.cpp:52` (`add_tool`) | `add_tool` after `run()` throws `logic_error` |
| Tool `info` | `src/mcp/handlers_core.cpp:198` (add_tool), `:106` (`handle_info`) | registry-derived help via `find_tool_definition`; `tool=version` returns server + protocol version |
| Tool `index_stats` | `src/mcp/handlers_index.cpp:430` (add_tool), `:102` (`handle_index_stats`) | `concurrent_ok=true` |
| Tool `debug_info` | `src/mcp/handlers_index.cpp:461` (add_tool), `:183` (`handle_debug_info`) | exclusive |
| Response helpers | `src/mcp/response.cpp:9,18,27` | `dump_json_lossy`, `make_error_response`, `make_unavailable_response` |
| Pagination | `include/lci/pagination.h:25` (`normalize_page`), `:39` (`page_has_more`) | used by MCP `list_symbols` (`handlers_explore.cpp:675`) and HTTP `/list-symbols` (`server_endpoints_symbols.cpp:178`) |
| Env `LCI_MCP_MODE` | `src/cli/cli_core.cpp:126` (`is_mcp_mode`) | forces MCP-mode detection; otherwise non-tty stdin implies MCP |
| Contract doc | `docs/TOOLS.md` | per-tool reference; see drift notes below |
| Client registration docs | `docs/src/content/docs/mcp-server.mdx`, `README.md:114-125` | `{"mcpServers":{"lci":{"command":"lci","args":["mcp"]}}}` |

Registered tools (15, verified live via `tools/list` on this build, 0.10.1): `browse_file`,
`callers`, `code_insight`, `context`, `debug_info`, `find_files`, `get_context`, `git_analysis`,
`index_stats`, `info`, `inspect_symbol`, `list_symbols`, `search`, `semantic_annotations`,
`side_effects`. `add_tool` call sites: `handlers_core.cpp:198,218,287,329`;
`handlers_explore.cpp:1049,1100,1134,1169`; `handlers_index.cpp:430,461,492`;
`handlers_analysis.cpp:2249,2279,2317`; `handlers_context.cpp:798`.

## Code map

Entry to core to storage:

- `src/cli/main.cpp:474` registers the `mcp` subcommand; `src/cli/mcp.cpp` is `run_mcp` plus the bridge loop.
- `src/cli/mcp.cpp:117-223` in-process path: `MasterIndex` + `SearchEngine` + `McpRuntime`, embedded `IndexServer` (idle timeout forced to 0, instance registry enabled at `:141`), `WarmupLatch` readiness gate, warmup thread runs `index_directory` then `McpRuntime::warmup`.
- `include/lci/mcp/server.h` types: `ToolProperty`, `ToolDefinition` (properties, required, aliases), `ToolResult`, `ToolHandler`, `McpServer`, protocol constants.
- `src/mcp/server.cpp` transport, dispatch, schema emit (`build_input_schema_ordered` anonymous ns), worker queue.
- `include/lci/mcp/runtime.h:26` `McpRuntime` (annotator, propagator, side_effects, ci_engine), `:45` `WarmupLatch`; `src/mcp/runtime.cpp:16` `McpRuntime::warmup` phases.
- `src/mcp/response.cpp` result envelopes. `src/mcp/validation.cpp` is search-only error shapes (`create_validation_error_response`, `validate_search_business_logic`), consumed at `src/mcp/handlers_search.cpp:371-394`. The unknown-param guard is NOT here; it is in `server.cpp`.
- `src/mcp/formatter_compact.cpp` + `include/lci/mcp/formatter_compact.h` `CompactFormatter` (o=/t=/n= field codes). No production caller; only `tests/mcp_validation_pagination_test.cpp` uses it. The LCF that ships is `code_insight`'s `LCF/1.0` header emitted from `src/mcp/handlers_analysis.cpp` / `src/mcp/insight_sections.cpp`.
- `include/lci/mcp/time_format.h` RFC3339Nano timestamps (Go parity) used by `index_stats`.
- `src/mcp/handlers_index.cpp` `info`-adjacent diagnostics: `handle_index_stats`, `handle_debug_info`, `handle_git_analysis`.
- `src/server/server.cpp:1239` POST /mcp; `include/lci/server/server.h:294-307` pool sizing rationale (`kMaxQueuedRequests=32`, workers = 2x) because bridge calls park on the latch.
- `src/server/client.cpp:409` `Client::mcp_dispatch`.
- Tests: `tests/mcp_server_test.cpp` (36 tests: handshake, version negotiation, batch rejection, concurrent_ok locking, readiness gate, unknown param, UTF-8 lossy dump, `RegistersAll15Tools`, `EverySchemaParameterHasAReader`), `tests/mcp_validation_pagination_test.cpp` (30: validation shapes, CompactFormatter, PageWindow), `tests/json_schema_smoketest.cpp` (4), `tests/mcp_handlers_core_test.cpp` (`InfoHandler.*`, `InfoRegistryFixture.*`), `tests/mcp_handlers_explore_index_test.cpp`, `tests/server_test.cpp:1846` (/mcp dispatcher).
- Integration: `tests/integration/spec_migration_test.cpp:420-440` (`IntegrationMcpSpec` walks `tests/integration/mcp/<tool>/<name>.spec.json`), goldens under `tests/integration/goldens/mcp/<tool>/` (27 json files, 19 tool dirs incl. `info`, `index_stats`, `debug_info`). `tests/integration/mcp/KNOWN_DIVERGENCE.md` records accepted divergences.
- Benchmarks: `benchmarks/repo-qa/comprehension/surface/tool-surface.json` (committed tools/list snapshot, 15 tools), gated by `benchmarks/repo-qa/tests/test_tool_surface.py:45` (`test_manifest_equals_the_live_tools_list`); 14 readers under `benchmarks/` (`grep -rl tool-surface.json benchmarks`). `benchmarks/repo-qa/scripts/mock_lci_mcp.py` is the mock server for selection benches.
- Docs: `docs/TOOLS.md`, `docs/src/content/docs/mcp-server.mdx`, `docs/src/content/docs/cli-usage.mdx:77`, `README.md:114`, `docs/reviews/2026-09-03-review.md:148-160` (S4 /mcp findings), `docs/plans/2026-07-18-hardcoded-response-shape-ab-design.md`.

## Config knobs

| Key / env | Default | Struct | Effect on this area |
|---|---|---|---|
| `server { idle_timeout_sec }` | 1800 | `ServerConfig::idle_timeout_sec` `include/lci/config.h:66` | `lci mcp` overrides to 0 for its embedded server (`mcp.cpp:136`) |
| `server { max_instances }` | 8 | `ServerConfig::max_instances` `config.h:71` | embedded MCP server is in the registry (`mcp.cpp:141`) so it participates in eviction |
| `insight { error_report }` | `"off"` | `InsightConfig::error_report` `config.h:155` | `"capture"` makes `lci mcp` write a report to `$XDG_STATE_HOME/lci/error-reports/` at exit (`mcp.cpp:241`, `handlers_analysis.cpp:2380`) |
| `LCI_ERROR_REPORT` | unset | env override of the above | also forces the in-process path (`mcp.cpp:102`) |
| `LCI_MCP_MODE=1` | unset | `is_mcp_mode` `cli_core.cpp:126` | forces MCP-mode behaviour regardless of tty |
| `--config <path>`, `--include`, `--exclude` | `.lci.kdl` | `GlobalFlags` | any override disables the bridge and builds a local index |
| User defaults file | `$XDG_CONFIG_HOME/lci/config.kdl` or `~/.config/lci/config.kdl` | `src/config/config.cpp:641` | layered under the project file; this host has `error_report "capture"` there, which is why probes emit `error report captured` |

No config key controls the tool set, protocol versions, pagination caps (`normalize_page` defaults 50 / cap 500), or the 16 MiB /mcp body cap; those are compile-time.

## Invariants and traps

- Framing is newline-delimited JSON-RPC, not LSP Content-Length. It was Content-Length until `0e9f085`; every real client hung while the suite stayed green because tests spoke the same wrong framing (`src/mcp/server.cpp:110-113`, memory `mcp-stdio-framing-fixed`).
- No JSON-RPC batch: an array frame answers -32600 with null id (`server.cpp:150`, test `BatchArrayFrameIsInvalidRequest`). Spec 2025-06-18 removed batching, so this is conformant for the newest version and a gap for 2024-11-05 clients that batch.
- Handshake never waits on the index; only `tools/call` waits, unbounded, on `WarmupLatch` (`mcp.cpp:152-163`). A cold one-shot client that pipes and closes stdin still gets answers because `run()` drains the worker after EOF (`server.cpp:730-738`).
- `stop()` cannot interrupt `getline`; closing stdin is the caller's job (`server.cpp:742`).
- `add_tool` after `run()` throws: `enqueue_call` holds raw pointers into `registered_tools_` (`server.cpp:52-63`). First registration wins on duplicate names, in both `tools/list` and dispatch (`DuplicateToolNameFirstRegistrationWinsEverywhere`).
- `concurrent_ok=true` needs a written proof comment at the call site (search, find_files, info, index_stats, explore tools have them). Unaudited handlers take the exclusive lock (`server.cpp:522-535`, karpathy rule 3).
- Unknown-param guard is dispatch-level and skips `search`; `aliases` are accepted but not advertised in the schema (`server.h:52-57`). Adding a param a handler reads without declaring it fails `EverySchemaParameterHasAReader` and the guard rejects callers.
- Error convention: `{"success":false,"error":...,"operation":...}` with `isError:true` only for caller mistakes and internal failures; missing preconditions return `available:false` + `reason` + `hint` and are NOT errors (`docs/TOOLS.md:26-33`, `response.cpp:27`). Readiness-gate failure is JSON-RPC -32603 "index unavailable", not a tool result.
- Every wire serialization goes through `dump_json_lossy`; a strict dump on non-UTF-8 source bytes aborted the run loop (found by fuzz_get_context, `response.cpp:6-8`).
- tools/list field order is pinned to Go's jsonschema-go emit (`server.cpp:482-520`); `handle_tools_list` and `build_input_schema` return plain `nlohmann::json` (alphabetised) and are retained for ABI only. Do not use them for wire output.
- A schema or description edit must re-pin `tool-surface.json` and `docs/TOOLS.md` and run `pytest benchmarks/repo-qa/tests/test_tool_surface.py`; ctest cannot see the drift (`.claude/rules/test-iteration-discipline.md:143`, `.claude/rules/bench-harness-oracle-independence.md:323`). `7949bc6` left that suite red for days.
- The embedded index server inside `lci mcp` calls `enable_instance_registry` (`mcp.cpp:139-141`) so `lci servers` / `lci shutdown --all` see it. `README.md:150-153` still says it "stays out of it deliberately". A self-stop from the RSS cap or root deletion exits the whole process via `std::_Exit(0)` (`mcp.cpp:179-185`).
- Bridge pool: parked /mcp calls during warmup consumed the default 8-worker pool and muted `/ping` and `/shutdown` (`docs/reviews/2026-09-03-review.md:155-157`); fixed by `kMinServerWorkerThreads = 2 * kMaxQueuedRequests` (`server.h:300-307`).
- Doc gap: `docs/src/content/docs/http-socket-api.mdx` does not document POST /mcp.
- Findings from the 2026-09-08 mapping (fixed or filed): see docs/reviews/2026-09-08-skill-mapping-findings.md.
- `CompactFormatter` (`src/mcp/formatter_compact.cpp`) has zero production callers; the class the README calls LCF is not what ships.
- `index_stats` accepts `include_watch_mode` but ignores it (`docs/TOOLS.md:302`).
- Dogfood gap: `lci def dispatch_wire` returns "no definition" for a member defined out-of-class; `lci def McpServer::dispatch_wire` works. Bare member names need the qualified form.
- Corpus for the tool-surface gate must be a real directory; the pytest skips (does not fail) when `LCI_BIN` or the corpus is absent, by design (rule 8.3).

## Probe recipes

Binary: `build/release/src/lci`. Use `--include '*.cpp'` to force the in-process path so a probe does not touch a shared server; drop it to exercise the bridge.

```sh
# Handshake + tools/list + batch rejection + unknown-param guard + index_stats, in-process
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
 '[{"jsonrpc":"2.0","id":3,"method":"ping"}]' \
 '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"info","arguments":{"bogus":1}}}' \
 '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"index_stats","arguments":{}}}' \
 | build/release/src/lci mcp --include '*.cpp' -r tests/parity

# Count tools
printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
 | build/release/src/lci mcp --include '*.cpp' -r tests/parity 2>/dev/null \
 | python3 -c 'import sys,json; d=json.loads(sys.stdin.readline()); print(len(d["result"]["tools"]))'

# Bridge path against the running per-root server (spawns one if absent)
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"ping"}' | build/release/src/lci mcp -r .

# info / debug_info
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"info","arguments":{"tool":"version"}}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"debug_info","arguments":{"mode":"overview"}}}' \
 | build/release/src/lci mcp --include '*.cpp' -r tests/parity
```

Tests (build only the binary you run, per `.claude/rules/test-iteration-discipline.md`):

```sh
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='McpStdioTest.*:McpServerTest.*:McpResponse.*:InfoHandler.*:InfoRegistryFixture.*:PageWindow.*:PageHasMore.*:CompactFormatter.*:JsonSchemaSmoketest.*'
ctest --test-dir build/release -R 'McpStdioTest|McpServerTest' -j4
# Goldens: tests/integration/mcp/<tool>/*.spec.json -> tests/integration/goldens/mcp/<tool>/*.json
ctest --test-dir build/release -R lci_integration_suite --output-on-failure
build/release/tests/lci_integration_tests --gtest_filter='*IntegrationMcpSpec*'
# Tool-surface gate (pytest, outside ctest)
/usr/bin/python3 -m pytest benchmarks/repo-qa/tests/test_tool_surface.py
```

Navigation with lci itself:

```sh
build/release/src/lci def McpServer -r .
build/release/src/lci def 'McpServer::dispatch_wire' -r .
build/release/src/lci refs set_mcp_dispatcher -r .
build/release/src/lci search add_tool -r .
```

## Product comparison

Comparable products for the MCP layer: Serena (LSP-backed MCP), code-index-mcp, GitHub MCP server, Sourcegraph MCP, JetBrains MCP server, mcp-language-server, Context7 (docs, not code), and IDE-native tools (Claude Code / Cursor built-in grep, glob, read). All competitor facts below are `(unverified, from model knowledge)` unless a probe command is given.

lci's claims in this area and how to check them:

| Claim | Where made | How to measure | Existing numbers |
|---|---|---|---|
| 15 tools, all engine-backed | live surface, verified via `tools/list` | `tools/list` probe above; `pytest benchmarks/repo-qa/tests/test_tool_surface.py` | `tool-surface.json` (15) |
| Token-dense output (LCF) | `README.md:119`, `docs/TOOLS.md:11-15` | `code_insight` header `tokens=` field; compare bytes of the same facts in JSON vs LCF | `benchmarks/repo-qa/ANALYSIS-comprehension-surface.md` (annotated arm won 4 of 14 tools, parity 10; not a production-output claim) |
| Fewer context tokens than grep-based reading | `README.md:4`, `.claude/rules/karpathy-principles.md` | `benchmarks/repo-qa/` discovery sweep; `ANALYSIS-discovery-sweep.md` | no committed measurement; the C++ bench found the A/B arms non-disjoint (bench rule 12), so any inherited Go-era number is unre-measured |
| Handshake independent of index size | `mcp.cpp:193-200` | time `initialize` on a large root while indexing; test `HandshakeDoesNotWaitOnTheReadinessGate` | none published |
| Spec-conformant stdio framing + version negotiation | `server.cpp:110`, `server.h:29` | MCP Inspector or Claude Code connects; `InitializeEchoesSupportedProtocolVersion` | fixed in `0e9f085` |
| Shared index across N stdio clients | `mcp.cpp:93-115` | start two `lci mcp` for one root, confirm one `lci servers` entry and one index build in stderr | none published |

Comparison axes:

| Axis | lci | How to check the competitor | Notes |
|---|---|---|---|
| Tool count / surface | 15 tools, alphabetised `tools/list`, schemas with ordered keys | run the competitor's `tools/list` over stdio, count | Serena exposes roughly 20-30 tools incl. editing and memory (unverified); GitHub MCP is API-shaped, not index-shaped (unverified) |
| Output density | JSON per tool; LCF only for `code_insight`; `object_id` short ids for drill-down | count tokens of one equivalent answer (e.g. symbol outline) from each server | no committed cross-server token measurement exists; `ANALYSIS-comprehension-surface.md` measures lci arms only |
| Pagination | `max`/`offset`/`has_more`, default 50 cap 500, one header shared by MCP and HTTP | inspect competitor schemas for cursor/offset params | `search` hard-caps at 100 (`handlers_search.cpp`), a known design residue |
| Error convention | `isError` only for caller/internal faults; `available:false` for missing preconditions; unknown params rejected with allowed list | send an unknown param and a call before indexing to the competitor | most MCP servers ignore unknown params (unverified) |
| Startup / index time | in-process: index on a warmup thread, handshake immediate; bridge: zero index cost after the first client | time to first successful `tools/call` on a fixed corpus, cold and warm | `index_stats.index_time_ms` reports the build; parity corpus 13 files indexed in 16 ms on this host |
| Protocol conformance | 3 protocol revisions, no batch, no `notifications/cancelled` handling, `listChanged:false`, no resources/prompts capabilities | run MCP Inspector's conformance checks | cancellation stub at `server.cpp:400` |
| Transport | stdio plus HTTP POST /mcp over a Unix socket (not Streamable HTTP) | check whether competitor offers SSE/Streamable HTTP | lci's /mcp is a private bridge, not a spec transport |
| Language coverage behind the tools | 13 tree-sitter languages, no LSP | LSP-backed servers inherit each language server's semantics | lci has no type-level hover or rename |

Honest gaps versus the field, from repo evidence: no disk persistence of the index (every server start re-indexes; `README.md:161`); no resources or prompts capability; no Streamable HTTP transport; no cancellation; no batch for 2024-11-05 clients; no editing tools (read-only surface); `search` caps at 100 results; the LCF density claim is measured only within lci arms, never against another server's output.

## Related skills

lci-feature-map (router), lci-search, lci-symbol-navigation, lci-get-context, lci-parsing-languages,
lci-code-insight, lci-side-effects, lci-git-analysis, lci-indexing-pipeline, lci-server-lifecycle,
lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation
