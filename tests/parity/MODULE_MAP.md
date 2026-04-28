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

Surveyed 2026-04-27: neither binary exposes any of the seven as a `debug` subcommand or
as a top-level subcommand.  Both binaries offer exactly five `debug` subcommands:
`info`, `validate`, `deps`, `export`, `graph`.

- [x] `lci debug trigrams <input>` — go: NO, cpp: NO — neither implements; needs both
- [x] `lci debug score <query> <doc-id>` — go: NO, cpp: NO — neither implements; needs both
- [x] `lci debug walk <dir>` — go: NO, cpp: NO — neither implements; needs both
- [x] `lci debug link <file>` — go: NO, cpp: NO — neither implements; needs both
- [x] `lci debug annotate <symbol>` — go: NO, cpp: NO — neither implements; needs both
- [x] `lci debug regex-analyze <pattern>` — go: NO, cpp: NO — neither implements; needs both
- [x] `lci debug encode-id <symbol>` / `decode-id <id>` — go: NO, cpp: NO — neither implements; needs both

Probe coverage for subcommands present on BOTH sides:

| subcommand | go | cpp | descriptor | parse | notes |
|---|---|---|---|---|---|
| `debug info` | YES | YES | `cli/debug/info.parity.json` | text | structural divergence (phase 2) |
| `debug validate` | YES | YES | `cli/debug/validate.parity.json` | text | structural divergence (phase 2) |
| `debug deps` | YES | YES | `probes/deps.parity.json` | text | Go=edge-count table; C++=file/symbol/index stats — will diff |
| `debug export` | YES | YES | `probes/export.parity.json` | exit-only | writes to file; stdout is status text; JSON schemas are disjoint |
| `debug graph` | YES | YES | `probes/graph.parity.json` | exit-only | writes to file; stdout is status text; DOT structure differs |

Also backlog (exists, needs dedicated descriptors):

- [ ] `lci debug export --json` per corpus — full symbol/ref graph parity (currently schema-disjoint; see Known Divergences)

---

## Decisions

### Text-mode normalization (Phase 2 — 2026-04-28)

**Chosen: Option A — descriptor-driven text normalizers.**

Two competing options were on the table for plain-text descriptors that
diff only on cosmetic noise:

- **A.** Add a normalizer pipeline to `canonicalize_text` (timing scrub,
  corpus path rewrite, line strip, emoji prefix strip, regex replace),
  with per-descriptor knobs in a `text_normalize` block.
- **B.** Switch text descriptors to JSON mode where both binaries support
  `--json` on the relevant subcommand.

Option A was chosen because:
- It is least invasive: pure additions to the diff engine and zero
  changes to either binary.
- B is blocked: today the C++ `cli/search` family does not emit a
  parseable JSON document on stdout (Go prepends a `DEBUG:` line that
  itself defeats JSON parsing — see "CLI JSON output" below). Migrating
  to JSON would require fixing both binaries first.
- A surfaces real divergences with sharper signal: with cosmetic noise
  filtered, the remaining 5/10 affected descriptors that still fail are
  visibly real bugs (case-insensitive, grep body, symbols/list, deps
  schema, debug/info schema, config/show pattern count) — not harness
  artefacts.

The normalizer pipeline applies, per text descriptor:
- (default-on)  trailing-whitespace trim
- (default-on)  timing scrub: `\d+(\.\d+)?ms` → `<MS>`
- (default-on)  corpus-prefix rewrite: `<abs corpus>` → `${CORPUS}`
- (opt-in)      `strip_lines: [substring, ...]` — drop matching lines
- (opt-in)      `strip_emoji_prefix: true` — drop leading emoji + WS
- (opt-in)      `replace: [{pattern, with}, ...]` — per-line ECMAScript regex

Schema lives in `tests/parity/runner/descriptor.{h,cpp}`. Implementation
lives in `tests/parity/diff_engine/canonicalize.{h,cpp}`. Unit-test
coverage in `tests/parity/unit_tests/canonicalize_test.cpp`.

---

## Known divergences (surfaced by harness)

Cumulative list of confirmed behavioral differences between Go and C++.
New findings should be appended here with the phase tag.

### CLI text output (Phase 2)

The first six items below are cosmetic and are now neutralized by the
text-mode normalizer pipeline (see "Decisions"). Remaining structural
divergences are flagged with **REAL** and persist after normalization.

