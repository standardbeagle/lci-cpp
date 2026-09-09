---
name: lci-symbol-navigation
description: "Use when: changing or debugging lci def/refs/callers/tree/symbols/inspect/browse/list, MCP list_symbols/inspect_symbol/browse_file/find_files/callers, the symbol store, reference tracker, call resolution, import resolver, object IDs, pagination, or comparing lci navigation against ctags, LSP, SCIP, cscope, GNU global, Serena."
---

# LCI Symbol Navigation

Symbol navigation answers "where is X defined, who references it, who calls it, what does this file contain" from the in-memory index. Definitions, symbol listing, outlines and callers read the reference tracker's RCU snapshot (indexed symbols plus resolved call edges); `def` and `refs` run text search over the trigram index and decorate hits with indexed symbol data afterwards. All three surfaces (CLI, MCP, HTTP) share one server; the CLI is a thin HTTP client.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| CLI `def` (alias `d`) | `src/cli/main.cpp:544` -> `run_def` `src/cli/commands.cpp:218` | POST /definition; prints `path:line [kind] signature` |
| CLI `refs` (alias `r`) | `src/cli/main.cpp:555` -> `run_refs` `src/cli/commands.cpp:423` | POST /references; `--all`, `--count`, `--terse`, `-m` (default 100). Splits code vs lexical (comment/string) hits client-side |
| CLI `callers` | `src/cli/main.cpp:585` -> `run_callers` `src/cli/commands.cpp:567` | POST /callers; `-m` default 50 |
| CLI `tree` (alias `t`) | `src/cli/main.cpp:608` -> `run_tree` `src/cli/commands.cpp:709` | POST /tree; `-d` default 5, `--compact`, `--agent`, `--metrics` (metrics fetched per node via /browse-file, `annotate_tree_metrics` `commands.cpp:454`) |
| CLI `list` (alias `ls`) | `src/cli/main.cpp:650` -> `run_list` `src/cli/commands.cpp:816` | Files that would be indexed; scanner only, no server |
| CLI `symbols` (alias `sym`) | `src/cli/main.cpp:868` -> `run_symbols` `src/cli/commands_query.cpp:375` | POST /list-symbols; `-k`, `-f` glob, `-n`, `--receiver`, complexity bounds, `-s`, `-m` 50 |
| CLI `inspect` (alias `insp`) | `src/cli/main.cpp:920` -> `run_inspect` `src/cli/commands_query.cpp:519` | POST /inspect-symbol; `--type`, `-f`, `--include` |
| CLI `browse` (alias `br`) | `src/cli/main.cpp:948` -> `run_browse` `src/cli/commands_query.cpp:661` | POST /browse-file; `--imports`, `--stats`, `-s` default `line` |
| MCP `list_symbols` | `src/mcp/handlers_explore.cpp:1049` (handler `:661`) | `kind` required; `max` clamp [1,500] via `normalize_page` |
| MCP `inspect_symbol` | `src/mcp/handlers_explore.cpp:1100` (handler `:740`) | `name` or `id`; `include` default `all` incl. `type_hierarchy` |
| MCP `browse_file` | `src/mcp/handlers_explore.cpp:1134` (handler `:861`) | `file` glob or `file_id`; `show_stats`; `ambiguous[]` on basename collision |
| MCP `callers` | `src/mcp/handlers_explore.cpp:1169` (handler `:832`) | `name`; `max` clamp [1,1000]; shared report builder |
| MCP `find_files` | `src/mcp/handlers_core.cpp:329` -> `handle_find_files` `src/mcp/handlers_find_files.cpp` | fuzzy + glob; `filter`, `flags` (`ci`,`exact`), `directory`, `include_hidden`; `max` clamp [1,200] |
| HTTP POST `/definition` | `src/server/server.cpp:1201` -> `handle_definition` `src/server/handlers_search.cpp:141` | `search_definitions` text hit, then nearest indexed symbol supplies kind + signature |
| HTTP POST `/references` | `src/server/server.cpp:1205` -> `handle_references` `src/server/handlers_search.cpp:235` | `search_with_options` text search, 5 context lines |
| HTTP POST `/callers` | `src/server/server.cpp:1209` -> `handle_callers` `src/server/handlers_search.cpp:301` | `max_callers` default 50, cap 1000 |
| HTTP POST `/tree` | `src/server/server.cpp:1213` -> `handle_tree` `src/server/server_endpoints.cpp:469` | `function_name`, `max_depth` (0 -> 10); `build_function_tree` |
| HTTP POST `/list-symbols` | `src/server/server.cpp:1221` -> `src/server/server_endpoints_symbols.cpp:124` | Same filter set as MCP; `object_id` base-63 |
| HTTP POST `/inspect-symbol` | `src/server/server.cpp:1225` -> `src/server/server_endpoints_symbols.cpp:338` | |
| HTTP POST `/browse-file` | `src/server/server.cpp:1229` -> `src/server/server_endpoints_symbols.cpp:538` | |
| HTTP POST `/symbol` | `src/server/server.cpp:1181` -> `handle_symbol` `src/server/server_endpoints.cpp:157` | Lookup by numeric `symbol_id` |
| HTTP POST `/fileinfo` | `src/server/server.cpp:1185` -> `handle_fileinfo` `src/server/server_endpoints.cpp:202` | Lookup by numeric `file_id`; symbol count |
| Client wrappers | `src/server/client.cpp:148` (`get_definition`), `:176` (`get_callers`), `:190` (`get_references`), `:216` (`get_tree`), `:331` (`list_symbols`), `:362` (`inspect_symbol`), `:382` (`browse_file`) | Unix-socket HTTP |

