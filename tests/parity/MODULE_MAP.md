# Module Map — Go LCI ↔ C++ LCI

Lightweight orientation document. Module comparison is opportunistic; only
algorithms that must match get a parity probe. Use this file to track which
packages have probe coverage and which are still backlog.

---

## Mapping table

Mapping kinds:
- `mapped`  — 1↔1 correspondence, direct name match
- `merged`  — Go N pkgs → C++ 1 dir (consolidation)
- `split`   — Go 1 pkg → C++ N dirs (expansion)
- `removed` — intentionally dropped in C++ port
- `added`   — present in C++ only, no Go equivalent

Coverage signals:
- `cli.search.*` covers search package
- `cli.symbols.*` covers symbol commands
- `mcp.*` covers mcp package
- `cli.git.*` covers git package
- `index.*` covers indexing/parser/symbollinker indirectly

| Go pkg | C++ home | Mapping | Has algo probe? | Notes |
|---|---|---|---|---|
| `internal/alloc` | `src/alloc` + `include/lci/alloc/` | mapped | No | Headers exist; src dir is empty — implementations may be header-only |
| `internal/analysis` | `src/analysis` | mapped | No | C++ expands to more sub-analyzers (health, layer, module, side-effect, feature) |
| `internal/cache` | _(merged into analysis/search)_ | merged | No | Go's MetricsCache absorbed into C++ inline LRU caches in semantic_scorer and regex_analyzer |
| `internal/config` | `src/config` | mapped | No | Go has 124 exclude patterns; C++ has 35 — known divergence |
| `internal/core` | `src/core` | mapped | indirectly via `index.*` | Go core carries assembly_search and ast_store; C++ expands into file_service, symbol_store, graph_propagator, etc. |
| `internal/debug` | `src/cli/debug.cpp` | merged | No | Go debug is a standalone package; C++ debug commands live in CLI layer |
| `internal/display` | `src/cli/` | merged | No | Go TreeFormatter merged into C++ CLI output; no standalone display dir |
| `internal/encoding` | `include/lci/idcodec.h` | merged | No | Folded into idcodec; base-63 alphabet identical by design |
| `internal/errors` | `src/error.cpp` + `include/lci/error.h` | mapped | No | C++ uses typed enum `ErrorType`; Go uses wrapped fmt.Errorf |
| `internal/git` | `src/git` | mapped | indirectly via `cli.git.*` | C++ adds `pattern_detector.cpp`; Go has `frequency_cache.go` |
| `internal/idcodec` | `include/lci/idcodec.h` | mapped | No | C++ header-only; encode-id/decode-id probe is backlog |
| `internal/indexing` | `src/indexing` | mapped | indirectly via `index.*` | C++ schema diverges — debug export fields are disjoint |
| `internal/interfaces` | _(removed)_ | removed | No | Go `Indexer` interface not needed in C++; compile-time polymorphism used instead |
| `internal/mcp` | `src/mcp` | mapped | indirectly via `mcp.*` | 13/17 tools are stubs in C++; wire protocol differs (ndjson vs Content-Length) |
| `internal/metrics` | `include/lci/analysis/metrics_calculator.h` | merged | No | Go CodebaseStats merged into C++ analysis layer |
| `internal/parser` | `src/parser` | mapped | indirectly via `index.*` | C++ uses unified_extractor pattern; community parsers differ |
| `internal/regex_analyzer` | `src/regex_analyzer` | mapped | No | Engine architecture similar; regex-analyze probe is backlog |
| `internal/search` | `src/search` | mapped | indirectly via `cli.search.*` | C++ adds requirements_analyzer; Go has context_extractor_zero_alloc |
| `internal/searchtypes` | `include/lci/search/search_options.h` | merged | No | Go GrepResult/SearchType merged into C++ search_options types |
| `internal/security` | _(removed)_ | removed | No | Go FileValidator dropped; C++ relies on OS limits and binary_detector |
| `internal/semantic` | `src/semantic` | mapped | No | C++ adds vocabulary_analyzer; Go has abbreviation bidirectional matchers |
| `internal/server` | `src/server` | mapped | No | Both carry server + client; Go adds build_id fingerprinting |
| `internal/symbollinker` | `src/symbollinker` | mapped | indirectly via `index.*` | Both support Go/JS/C#/Python/PHP; link probe is backlog |
| `internal/test` | _(removed)_ | removed | No | Go profiling harness; no C++ equivalent |
| `internal/testing` | `tests/helpers/` | mapped | No | C++ test helpers in tests/helpers/; different patterns |
| `internal/tools` | _(removed)_ | removed | No | Go EntityIDGenerator merged into core ID generation logic |
| `internal/types` | `include/lci/types.h` + `include/lci/symbol.h` etc. | split | No | C++ splits types across multiple headers (types.h, symbol.h, graph_types.h, side_effects.h, context_manifest.h) |
| `internal/version` | `include/lci/version.h` (generated) | mapped | No | Go=0.4.1, C++=0.1.0; not expected to match |
| _(none)_ | `src/cli` | added | No | C++ has dedicated CLI layer; Go uses cobra commands at top level |
| _(none)_ | `src/string_pool.cpp` + `include/lci/string_pool.h` | added | No | C++ string interning layer has no Go equivalent |

