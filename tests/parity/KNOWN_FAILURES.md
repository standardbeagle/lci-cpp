# Known Parity Failures

Baseline captured 2026-04-27 against:

- Go reference: `lci version 0.4.1`
- C++ port: `lci 0.1.0` (commit `c648ab1`)

Run: `ctest --test-dir build/debug -L parity -j$(nproc)`

Result: **11 / 55 parity descriptors passing (44 failing)**. All 22 harness
unit tests (`Canonicalize*`, `FieldTier*`, etc.) pass.

These failures are real port regressions, not harness bugs. Do not silence by
adjusting tiers. Fix the C++ implementation until the descriptor passes
unmodified, or document an intentional divergence in `MODULE_MAP.md` first.

## Failure summary by category

| Category | Failing | Total | Dominant cause |
|---|---:|---:|---|
| `mcp.*` | 17 | 17 | C++ MCP tool handlers stubbed: payload literal `"Tool handler will be implemented in a subsequent task"` |
| `cli.*` | 16 | 19 | C++ CLI subcommands return empty output / non-zero exit (e.g. `cli/search/basic`: Go finds 2 results & exits 0; C++ stdout empty, exit 1) |
| `http.*` | 6 | 12 | HTTP response schema drift — score/path/match fields diverge, type mismatches (object vs null) |
| `index.*` | 3 | 3 | Debug-export schema disjoint between Go and C++ (`files`, `file_count`, `symbol_count` fields not aligned) |
| `probes.*` | 2 | 3 | `debug deps` / `debug graph` text format differs |
| **Total** | **44** | **55** | |

## Failing test IDs

### cli (16)
- `parity.cli.config.show`
- `parity.cli.config.validate`
- `parity.cli.debug.info`
- `parity.cli.debug.validate`
- `parity.cli.git.git-analyze`
- `parity.cli.search.basic`
- `parity.cli.search.case-insensitive`
- `parity.cli.search.compact`
- `parity.cli.search.grep`
- `parity.cli.search.json`
- `parity.cli.search.regex`
- `parity.cli.symbols.browse`
- `parity.cli.symbols.inspect`
- `parity.cli.symbols.list`
- `parity.cli.symbols.symbols`
- `parity.cli.symbols.tree`

### http (6)
- `parity.http.browse-file`
- `parity.http.list-symbols`
- `parity.http.ping`
- `parity.http.references`
- `parity.http.search`
- `parity.http.tree`

### mcp (17)
- `parity.mcp.browse_file.basic`
- `parity.mcp.code_insight.basic`
- `parity.mcp.context_manifest.basic`
- `parity.mcp.debug_info.basic`
- `parity.mcp.find_files.basic`
- `parity.mcp.get_context.basic`
- `parity.mcp.git_analysis.basic`
- `parity.mcp.grep.basic`
- `parity.mcp.index_stats.basic`
- `parity.mcp.info.basic`
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

### probes (2)
- `parity.probes.deps`
- `parity.probes.graph`

## Currently passing (11)

- `parity.cli.symbols.def`
- `parity.cli.symbols.refs`
- `parity.cli.version`
- `parity.http.definition`
- `parity.http.fileinfo`
- `parity.http.git-analyze`
- `parity.http.inspect-symbol`
- `parity.http.reindex`
- `parity.http.stats`
- `parity.http.status`
- `parity.probes.export`

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