There is no MCP `tree` tool; `tests/integration/goldens/mcp/tree/basic.json` pins the `unknown tool "tree"` error. Call trees over MCP come from `get_context` (`include_call_hierarchy`, see lci-get-context).

## Code map

Entry
- `src/cli/main.cpp` — CLI11 registration of every verb above; aliases; `std::exit(run_*)` callbacks.
- `src/cli/commands.cpp` — `run_def`, `run_refs` (lexical partition using `ast_filters` comment/string masks), `run_callers`, `run_tree`, `run_list`.
- `src/cli/commands_query.cpp` — `run_symbols`, `run_inspect`, `run_browse`; text and JSON renderers.
- `src/cli/tree_formatter.h` — `format_tree`, `format_text`, `format_compact`, agent mode; pure, unit-tested.
- `src/cli/symbol_filters.h` — `glob_match`, `apply_file_glob`, `sort_symbols`, `apply_max_limit` for the CLI-side pass.
- `src/cli/name_aggregation.h` — caller-name folding used in text output (test callers demoted).
- `src/cli/ast_filters.h` — comment/string classification reused by `refs` to split code from lexical hits.
- `src/cli/cli_core.cpp` — `ensure_server_running`, `load_config_with_overrides` (every verb starts here).
- `src/mcp/handlers_explore.cpp` — `handle_list_symbols`, `handle_inspect_symbol`, `handle_browse_file`, `handle_callers`; `build_explore_symbol`, `build_inspect_result`, `matches_list_filters`, `sort_enhanced_symbols`, `path_matches_glob`.
- `src/mcp/handlers_find_files.cpp` — `handle_find_files`, `wildcard_match` (`:37`, declared in `include/lci/mcp/handlers_core_shared.h:63`), Go-shape fuzzy scoring, miss hints.
- `include/lci/mcp/handlers_core_shared.h` — `clamp_int`, `to_lower`, `similar_symbol_suggestions`, `wildcard_match`.
- `src/server/handlers_search.cpp` — HTTP `/definition`, `/references`, `/callers`.
- `src/server/server_endpoints.cpp` — `/symbol`, `/fileinfo`, `/tree` (tree JSON node shape).
- `src/server/server_endpoints_symbols.cpp` — `/list-symbols`, `/inspect-symbol`, `/browse-file`; `parse_symbol_kinds`, `kind_matches`.

