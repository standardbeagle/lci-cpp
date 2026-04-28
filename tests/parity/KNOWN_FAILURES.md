# Known Parity Failures

Baseline last updated 2026-04-28 (HTTP symbol-aware endpoints landed; see
`MODULE_MAP.md` "Decisions" section and the iter-3 task `r6IbUABIThml`
for the full root-cause + parity-harness rationale).

- Go reference: `lci version 0.4.1`
- C++ port: `lci 0.1.0`

Run: `ctest --test-dir build/debug -L parity -j$(nproc)`

Result: **24 / 55 parity descriptors passing (31 failing)**. All 32 harness
unit tests (`Canonicalize*`, `FieldTier*`, etc.) pass.

Delta from previous baseline (19 / 55):
- +5 HTTP endpoints (`http.browse-file`, `http.list-symbols`,
  `http.references`, `http.search`, `http.tree`) now pass after wiring
  the tree-sitter UnifiedExtractor into the indexing pipeline,
  serializing FileContentStore writes (latent race), assigning FileIDs
  in producer-thread scan order, and fixing minor schema drift on the
  endpoints' JSON encoders.
- New parity-harness primitive: `tiers.sort_arrays` (descriptor key)
  asks the canonicalizer to sort named arrays before tier comparison so
  Go-side hash-map iteration order does not flake the test bed. Used
  for `http.list-symbols`, `http.references`, `http.search`.

These failures are real port regressions, not harness bugs. Do not silence by
adjusting tiers. Fix the C++ implementation until the descriptor passes
unmodified, or document an intentional divergence in `MODULE_MAP.md` first.

## Failure summary by category

| Category | Failing | Total | Dominant cause |
|---|---:|---:|---|
| `mcp.*` | 16 | 17 | C++ MCP tool handlers stubbed: payload literal `"Tool handler will be implemented in a subsequent task"` |
| `cli.*` | 8 | 19 | After text-mode normalization the residue is real divergences: structural schema drift in `config.show` / `debug.info`, real bugs in `search.case-insensitive` / `search.grep` / `symbols.list`, plus `git.git-analyze` and `search.json` / `symbols.tree` JSON-format drift |
| `http.*` | 3 | 12 | Remaining: `http.definition` (search candidate filter drops the trigram match for short patterns under declaration-only), `http.inspect-symbol` (extra fields), `http.git-analyze` (not yet ported) |
| `index.*` | 3 | 3 | Debug-export schema disjoint between Go and C++ (`files`, `file_count`, `symbol_count` fields not aligned) |
| `probes.*` | 1 | 3 | `probes/deps` text-format and content fundamentally differ (Go: edge stats; C++: file/symbol summary) |
| **Total** | **31** | **55** | |

## Failing test IDs

### cli (8)
- `parity.cli.config.show`
- `parity.cli.debug.info`
- `parity.cli.git.git-analyze`
- `parity.cli.search.case-insensitive`
- `parity.cli.search.grep`
- `parity.cli.search.json`
- `parity.cli.symbols.list`
- `parity.cli.symbols.tree`

(`parity.cli.search.basic` and `parity.cli.search.compact` previously
listed here are now passing thanks to the iter-3 indexer wiring + the
text-mode formatter alignment.)

### http (3)
- `parity.http.definition`
- `parity.http.git-analyze`
- `parity.http.inspect-symbol`

### mcp (16)
- `parity.mcp.browse_file.basic`
- `parity.mcp.code_insight.basic`
- `parity.mcp.context_manifest.basic`
- `parity.mcp.debug_info.basic`
- `parity.mcp.find_files.basic`
- `parity.mcp.get_context.basic`
- `parity.mcp.git_analysis.basic`
- `parity.mcp.grep.basic`
- `parity.mcp.index_stats.basic`
- `parity.mcp.inspect_symbol.basic`
- `parity.mcp.list_symbols.basic`
- `parity.mcp.search.basic`
- `parity.mcp.search_definitions.basic`
- `parity.mcp.semantic_annotations.basic`
- `parity.mcp.side_effects.basic`
- `parity.mcp.tree.basic`

### index (3)
- `parity.index.lci-cpp-repo`
- `parity.index.lci-go-repo`
- `parity.index.synthetic-multilang`

### probes (1)
- `parity.probes.deps`

## Currently passing (24)

- `parity.cli.config.validate`
- `parity.cli.debug.validate`
- `parity.cli.search.basic`
- `parity.cli.search.compact`
- `parity.cli.search.regex`
- `parity.cli.symbols.browse`
- `parity.cli.symbols.def`
- `parity.cli.symbols.inspect`
- `parity.cli.symbols.refs`
- `parity.cli.symbols.symbols`
- `parity.cli.version`
- `parity.http.browse-file`
- `parity.http.fileinfo`
- `parity.http.list-symbols`
- `parity.http.ping`
- `parity.http.references`
- `parity.http.reindex`
- `parity.http.search`
- `parity.http.stats`
- `parity.http.status`
- `parity.http.tree`
- `parity.mcp.info.basic`
- `parity.probes.export`
- `parity.probes.graph`

## Recommended fix order

1. **`cli.search.basic`** — smoke test of the whole CLI path. Empty stdout +
   exit 1 indicates the subcommand pipeline (`run_search` → server → search
   engine) is broken end-to-end; fixing it likely cascades into the other
   `cli.search.*` and `cli.symbols.*` cases.
2. **`mcp.*`** — 13/17 tools are stubs per `MODULE_MAP.md`. Replace stub
   payloads with real handler dispatch.
3. **`http.*`** — schema alignment, ranking parity. Lower priority than CLI
   correctness.
4. **`index.*`** — debug-export schema is intentionally disjoint per
   `MODULE_MAP.md`; decide whether to align or move fields to the `ignore`
   tier.
5. **`probes.deps` / `probes.graph`** — text-format probes; cosmetic but the
   format drift signals deeper graph-output divergence.

Performance comparison (`scripts/benchmark-compare.sh`) is meaningless until
correctness parity is restored — speed of an empty answer is not a metric.