- Go emits `DEBUG: verbose=...` line on stdout in non-grep modes; C++ does not. _(neutralized via `strip_lines: ["DEBUG:"]`.)_
- Go emits `=== Direct Matches ===` section header in basic search; C++ does not. _(neutralized via `strip_lines`.)_
- C++ result paths are absolute; Go result paths are relative to the repo root. _(neutralized via corpus-prefix rewrite + `${CORPUS}/` strip in `cli/search/basic`.)_
- Mode label strings differ: Go uses `"integrated mode"`, C++ uses `"standard mode"`. _(neutralized via regex replace to `(MODE)`.)_
- Go uses emoji prefixes (`✅`, `📍`, `📊`, `⚠️`, `✓`); C++ does not. _(neutralized via `strip_emoji_prefix: true`.)_
- Go shows verbose preamble lines in `debug validate` (`Building index...`, `Linking symbols...`, `Incremental Mode: false`). _(neutralized via `strip_lines`.)_
- **REAL — `cli/search/case-insensitive`:** Go returns 0 results, C++ returns 4 results on `multi-lang/add -i`. Go's case-insensitive search is likely broken on small synthetic corpora.
- **REAL — `cli/search/grep`:** C++ grep output body is empty (timing line printed, no `file:line:col` records produced). Go emits all matches concatenated on a single line without trailing newline.
- **REAL — `cli/symbols/list`:** C++ prints `Files: 4` (a count summary) instead of the per-file listing Go produces.
- **REAL — `cli/config/show`:** Go reports 124 exclude patterns and emits `Performance Settings` (with `Max goroutines:`) and `Search Settings` sections; C++ reports 35 patterns and omits both sections.
- **REAL — `cli/debug/info`:** Output shape is fundamentally different. Go emits a `Symbol Linking System Debug Info` summary with file/symbol/import/reference counts and per-extractor stats; C++ emits a `Server Status` block with `Ready`, `Indexing Active`, `Memory RSS`, `Uptime`, etc. No common fields.
- **REAL — `probes/deps`:** Go emits a dependency-edge-count table (Total Dependency Edges, Maximum Dependency Depth, etc.); C++ emits a file/symbol/index-size summary. No common fields.

### CLI JSON output (Phase 2)

- Go writes a `DEBUG:` line to stdout before the JSON payload, breaking JSON parsing for callers that don't strip it.
- `git-analyze --json --scope wip` exit code: Go=0 (success with empty result), C++=1 (error with no output).

### CLI config/debug (Phase 2)

- `config show`: Go reports 124 exclude patterns; C++ reports 35. Go also emits `Performance Settings` and `Search Settings` sections that C++ omits entirely.
- `debug info` and `debug validate`: structural divergence; field names and nesting do not align.
- `debug deps`: Go emits a dependency-edge-count table (Total Dependency Edges, Maximum Dependency Depth, etc.); C++ emits a file/symbol/index-size summary. No common fields. (`probes/deps` fails text comparison — confirmed divergence.)
- `debug export`: writes JSON to file; Go schema has top-level keys `summary`, `files`, `symbols`, `refs`, `extractors`, `resolvers`, `dependencies`; C++ schema has `avg_search_time_ms`, `file_count`, `ready`, `uptime_seconds`, etc. Completely disjoint. (`probes/export` is exit-only; JSON content comparison is not possible until C++ adopts Go schema.)
- `debug graph`: writes DOT to file; Go produces a node-per-file graph with `rankdir=TB`; C++ produces a summary placeholder node with `rankdir=LR` and no file-level nodes. (`probes/graph` is exit-only.)

### MCP (Phase 3)

- Wire protocol: Go uses newline-delimited JSON (ndjson); C++ uses `Content-Length` framing. The parity runner adapts per binary.
- 13 of 17 MCP tools return `not_implemented` stubs in C++; only skeleton handler code exists.
- Tool name mismatch: C++ exposes `context_manifest`, `search_definitions`, `grep`, and `tree`; Go exposes `context` (not `context_manifest`) and does not expose the other three names.

### Index/data (Phase 4)

- `debug export --json` schemas are completely disjoint: Go exports a symbol graph (`files`, `symbols`, `refs`, `extractors`, `resolvers`, `dependencies`, `summary`); C++ exports runtime stats (`file_count`, `ready`, `uptime_seconds`, `avg_search_time_ms`, …). No common top-level fields exist.
- `lci-go-repo` index build timed out at 60 s during harness runs.