Core
- `src/core/callers_report.cpp`, `include/lci/core/callers_report.h` — `build_callers_report`: one JSON builder for HTTP `/callers` and MCP `callers` (definitions, grouped confirmed callers, dynamic and unresolved sites split out).
- `src/core/reference_tracker.cpp`, `include/lci/core/reference_tracker.h` — `ReferenceTracker` and its RCU `Snapshot` (`pin()` / `load_snapshot`): `find_symbols_by_name` (`:621`), `get_enhanced_symbol` (`:631`), `get_file_enhanced_symbols` (`:632`), `get_symbol_at_line` (`:641`), `collect_callers` (`:614`, `CallersResult` `:604`), `build_function_tree` (`:463`), `get_caller_names`, `get_callee_symbols`, type relationships (`get_implementors`, `get_base_types`). `resolve_reference_target` (`reference_tracker.cpp:1280`) is the name plus receiver-type call resolver; `derive_receiver_type` (`:637`) writes `receiver_type` at enrichment.
- `include/lci/core/reference_tracker.h:90` `ImportResolver`, `src/core/import_resolver.cpp` — line-based import extraction per language family (Go, JS/TS, Python, Rust, C#, C-family, PHP, Zig), feeds import-aware disambiguation.
- `include/lci/core/reference_tracker.h:161` `PostingsIndex` — token postings used by `search_references`.
- `src/core/symbol_store.cpp`, `include/lci/core/symbol_store.h` — `SymbolStore` (`get_symbols_by_file`, `get_symbols_by_name`, `get_entry_points`) and `SymbolLocationIndex` (`find_symbol_at_position`, `find_symbol_id_at_line`).
- `src/analysis/call_graph.cpp`, `include/lci/analysis/call_graph.h` — `CallGraph` (Tarjan SCC, reach, betweenness, Louvain). Consumed by insight and module analysis, not by the navigation endpoints; callers/tree walk the tracker directly.
- `include/lci/symbol.h` — `Symbol` (name, type, file_id, line/column, end_line, visibility, `declaration_only`), `EnhancedSymbol` (id, ref counts, `scope_chain`, `signature`, `doc_comment`, `complexity`, `receiver_type`, flags).
- `include/lci/reference.h` — `ReferenceType`, `RefStrength`, `Reference`, `Import`.
- `include/lci/scope.h` — `BlockBoundary`, `ScopeInfo`, `SymbolScope` (scope chains behind `Scope:` lines and receiver derivation).
- `include/lci/idcodec.h` — base-63 `encode_symbol_id` / `decode_symbol_id`, `encode_file_id`, `CompositeID` (ids are `file<<32|local`, not sequential).
- `include/lci/pagination.h` — `PageWindow` / `normalize_page`: `max<=0` -> default, cap, negative offset -> 0, `has_more`.
- `src/indexing/master_index_search.cpp:121` `search_definitions`, `:129` `search_references` — the text-search paths behind `/definition` and `/references`.

Tests
- `tests/cli_test.cpp` — `CliDefDiagnosisTest`, `CliRefsPartitionTest`, `SymbolFiltersSort`, `NameAggregation`, `TreeFormatter*`, `CommandsJsonTest`.
- `tests/mcp_handlers_explore_index_test.cpp` — `ExploreIndexTestFixture` (ListSymbols*, InspectSymbol*, BrowseFile*, Callers*), `ExploreGlobFilterTest`, `BrowseFileDeterminismTest.AmbiguousBasenameResolvesSmallestPath`.
- `tests/mcp_handlers_core_test.cpp` — `FindFilesHiddenAncestor` and find_files fixtures.
- `tests/reference_tracker_test.cpp` — `ReferenceTrackerTest` (FunctionTree, ResolvesByReceiverTypeScope, ForeignReceiver*, TypedReceiver*, Interface/Embedded/Trait resolution), `ImportResolverTest`, `PostingsIndex*`.
- `tests/symbol_store_test.cpp`, `tests/idcodec_test.cpp`, `tests/call_graph_test.cpp`, `tests/mcp_validation_pagination_test.cpp` (`PageWindow`, `PageHasMore`).
- `tests/server_test.cpp` — `DefinitionKindSignatureTest.RealKindAndSignatureAcrossLanguages`.
- `tests/integration/mcp_tools_integration_test.cpp` — direct handler calls for find_files, list_symbols, inspect_symbol, browse_file.
- Goldens: `tests/integration/goldens/cli/symbols/` (def, refs, refs-json, tree, symbols, inspect*, browse*, list), `goldens/cli/callers/basic.txt`, `goldens/http/{definition,references,tree,list-symbols,list-symbols-receiver,inspect-symbol,browse-file,fileinfo}.json`, `goldens/mcp/{list_symbols,inspect_symbol,browse_file,callers,find_files,tree}/`. Specs beside them under `tests/integration/{cli,http,mcp}/`; regenerate with `LCI_UPDATE_GOLDENS=1` (`tests/integration/README.md`).

Docs
- `docs/TOOLS.md` (`list_symbols` :200, `inspect_symbol` :230, `browse_file` :256, `find_files` :110), `docs/src/content/docs/cli-usage.mdx`, `docs/src/content/docs/http-socket-api.mdx`, `README.md:98-125`.
- `docs/plans/2026-06-17-scope-type-resolution.md` — receiver-type call resolution design (SCIP base case, no type checker).
- `docs/reviews/2026-09-03-review.md` S6 — explore-handler defects and their fixes.

## Config knobs

No `.lci.kdl` key is specific to this area. What reaches it:
- `include` / `exclude` (`Config::include`, `Config::exclude`, `include/lci/config.h:176-177`) and `index.respect_gitignore` (`IndexConfig`, `:48`) decide which files exist for `list`, `find_files`, `browse`.
- `index.max_parse_file_size` (2 MB default, `:25`) — larger files are indexed for text but have no symbols, so `def`/`browse` miss them while `refs` still hits.
- `search.max_results` (100, `:120`) and `search.max_context_lines` (100, `:127`) bound `/definition` and `/references` since both are search calls; CLI `refs -m` overrides per call.
- `attributes` / test path rules (`Config::attributes`, `:188`) drive the test-caller demotion in `name_aggregation.h`.
- Env: `LCI_MCP_MODE` (forces MCP stdio mode), `LCI_UPDATE_GOLDENS=1` (test-only). Only `LCI_ERROR_REPORT` and `LCI_MCP_MODE` are read by the binary.

## Invariants and traps

- `/definition` and `/references` are text searches, not symbol-table lookups (`src/server/handlers_search.cpp:141-300`). `def` is decorated afterwards with the nearest indexed symbol's kind and signature; `refs` returns every textual occurrence and the CLI masks comment/string hits (`src/cli/commands.cpp`, `CliRefsPartitionTest`). Resolved, receiver-aware edges exist only in `callers`, `tree`, `inspect` callers/callees and `get_context`.
- `callers`, `list_symbols` and `browse_file` outputs sort by (file_path, line) or name before emit; `browse_file` picks the lexicographically smallest path on basename collision and reports `ambiguous[]` (`handlers_explore.cpp:861-1046`). Hash-order determinism must be proven in separate processes (`.claude/rules/karpathy-principles.md` rule 4).
- Confirmed callers exclude dynamic (foreign receiver, bodiless declaration) and unresolved sites; those are listed separately and never counted (`include/lci/core/callers_report.h:27-33`, `reference_tracker.h:604`).
- Receiver-type resolution is syntactic (local type env, `Type.M` qualified refs); interface and dynamic dispatch stay candidate sets, same as gopls/SCIP (`docs/plans/2026-06-17-scope-type-resolution.md`). A typed-receiver miss does not fall back to bare-name matching (`TypedReceiverMissSkipsNameFallback`).
- `wildcard_match` lives in `handlers_find_files.cpp:37` and is declared in `handlers_core_shared.h:63`; promoting it broke overload resolution against private `to_lower`/`clamp_int` copies (`.claude/rules/worktree-isolation-and-goldens.md` rule 7). Do not add a second matcher; `handlers_explore.cpp:176` `path_matches_glob` is the explore-side glob (equality, basename, suffix, `*`/`?`), pinned by `ExploreGlobFilterTest`.
- Pagination semantics are shared through `include/lci/pagination.h`; before it MCP and HTTP disagreed on `max<=0` and negative offsets.
- Any change to a field's meaning in these responses must grep `tests/integration/goldens/` and re-pin in the same commit (`.claude/rules/test-iteration-discipline.md` rule 4). Any MCP schema or description edit must also run `pytest benchmarks/repo-qa/tests/test_tool_surface.py` and re-pin `benchmarks/repo-qa/comprehension/surface/tool-surface.json` plus `docs/TOOLS.md` (rule 7). That snapshot is RED on main today: it still carries `browse_file.show_imports` (`tool-surface.json:45`) and `inspect_symbol.max_depth`.
- `/tree` emits fixed zeros for `dependent_count`, `edit_risk_score`, `impact_radius` and null `annotations`/`safety_notes`/`stability_tags` (`server_endpoints.cpp:469-590`); only `dependency_count` and locations are real. `tree --metrics` costs one `/browse-file` round trip per node.
- `receiver_type` filter had no writer until `eb85054`; `derive_receiver_type` now fills it at enrichment. `type_info` is still writer-less (memory `ref-tracker-memory-refactor`).
- Findings from the 2026-09-08 mapping (fixed or filed): see docs/reviews/2026-09-08-skill-mapping-findings.md.
- Extraction quality seen on this repo: `lci symbols` marks C++ function-local variables `[exported]` (`src/core/callers_report.cpp:106` `a`, `b`) and `browse --stats` counts 41/41 exported; `lci inspect build_callers_report` shows `Doc: // namespace`, the closing-namespace comment of the previous block. Visibility and doc-comment attribution are extractor issues (lci-parsing-languages), but they surface here.
- Dogfood gaps: `lci refs` cannot restrict to resolved (call-graph) references; `lci def` cannot take an object id; `lci symbols` has no `--flags` (async/variadic) while MCP `list_symbols` does; no CLI for `find_files`.
- No disk persistence: every server start re-indexes (`README.md:160-161`), so first navigation call pays index time.

## Probe recipes

Binary: `build/release/src/lci`, root via `-r`. All examples use `-r .`.

```
lci def build_callers_report -r .
lci refs build_callers_report --terse -r .        # add --all for lexical hits, --count for total
lci callers build_callers_report --json -r .
lci tree run_callers -d 2 --metrics -r .
lci symbols -k method --receiver IndexServer -s complexity -m 10 -r .
lci inspect handle_callers --type function -f 'src/mcp/*' --json -r .
lci browse src/core/callers_report.cpp --stats --imports -r .
lci list -v -r . | head
```

MCP over stdio (one JSON-RPC line each, newline framed):
```
printf '%s\n%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"x","version":"0"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"callers","arguments":{"name":"build_callers_report","max":5}}}' \
  | build/release/src/lci mcp -r .
```
Swap `name` for `list_symbols` (`{"kind":"func","file":"src/core/*","sort":"complexity","max":5}`), `inspect_symbol` (`{"name":"handle_callers","include":"callers,callees"}`), `browse_file` (`{"file":"callers_report.cpp","show_stats":true}`), `find_files` (`{"pattern":"*.h","filter":"src/mcp"}`).

Targeted tests (build only `lci_tests` / `lci_integration_tests`, per `.claude/rules/test-iteration-discipline.md`):
```
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='ExploreIndexTestFixture.*:ExploreGlobFilterTest.*:BrowseFileDeterminismTest.*:CliRefsPartitionTest.*:CliDefDiagnosisTest.*:TreeFormatter*:ReferenceTrackerTest.*:ImportResolverTest.*:SymbolStoreTest.*:SymbolLocationIndexTest.*:PageWindow.*:DefinitionKindSignatureTest.*'
ctest --test-dir build/release -R lci_integration_suite --output-on-failure
```
Goldens: `tests/integration/goldens/cli/symbols/`, `goldens/cli/callers/`, `goldens/http/`, `goldens/mcp/{list_symbols,inspect_symbol,browse_file,callers,find_files}/`.

## Product comparison

Comparable tools: universal-ctags (tags file, definitions only); cscope/GNU global (C-centric def/ref/caller DBs); LSP servers (clangd, gopls, tsserver: compiler-grade go-to-def, find-refs, call hierarchy, per-language); Sourcegraph SCIP indexers; GitHub code navigation (tree-sitter based, name lookup); Serena MCP (LSP-backed symbol tools); Aider repo-map / Cursor indexing (ranked symbol maps for prompts, not navigation queries).

lci claims in this area:
- "definition + reference lookup" and "function call hierarchy with annotations" (`README.md:103-104`). Measure: run the probe recipes above against a corpus and compare `def` hits to the true definition set; goldens under `tests/integration/goldens/cli/symbols/` are the pinned behaviour.
- Sub-millisecond queries (`README.md:3`). Measure: `lci tree` prints its own generation time (1.3 ms on this repo, in-process on a warm server); for HTTP, time `Client::get_definition` calls in a loop. Existing numbers: `docs/performance/test-suite-baseline.md`, `tests/benchmarks/baseline/`, `tests/integration/real_project_performance_test.cpp`. No committed number exists for def/refs/callers latency specifically.
- Call-graph precision from receiver-type resolution (`docs/plans/2026-06-17-scope-type-resolution.md`). Measure: `callers <method>` on a corpus with same-named methods (ServeHTTP, Close, String) and count confirmed vs dynamic vs unresolved; `tests/reference_tracker_test.cpp` pins the resolver cases. Bench evidence: `benchmarks/repo-qa/ANALYSIS-discovery-sweep.md` (callers P=1.0/R=1.0 vs grep P=0.111 on one cell, tool level; rule 9 of `.claude/rules/bench-harness-oracle-independence.md` records the harness fix that produced it).

| Axis | lci | How to check the competitor | Notes |
|---|---|---|---|
| Definition lookup | Text search + nearest indexed symbol (`handlers_search.cpp:141`) | ctags: `readtags`; LSP: `textDocument/definition`; SCIP: `scip snapshot` | lci's `def` can return several same-named hits across languages; LSP resolves one per call site |
| References | Textual occurrences, comment/string masked client-side | LSP `textDocument/references`; cscope `-L1`; GNU global `global -r` | lci refs are lexical; precision on common names is lower than LSP by construction |
| Callers | Resolved call edges grouped by enclosing function, dynamic/unresolved split | LSP `callHierarchy/incomingCalls`; cscope `-L3`; gopls `call hierarchy` | lci is cross-language and index-wide in one call; LSP is per language server |
| Call tree | `build_function_tree`, depth-capped, cycle-marked in `get_context` | LSP outgoing calls recursed by the client; Sourcegraph has no tree view | `/tree` risk fields are stubs (see traps) |
| File outline | `browse_file` with kinds, sort, stats | LSP `textDocument/documentSymbol`; ctags per file | lci adds complexity and ref counts per symbol |
| Symbol listing/filter | `list_symbols` kind/glob/receiver/complexity/params/flags, paginated | ctags + grep on tags file; Sourcegraph symbol search | No competitor offers complexity or parameter-count filters in the same query (unverified, from model knowledge) |
| File finding | `find_files` fuzzy + glob with miss hints | `fd`, `fzf`, editor quick-open | lci scores per Go-era formula (`handlers_find_files.cpp:362-409`) |
| Receiver-aware resolution | Syntactic local type env, no type checker | gopls/clangd/tsserver use the real compiler front end | lci precision is below compiler-backed LSP on generics, inference, overloads; equal on plain method calls |
| Multi-language | 13 grammars, one index | One LSP per language; ctags many languages, definitions only | |
| Persistence | None; re-index on server start | ctags/global/cscope write a database; SCIP indexes are files; LSP caches vary | Cold start cost is lci's per session (`README.md:160-161`) |
| Agent surface | MCP tools with object ids, hints, pagination | Serena MCP wraps LSP; GitHub/Sourcegraph have HTTP APIs | |

Honest gaps versus the field: no compiler-grade resolution (generics, overloads, inference), references are lexical, no rename/refactor support, no disk persistence, no cross-repository navigation, no MCP call-tree tool, and `list_symbols` glob support is equality/basename/suffix plus `*`/`?` rather than full glob syntax.

## Related skills

lci-feature-map (router), lci-search, lci-get-context, lci-parsing-languages, lci-code-insight, lci-side-effects, lci-git-analysis, lci-indexing-pipeline, lci-server-lifecycle, lci-mcp-server, lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation
