# Known Parity Failures

Baseline last updated 2026-05-16.

- Go reference: `lci version 0.4.1`
- C++ port: `lci 0.1.0`

Run: `ctest --test-dir build -L parity --output-on-failure`

Result: **104 / 109 parity descriptors passing**, **5 regressions** open under FIX-D.1.{A,B,C}; FIX-D.1.{D,E} resolved (see [Active regressions](#active-regressions-fix-d1-fallout) below).

## Active regressions (FIX-D.1 fallout)

FIX-D.1 (Dart `FZJ6Iip4we3U`, iter-9) deleted 8 parity-compat stubs from `src/cli/mcp.cpp` that were shadowing real handlers via reverse-iteration dispatch. tools/list now correctly emits 14 tools (was 22); real handlers in `handlers_{core,explore,index,analysis,context}.cpp` now own dispatch. Six descriptors regressed because real-handler output diverges from Go shape — each tracked under its own fix subtask:

| Descriptor | Subtask | Class | Issue |
|---|---|---|---|
| `mcp/find_files/basic` | `w6ZrZX8fAA6h` (FIX-D.1.A) | real-handler bug | Empty results for `pattern='*.go'` (no fuzzy/glob fallback) |
| `mcp/side_effects/basic` | `TwJuY55J9KM1` (FIX-D.1.B) | real-handler gap | `total_count=0` vs Go=4 — analyzer never populated |
| `mcp/code_insight/{basic,mode-*}` (×6) | `mz2z1Xn0gQEm` (FIX-D.1.C) | real-handler shape | Emits JSON, Go emits LCF text format |
| ~~`mcp/inspect_symbol/basic`~~ | `9b6XxaeB08VL` (FIX-D.1.D) ✓ | (b) enrichment | ~~Extra `callees, callers, incoming_refs, parameter_count, signature`~~ — RESOLVED iter-11: masked runner-wide in parity_runner.cpp normalize_mcp_inner_text inner_opts.ignore_paths (symbols[].{callees,callers,incoming_refs,parameter_count}; signature already masked by FIX-D.1.E iter-10). |
| ~~`mcp/list_symbols/basic`, `mcp/browse_file/basic`~~ | `HVbfjGGBtAtU` (FIX-D.1.E) ✓ | (b) enrichment | ~~Extra `symbols[].signature` via tree-sitter~~ — RESOLVED iter-10: masked runner-wide in parity_runner.cpp normalize_mcp_inner_text inner_opts.ignore_paths (symbols[].signature). Both descriptors 10/10 stable. |

Two of the eight target descriptors (`mcp/debug_info/basic`, `mcp/git_analysis/basic`) pass post-removal — real handler shape already matches Go.

> **Important:** "Passing" does not mean "byte-equivalent output." Most
> non-trivial descriptors carry an `ignore` tier that masks at least one
> field, and several carry sort/collapse normalizers. Masks fall in four
> categories (see _Mask Categories_ below). Every mask should have a
> `_rationale` field on the descriptor or a linked Dart task.

## Status by category

| Category | Failing | Total | Status |
|---|---:|---:|---|
| `cli.*` | 0 | 28 | Green |
| `http.*` | 0 | 13 | Green |
| `index.*` | 0 | 3 | Green |
| `mcp.*` | 3 | 17 | Regressed (FIX-D.1 fallout — see above; iter-10 closed FIX-D.1.E: list_symbols + browse_file; iter-11 closed FIX-D.1.D: inspect_symbol) |
| `probes.*` | 0 | 3 | Green |
| `cli.* / http.* / mcp.*` (integration) | 0 | 45 | Green |
| **Total parity (`-L parity`)** | **3** | **109** | **3 regressions** |

(Counts via `ctest -L parity` cover the side-by-side parity_runner
descriptors plus the integration goldens that also carry the `parity`
label.)

## Mask categories

Every tier mask should fall in one of these four buckets. The
descriptor's `_rationale` field should make the bucket explicit; if
missing, the descriptor is in violation of the parity contract.

1. **(a) Non-determinism.** Timestamps, pids, request_ids, uptime_ms,
   elapsed_ms, schema_version. Always ignored. Centralization filed as
   `LaID7inbumds` (DRY parity descriptor ignore lists).

2. **(b) Intentional C++ enrichment.** C++ emits more useful data than
   Go in the same field. Documented per-descriptor `_rationale`. Locked
   by C++-side unit/integration tests. Examples:
   - `http/tree.tree.root.file_path` — C++ resolves the defining file
     (locked by `ServerTest.TreeRootFilePathIsRelativeToProjectRoot`).
   - `http/status.indexing_progress` — C++ emits live progress object
     (locked by `ServerTest.IndexingProgressFieldTypesAndRanges`).
   - `cli/symbols/inspect{,-json}.signature` — C++ extracts function
     signature via tree-sitter (locked by integration golden).
   - `http/search.results[].context.block_name` — C++ always emits the
     field with stable shape (locked by
     `ServerTest.SearchResultContextBlockNameContractEmptyOrSymbolName`).

3. **(c) Workspace-state-dependent.** Counts and arrays that change per
   the user's WIP/git state at test time. Examples:
   - `http/git-analyze.report.summary` and `report.{naming_issues,
     duplicates, metrics_issues}` — vary with the lci-cpp repo's WIP
     when the test runs. Locked C++-side by `GitReportToJson.*` unit
     tests in tests/git_test.cpp.
   - `cli/status.{text,json}` runtime metrics (goroutines, memory,
     build_duration_ms, file_count) are runtime/host-dependent.

4. **(d) Ranking divergence.** Go and C++ produce the same multiset of
   results in different orders. Masked via `sort_lines: true` (text) or
   `sort_arrays` (JSON). Convergence tracked in `A38Q2RR8ZcyL`.

## Intentional divergences (will stay masked)

1. `index.*` compares `debug export` at exit-code level only because
   Go and C++ export fundamentally different payloads.
2. `cli.search.case-insensitive` is intentionally stabilized as a
   lighter probe because the Go reference is flaky on the synthetic
   multi-language corpus.
3. `http.git-analyze` ignores `report.summary.top_recommendation`
   because the recommendation copy diverges even when summary counts
   align.
4. `cli.status.{text,json}` runtime metrics. Tracked in `VFIWNmWKXNgn`
   to replace fake-Go-shaped zeros with accurate C++ counters once the
   runtime-metric implementation lands.
5. `mcp.get_context.basic` ignores `result.content[].text` for two
   contexts[] sub-paths: (b) C++ extracts a tree-sitter function
   signature where Go leaves it empty (enrichment, same class as
   `cli/symbols/inspect.signature`); and C++ omits `contexts[].purity`
   because the C++ MCP runtime wires no side-effect propagator into the
   get_context handler — Go does. The id-only contract (resolution,
   `errors[]` for unresolvable ids, `mode=` fail-fast, id/name
   mutual-exclusion) is locked C++-side by `HandlersFixture.GetContext*`.
   `mode=` context lookup (Go's `handleGetObjectContextWithMode` /
   `ContextLookupEngine`) is not ported; C++ returns a fail-fast error
   rather than a silent stub. Wiring purity tracked below; porting the
   mode engine and `symbol`+`path` auto-search tracked below.
6. `mcp.search.basic` ignores `result.content[].text` for two
   `results[]` sub-paths: (b) C++ enrichment — `handle_search` emits
   `results[].context_lines` (the matched source line); Go's MCP
   search omits the field; and (d) ranking — Go and C++ compute their
   own match scores (Go ~855.5/125.5; C++ ~121.5 from a different
   ranker) and item orders for the same multiset of matches. Tracked
   under `A38Q2RR8ZcyL`. The substring-on-symbol-names parity-compat
   stub in `src/cli/mcp.cpp` (hardcoded `score:855.5`, `column:1`, no
   regex/flags/output/semantic/ranking) was removed; the real
   `handle_search` now runs the full trigram-backed `SearchEngine` and
   attributes each match to its enclosing symbol via
   `ref_tracker.get_symbol_at_line`. Structural body parity (item
   count, file set, match strings, symbol attribution) is locked
   C++-side by `HandlersFixture.Search*` unit tests.

## Open follow-ups (filed as Dart tasks on Personal/lci)

| Task ID | Title |
|---|---|
| `8vj4E26sMucH` | Parity audit: enumerate and triage every tier mask |
| `VFIWNmWKXNgn` | cli/status: emit accurate C++ runtime metrics |
| `AegvABjs4MF0` | Real regex engine for lci search |
| `A38Q2RR8ZcyL` | Ranking parity: converge result ordering with Go |
| `MWm23vqgX9O6` | Wire up ignored CLI flags |
| `sL0AJDf2hjIh` | Audit handlers_explore / _index / _analysis MCP |
| `3PMRNxdzbr96` | Add CI benchmark gates vs Go |
| `xDvnzlsiPKtO` | Enable pocketbase real-project tests |
| `ClivrpWd5RIf` | Add TypeScript real-project corpora |
| `x6LLkThO0UAA` | Diagnose flaky parity failures under -j |
| `h76USlFW6JLd` | Cleanup orphan lci server processes |
| `LaID7inbumds` | DRY parity descriptor ignore lists |
| `BNXsh3tUpMSW` | Socket path namespacing dual-candidate sunset |
| `Qg74ZYD95WP6` | Multi-word search semantics |
| `qkbC8BBuW14H` | Rename misleading config.search.enable_fuzzy |
| `236EnLNu6xMy` | MCP tools/call unknown-tool: spec vs parity |
| `2OayJlW5e5FJ` | cli/debug/info: align C++ debug output schema |
| _(to file)_ | MCP get_context: wire side-effect propagator so `contexts[].purity` is populated (currently omitted, diverges from Go) |
| _(to file)_ | MCP get_context: port `mode=` context lookup (`handleGetObjectContextWithMode` / `ContextLookupEngine`) — C++ currently fails fast |
| _(to file)_ | MCP get_context: port `symbol`+`path` auto-search (Go's `extractAutoSearchParams` / `autoSearchAndReturnContext`) |
| `nvMSktz7YYIZ` | lci-cpp: add re2 dep, replace `std::regex` in hot paths |
| `bm09pW3iU1co` | lci-cpp: add libstemmer (Snowball), replace hand-port Porter2 |
| `bATJrARRwHAy` | lci-cpp: add nlohmann-json-schema-validator, replace hand-rolled MCP validator |
| `dVjdFhemWPDA` | lci-cpp: add rapidfuzz-cpp, replace hand-rolled fuzzy matcher |
| `2P0xeGBuU0CN` | (Done) lci-cpp: add efsw, replace raw inotify watcher (cross-platform) |
| `Fs19gT5u1aVu` | (Done) Default-exclude contract test |
| `c8DhcMRqeN7v` | (Done) Fix glob: single * must not cross / |
| `rWZYDAQgQsTt` | (Done) Path-matching audit |
| `p1Iqm9Y7Olcw` | (Done) git-analyze serialization bug fixes |

## CI gating gap

The parity suite is in CI but **this document is not**. A follow-up
should diff `_rationale` coverage and rationale↔task linkage against
actual descriptor files and fail when they disagree, so the doc stays
accurate. Tracked as part of `JHXocNGammb8`.

## Next strengthening work

1. Promote stable parity surfaces into C++-owned goldens where that
   gives clearer ownership than side-by-side Go comparison.
2. Replace weakened parity probes with stronger happy-path coverage
   where the product surface is ready.
3. Keep `MODULE_MAP.md` aligned with the current parity decisions so
   this file can remain a short status page instead of a historical
   backlog dump.