---

## Backlog: probes to add

These seven algorithmic probes are defined in the design spec
(`docs/superpowers/specs/2026-04-27-lci-cpp-vs-go-parity-verification-design.md`, §Algorithmic targets).
Each probe = one `mode: cli` descriptor driving a `lci debug …` sub-command.

- [ ] `lci debug trigrams <input>` — sorted trigram list; same input must produce same set or search recall diverges
- [ ] `lci debug score <query> <doc-id>` — BM25/IDF/weight breakdown; required for ranking parity
- [ ] `lci debug walk <dir>` — included file paths; validates include/exclude rule alignment
- [ ] `lci debug link <file>` — name→def edges; validates symbollinker output
- [ ] `lci debug annotate <symbol>` — semantic annotation labels; validates `mcp.semantic_annotations` parity
- [ ] `lci debug regex-analyze <pattern>` — literal/prefix/anchor flags; validates search planner pre-filter
- [ ] `lci debug encode-id <symbol>` / `decode-id <id>` — cross-binary ID stability (if exposed)

Also backlog (exists, needs dedicated descriptors):

- [ ] `lci debug export --json` per corpus — full symbol/ref graph parity (currently schema-disjoint; see Known Divergences)

---

## Known divergences (surfaced by harness)

Cumulative list of confirmed behavioral differences between Go and C++.
New findings should be appended here with the phase tag.

### CLI text output (Phase 2)

- Go emits `DEBUG: verbose=...` line on stdout in non-grep modes; C++ does not.
- Go emits `=== Direct Matches ===` section header in basic search; C++ does not.
- `-i` case-insensitive: Go returns 0 results, C++ returns 4 results on `multi-lang/add` query — Go behavior is likely a bug.
- C++ result paths are absolute; Go result paths are relative to the repo root.
- Mode label strings differ: Go uses `"integrated mode"`, C++ uses `"standard mode"`.
- C++ grep output: body is empty — no `file:line:col` records produced.

### CLI JSON output (Phase 2)

- Go writes a `DEBUG:` line to stdout before the JSON payload, breaking JSON parsing for callers that don't strip it.
- `git-analyze --json --scope wip` exit code: Go=0 (success with empty result), C++=1 (error with no output).

### CLI config/debug (Phase 2)

- `config show`: Go reports 124 exclude patterns; C++ reports 35. Go also emits `Performance Settings` and `Search Settings` sections that C++ omits entirely.
- `debug info` and `debug validate`: structural divergence; field names and nesting do not align.

### MCP (Phase 3)

- Wire protocol: Go uses newline-delimited JSON (ndjson); C++ uses `Content-Length` framing. The parity runner adapts per binary.
- 13 of 17 MCP tools return `not_implemented` stubs in C++; only skeleton handler code exists.
- Tool name mismatch: C++ exposes `context_manifest`, `search_definitions`, `grep`, and `tree`; Go exposes `context` (not `context_manifest`) and does not expose the other three names.

### Index/data (Phase 4)

- `debug export --json` schemas are completely disjoint: Go exports a symbol graph (`files`, `symbols`, `refs`, `extractors`, `resolvers`, `dependencies`, `summary`); C++ exports runtime stats (`file_count`, `ready`, `uptime_seconds`, `avg_search_time_ms`, …). No common top-level fields exist.
- `lci-go-repo` index build timed out at 60 s during harness runs.
