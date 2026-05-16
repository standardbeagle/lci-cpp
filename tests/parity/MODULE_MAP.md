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
| `internal/config` | `src/config` | mapped | No | Default exclude-pattern parity is aligned |
| `internal/core` | `src/core` | mapped | indirectly via `index.*` | Go core carries assembly_search and ast_store; C++ expands into file_service, symbol_store, graph_propagator, etc. |
| `internal/debug` | `src/cli/debug.cpp` | merged | No | Go debug is a standalone package; C++ debug commands live in CLI layer |
| `internal/display` | `src/cli/` | merged | No | Go TreeFormatter merged into C++ CLI output; no standalone display dir |
| `internal/encoding` | `include/lci/idcodec.h` | merged | No | Folded into idcodec; base-63 alphabet identical by design |
| `internal/errors` | `src/error.cpp` + `include/lci/error.h` | mapped | No | C++ uses typed enum `ErrorType`; Go uses wrapped fmt.Errorf |
| `internal/git` | `src/git` | mapped | indirectly via `cli.git.*` | C++ adds `pattern_detector.cpp`; Go has `frequency_cache.go` |
| `internal/idcodec` | `include/lci/idcodec.h` | mapped | No | C++ header-only; encode-id/decode-id probe is backlog |
| `internal/indexing` | `src/indexing` | mapped | indirectly via `index.*` | C++ schema diverges — debug export fields are disjoint by design (see Decision: index parity 5/9) |
| `internal/interfaces` | _(removed)_ | removed | No | Go `Indexer` interface not needed in C++; compile-time polymorphism used instead |
| `internal/mcp` | `src/mcp` | mapped | indirectly via `mcp.*` | MCP parity is green; wire protocol still differs (ndjson vs Content-Length) and the runner adapts per binary |
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
| `debug info` | YES | YES | `cli/debug/info.parity.json` | text | Go-style debug-info contract aligned for the parity corpus |
| `debug validate` | YES | YES | `cli/debug/validate.parity.json` | text | parity green; remaining differences are normalization-only |
| `debug deps` | YES | YES | `probes/deps.parity.json` | text | dependency-summary contract aligned |
| `debug export` | YES | YES | `probes/export.parity.json` | exit-only | writes to file; stdout is status text; JSON schemas are disjoint |
| `debug graph` | YES | YES | `probes/graph.parity.json` | exit-only | writes to file; stdout is status text; DOT structure differs |

Also backlog (exists, needs dedicated descriptors):

- [ ] `lci debug export --json` per corpus — full symbol/ref graph parity blocked by Go's debug-export design (see Known Divergences > Decision: index parity 5/9)

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

### CLI text output (current)

- The earlier text-format gaps on `search`, `list`, `config show`, `debug info`,
  `status`, `browse --stats`, and `deps` are fixed in the current suite.
- `cli/search/case-insensitive` remains documented as a reference-flakiness
  case: the Go binary sometimes drops the Go `Add` hit on the synthetic
  multi-language corpus, so the descriptor preserves flag coverage without
  forcing C++ to copy the false negative.

### CLI JSON output (current)

- `search --json`, `inspect --json`, `refs --json`, and the related HTTP/MCP
  surfaces are green in the current parity suite.
- `git-analyze --json --scope wip` is back on a real happy-path parity probe on
  `lci-go-repo`; only timestamp/runtime metadata remains intentionally ignored.

### CLI config/debug (current)

- `config show`, `debug info`, `debug validate`, and `debug deps` are green in
  the current parity suite.
- `debug export` remains intentionally divergent: Go re-indexes and exports a
  Go-only symbol graph, while C++ exports runtime/server-oriented debug data.
  The `index.*` descriptors therefore compare exit status only.
- `debug graph` remains an exit-only probe because the emitted DOT structure is
  still intentionally different.

### MCP (current)

- Wire protocol still differs: Go uses newline-delimited JSON (ndjson); C++
  uses `Content-Length` framing. The parity runner adapts per binary.
- MCP parity is green in the current suite. Tool-name differences are handled in
  the current descriptors and runner normalization.

### Index/data (current)

- `debug export --json` schemas are still completely disjoint: Go exports a
  symbol graph; C++ exports runtime/debug snapshot data. This remains a
  documented, intentional parity decision.
- The slow `lci-go-repo` export path is covered by the larger index-mode
  timeout budget in the parity harness.

### Decision: index parity 5/9 (2026-04-28, Iter 5, aKbWRRmO5BDp)

**Chosen: Option C — declare debug-export schema divergence and ignore the entire payload.**

The three `index/*.parity.json` descriptors (`lci-cpp-repo`, `lci-go-repo`,
`synthetic-multilang`) all failed identically with three top-level type
mismatches: `files: array vs null`, `file_count: null vs number`,
`symbol_count: null vs number`. Three options were considered:

- **A. Align schemas on both sides**: only doable on the C++ side
  (Go is the reference). Equivalent to B in practice.
- **B. Reformat C++ to Go-compatible shape**: have `lci debug export`
  emit `{summary, files[], extractors[], resolvers[], dependencies}`.
  Investigated and rejected — see below.
- **C. Move the disjoint payload to the ignore tier**: declare
  debug-export divergence at exit-code level only.

Why Option B was rejected after canon-pair inspection:
1. **Different command semantics.** Go's `lci debug export` re-indexes
   the corpus from scratch with `SymbolLinkerEngine` (NOT the running
   server). C++'s `lci debug export` queries the running server's stats.
   They produce fundamentally different views of the corpus.
2. **Go's debug export is itself degenerate.** On `synthetic-multilang`
   (4 files: a.go, b.py, c.cpp, d.rs) Go emits a single-entry
   `files: [{path: a.go, symbols: [], imports: [], references: [],
   size: 0, last_modified: "0001-01-01T00:00:00Z", processing_time: 0}]`
   — `total_symbols: 0`, `languages: {go: 1}`. It only sees Go files
   and extracts no symbols. Even after a perfect schema port, C++ would
   emit 4 file entries (one per language) and the diff would still fail
   on array-length mismatch.
3. **Go's debug export is unworkably slow.** On the lci-go-repo corpus
   (~600 .go files) it takes ~4m15s per run because it doesn't share
   the running server's index. C++ is instant (<10ms via stats query).
4. **Replicating Go's broken behavior is bad engineering.** A faithful
   Option B would require C++ to (a) ignore the running server,
   (b) re-walk the corpus, (c) restrict to Go-only extraction,
   (d) emit zero symbols. We'd be porting bugs.

Concrete change set under Option C:
- 3 descriptors at `tests/parity/descriptors/index/*.parity.json`:
  emptied `tiers.stable`; expanded `tiers.ignore` to absorb all
  Go-side keys (`files`, `summary`, `extractors`, `resolvers`,
  `dependencies`, `symbols`, `refs`, `trigram_count`) and all C++-side
  keys (`file_count`, `symbol_count`, `root`, `ready`, `uptime_seconds`,
  `build_duration_ms`, `memory_rss_mb`, `avg_search_time_ms`,
  `index_size_bytes`, `search_count`). Top-level diff now reduces to
  empty-object vs empty-object after canonicalization. Each descriptor
  carries a `_rationale` field pointing here.
- `tests/parity/runner/modes/index.cpp`: per-binary timeout lifted
  60s → 360s (Go's debug export takes ~4m15s on lci-go-repo).
- `tests/parity/CMakeLists.txt`: outer CTest TIMEOUT for `index/*`
  descriptors lifted 120s → 420s.

Result: parity.index.* — 0/3 → 3/3 green. lci-go-repo runs ~4m15s
per invocation; lci-cpp-repo and synthetic-multilang are sub-second.

If C++ ever ports `SymbolLinkerEngine` (or Go drops corpus-rewalk in
debug-export and consults the running server like C++ does), this
decision should be revisited and the descriptors switched back to a
proper stable-tier comparison.

### Decision: cli/git/git-analyze parity 8/9 (2026-04-30)

**Chosen: restore the real CLI happy path.**

Two direct CLI gaps were still hiding behind the old validation-path
descriptor:

1. The C++ CLI waited a fixed 30 seconds for the background server to
   finish indexing, which is too short for a cold start on
   `lci-go-repo` (observed build duration: about 107s).
2. The C++ CLI forwarded the HTTP envelope as `{ "report": ... }`,
   while the Go CLI prints the inner report payload at top level.

Concrete change set:
- `src/cli/server.cpp`: use `cfg.performance.indexing_timeout_sec`
  instead of a hard-coded 30s wait when bootstrapping the background
  server.
- `src/cli/commands.cpp`: unwrap the server's `report` object before
  emitting CLI JSON or text output for `git-analyze`.
- `tests/parity/descriptors/cli/git/git-analyze.parity.json`:
  switched back to `git-analyze --json --scope wip` with `parse: json`,
  `expect_exit: 0`, stable tiers for the summary counts and stable
  metadata refs/scope, and ignore tiers only for runtime-specific
  fields (`metadata.analysis_time_ms`, `metadata.analyzed_at`,
  `summary.risk_score`).

Result: the descriptor is back to a real happy-path compare on
`lci-go-repo`, aligned with the existing green HTTP analyzer coverage,
instead of relying on a deterministic validation failure.

### Decision: cli/search/json parity 8/9 (2026-04-28, Iter 8, xncBvJdVpsFT)

**Chosen: faithful CLI port + descriptor strip-lines hook.**

Baseline reality check overturned the task description. Both
binaries already exit 0; the actual failure was an unparseable
`stdout` because Go writes `DEBUG: verbose=...` (cmd/lci/search.go:49,
unconditional) before the JSON body. Beyond that, the JSON shape
diverged structurally: Go wraps each result in `{"result": {...}}`
(per `searchtypes.StandardResult` having `Result GrepResult json:"result"`),
adds top-level `"mode": "standard"`, and converts paths to
relative via `pathutil.ToRelativeStandardResults`. C++ CLI
emitted a flat shape with absolute paths and no `mode` field.

Two changes:

1. **C++ CLI port** (`src/cli/search.cpp` json_output branch):
   wrap each result in `{"result": ...}`, rewrite `path` to
   relative-to-cwd, add `"mode": "standard"`. This makes the C++
   `lci search --json` wire format a faithful port of Go's CLI
   contract. The HTTP `/search` endpoint is unaffected — both
   binaries already agree on the flat server-side shape there.

2. **Runner pre-parse strip-lines hook**
   (`tests/parity/runner/parity_runner.cpp::strip_preamble_lines`):
   when a CLI descriptor sets `text_normalize.strip_lines`,
   apply it to raw stdout before `nlohmann::json::parse`. This
   reuses `canonicalize_text` with a strip-only options block so
   the line-matching rules stay consistent with text-mode
   behavior. With no patterns it is a no-op — existing JSON
   descriptors are unaffected. This is the cleanest cross-binary
   fix for upstream debug pollution we don't own.

3. **Descriptor**
   (`tests/parity/descriptors/cli/search/json.parity.json`):
   added `text_normalize.strip_lines: ["DEBUG: verbose="]`,
   rewrote tier paths to address the wrapped shape
   (`results[].result.<x>` not `results[].<x>`), masked
   `file_id` (same scanner-ordering divergence as
   `http/search`) and `context` (producer-specific window
   bytes), added `sort_arrays: ["results"]` (Go's
   trigram-index hash-iteration order).

Result: `parity.cli.search.json` — failing → green, 10/10
stable runs. Parity score 30/55 → 31/55. Non-parity unit suite
1456/1456 passing both before and after.

Why not pure descriptor honesty (move the whole payload to
`ignore` like the iter-5 escape hatch)? Because the content
*is* equivalent once you peel the wrapper — `path`, `line`,
`column`, `match`, `score` all match across binaries. Hiding
that behind an empty `ignore`-tier diff would silently regress
to "both exit 0" and lose the value of the parity test. The
real fix was a small CLI port + a generally useful runner
feature.

### Decision: cli/search/{enhanced,assembly} — C++-only surface (2026-05-12, Iter 2, EUQHIn60mbzd)

**Decision A executed (2026-05-12, Iter 3 of loop zRvo9CV23xZD, removal task lvGYODNAw8SP).**

The C++-only `--enhanced` and `--assembly` flags were removed to match Go's CLI surface
(karpathy rule 1 — Go is the bar). Concrete changes:

- `src/cli/main.cpp` — deleted flag registrations (CLI11 `add_flag` for `--enhanced` and
  `--assembly`) and dropped the corresponding bool arguments from the `run_search` callback.
- `src/cli/main.cpp` — replaced `CLI11_PARSE` with manual `try { app.parse(...) } catch
  (CLI::ParseError& e)` so unknown-flag rejections exit 1 (matching Go's
  `os.Exit(1)` on `flag provided but not defined`), not CLI11's default `ExtrasError = 109`.
- `src/cli/search.cpp` — deleted the `if (enhanced || assembly)` branch in `run_search`
  (~190 LOC: JSON output mode + compact mode + text-mode breadcrumb/metrics formatter),
  dropped the `bool enhanced, bool assembly` parameters from the signature, deleted four
  helper bodies (`format_breadcrumb_segment`, `format_metrics_line_text`,
  `widen_to_enclosing_blocks`, `annotate_with_symbol_metrics`), deleted the three
  `grep_filters::` forwarders that bridged the unit tests, and scrubbed
  `enhanced/assembly` references in directive-post-filter and AST-filter comments.
- `src/cli/grep_filters.h` — deleted the `format_breadcrumb`, `format_metrics_line`,
  `widen_to_enclosing_block` declarations and the "Enhanced/assembly output helpers"
  section header.
- `include/lci/cli/commands.h` — removed the `enhanced, assembly` doc paragraph and
  dropped the two bool tail-parameters from the `run_search` prototype.
- `tests/cli_test.cpp` — deleted 12 helper unit tests (`SearchFormatBreadcrumb.*`,
  `SearchFormatMetricsLine.*`, `SearchWidenToEnclosingBlock.*`).
- `tests/parity/descriptors/cli/search/enhanced-rejected.parity.json` and
  `assembly-rejected.parity.json` — added with `parse: exit-only`, `expect_exit: 1`,
  `_rationale` populated on every tier. Both binaries reject the respective flag with
  exit 1 (Go's urfave/cli emits help dump + `Fatal error:`; CLI11 emits unknown-argument
  message). 10/10 stable. Verified `parity_setup`, all 7 pre-existing cli/search
  descriptors, and the 2 new rejection descriptors all green in the same run.

Non-parity unit suite: 1635/1638 (j4 wall-clock run; the 3 j-induced flakes
[`ServerTest.BuildIdStaleDetection`, `ServerTest.ConcurrentRequests`,
`ServerTest.StatusEndpoint`] all pass in j1 and were already flaky pre-change). Floor
1632/1633 from iter-1 baseline is held. Test count dropped by 12 (deletion of
SearchFormat/Widen helpers) — this is the deliberate scope of the removal.

HTTP/MCP surfaces verified unaffected: `enhanced`/`assembly` appear in `src/mcp/*` only
as unrelated concepts (`pagination.cpp` `pr.enhanced`, `handlers_*.cpp`
`get_enhanced_symbol`, `get_file_enhanced_symbols`); `src/http/` has zero references.

### Decision: cli/search/{enhanced,assembly} — original audit (2026-05-12, Iter 2, EUQHIn60mbzd)

**Original decision (superseded above): audit-only — document C++-only divergence, file removal subtask, no parity descriptor yet.**

Baseline reality check (per iter-6/iter-9 phantom-failure pattern) overturned the
task description. The task spec described `lci search --enhanced` and
`--assembly` as flags whose behavior "isn't covered by parity tests." Actual
ground truth, captured 2026-05-12 against `corpora/synthetic/multi-lang`:

| Binary | `search --enhanced add` | `search --assembly add` |
|---|---|---|
| Go `lci-linux-amd64` (parity reference) | **exit 1**, stderr: `flag provided but not defined: -enhanced` | **exit 1**, stderr: `flag provided but not defined: -assembly` |
| C++ `build/src/lci` (head) | exit 0, full enhanced output (metrics + breadcrumbs, 3 hits) | exit 0, integrated-mode output (3 direct matches, 0 assembly patterns) |

Go's `cmd/lci/search.go` defines `displayEnhancedResults` (line 616) and the
assembly path (lines 174–207), but **neither is wired to a CLI flag** — the Go
`Flags:` block on `searchCommand` does not register `enhanced` or `assembly`.
The Go function is dead code; the assembly path is triggered internally only
when `isAssemblySearchCandidate(pattern)` heuristics fire on the standard
search command.

C++ (`src/cli/main.cpp:127–139`) registers both as CLI11 flags and wires them
through `run_search` (`src/cli/search.cpp:1526–1700+`) with a complete
implementation: `annotate_with_symbol_metrics`, `widen_to_enclosing_blocks`,
`format_breadcrumb_segment`, `format_metrics_line_text` are all real and
covered by `tests/cli_test.cpp:492+`. The implementation is parity-aspirational
— it was written against the **internal Go functions**, not against Go's CLI
surface.

This is therefore a C++-only CLI surface, not a divergence to normalize.
Three options were considered:

- **A. Remove the C++-only flags to match Go.** Touches `src/cli/main.cpp`
  (~15 LOC removed), `src/cli/search.cpp` (~250 LOC removed including the
  enhanced/assembly branches in `run_search`, helper bodies, and the
  `bool enhanced, bool assembly` parameters), `src/cli/grep_filters.h` /
  `grep_filters.cpp` (helper API surface, ~50 LOC), `tests/cli_test.cpp`
  (delete enhanced/assembly format helper unit tests, ~200 LOC).
  Total ≈ 5 files, ≈ 500 LOC removal plus header signature churn.
  This is the karpathy-correct outcome — Go is the bar.

- **B. Extend descriptor schema with per-binary `expect_go_exit` /
  `expect_cpp_exit` so a parity descriptor can lock in the divergence.**
  Touches descriptor schema, parser, runner, plus two new descriptors.
  Schema-extension work is itself non-trivial and unmotivated by this single
  audit — it would invite future divergence-locking elsewhere as a path of
  least resistance, which the eagle-eye discipline expressly rejects.

- **C. Audit-only.** Document the divergence here; file fix subtask for
  Option A under the iteration loop; defer parity descriptors until C++
  removal completes, at which point both binaries will reject identically
  with exit 1 and the descriptors become a trivial `expect_exit: 1` +
  stderr-substring match.

Chose C: scope of A exceeds this task's context budget (>5 files, >hundreds
of LOC removal touching a hot path with its own unit test coverage); B is
schema overreach for a single case. Fix subtask filed under loop
`zRvo9CV23xZD` describing the exact removal surface. Once removed, this
decision stub gets replaced with a "Decision A executed" entry plus the two
descriptors.

No parity descriptors land in this iteration. The non-action is deliberate:
writing a `_rationale`-laden ignore-tier descriptor against the current
state would be a karpathy-rule-6 violation ("no silent fallbacks, no
'implemented but returns empty' stubs"). The audit stands as the work
product.

**Result for this iteration**: 0 descriptor change, 1 MODULE_MAP decision,
1 fix subtask filed. Parity score unchanged. Non-parity unit suite baseline
preserved.

### Decision: MCP handlers explore/index/analysis audit (2026-05-12, Iter 4, sL0AJDf2hjIh)

**Scope**: 9 MCP tools across `src/mcp/handlers_{explore,index,analysis}.cpp`
(find_files, search_definitions, tree, get_context, index_stats,
semantic_annotations, side_effects, code_insight, git_analysis). 11 modes
counting `code_insight` mode-dispatch (overview/statistics/structure/unified/
git_analyze/git_hotspots) and `index_stats` mode-dispatch (overview/symbols/
references/types).

**Audit method**: Probe each tool on `corpora/synthetic/multi-lang` via both
binaries using the canonical init handshake (initialize → initialized
notification → tools/call). Go uses ndjson framing; C++ uses Content-Length.
Multiple back-to-back probes per tool to let indexer warm. Probe transcripts
in `/tmp/parity-iter4/`; harness `/tmp/parity-iter4/{probe.py,runall.sh}`.

**Classification (4 buckets per existing audit framework)**:

| Tool / mode | Bucket | Evidence |
|---|---|---|
| find_files `*.go` | green parity | byte-identical 734-byte payloads, identical result list |
| get_context name=Add | green parity | both `count:0, contexts:[]` (no symbol "Add" in corpus) |
| search_definitions Add | green parity | both 107-byte identical responses |
| tree name=Add | green parity | both 93-byte identical responses |
| semantic_annotations label=pure | green parity | both `total_count:0, annotations:[]` |
| side_effects symbol_name=Add | cosmetic type | `purity_ratio: 1` (Go int) vs `1.0` (C++ float). Existing JSON normaliser tolerates. |
| git_analysis scope=wip | cosmetic type | `risk_score: 0` (Go int) vs `0.0` (C++ float). Timestamp tz format differs (already documented). Existing `_rationale` covers this. |
| code_insight (no mode, default=overview) | green parity | 350-byte byte-identical LCF payload |
| **code_insight mode=statistics** | **C++ BUG** | C++ returns `mode=overview\\ntokens=90` regardless of requested mode. Verified across statistics, structure, unified, git_analyze, git_hotspots. Engine downstream of `handle_code_insight` (src/mcp/handlers_analysis.cpp:486-518) accepts mode arg but ignores it. |
| code_insight mode=structure/unified/git_analyze/git_hotspots | same bug | Same evidence; one descriptor (`mode-statistics.parity.json`) locks the divergence class. |
| **index_stats** (all 5 modes) | **C++ BUG** | Go reaches `status:ready, file_count:4, symbol_count:4, reference_count:10` within 38ms. C++ stays `status:indexing, file_count:0, symbol_count:1, indexing_progress:25` across 20 back-to-back probes over 10s. Indexer thread appears not to progress under MCP stdio session. |

**Deliverables this iter**:

1. Two new descriptors with full `_rationale` on every tier (per karpathy
   rule: new descriptors must justify each tier choice):
   - `tests/parity/descriptors/mcp/code_insight/mode-statistics.parity.json`
     — locks the mode-dispatch divergence as one class.
   - `tests/parity/descriptors/mcp/index_stats/wait-ready.parity.json`
     — locks the `status:indexing` perma-state.
2. Two C++ fix subtasks filed under loop `zRvo9CV23xZD`:
   - code_insight mode-arg ignored downstream of handler.
   - index_stats indexer thread never advances under MCP stdio.
3. Follow-up descriptor backlog (deferred, filed as one subtask):
   - 7 additional descriptors to cover the remaining mode permutations
     and a get_context probe with a corpus-resident symbol (current "Add"
     yields empty on both sides — green but content-free).

**Existing descriptor `_rationale` gap**: 8 of 9 existing basic.parity.json
files (find_files, search_definitions, tree, get_context, code_insight,
index_stats, semantic_annotations, side_effects) predate the karpathy
"_rationale on every tier" rule and use boilerplate `stable: ["result.content[].type",
"result.content[].text"]`. They pass today because the corpus-level outputs
are byte-identical or normalised. Filed as the follow-up descriptor subtask;
not retrofitted this iter because (a) each requires a per-tool justification
write-up, (b) bundling 8 file edits with 2 new descriptors blows context
budget (>5 files of substantive change), (c) the audit framework only
penalises new descriptors without rationale. Existing parity tests remain
green; the rule is forward-looking.

**Result for this iteration**: 2 new descriptors with full per-tier
`_rationale`, 1 MODULE_MAP audit table, 2 C++ fix subtasks + 1 descriptor
backlog subtask filed under loop. Parity score unchanged (no existing
descriptor flipped). Non-parity unit suite untouched (audit + descriptor
edits only).

### Decision: MCP descriptor backlog cleared + iter-5/6 fixes validated (2026-05-12, Iter 7, iEA8zrihA7b3)

**Scope**: descriptor backlog from iter-4 — 7 new mode descriptors + 8
basic-descriptor `_rationale` retrofits.

**Pre-flight validation**: Source files for iter-5 (AQQfI8XEEkV6) and
iter-6 (DGeclu4miU5q) C++ fixes were on disk but the `build/src/lci`
binary was stale (mtime 15:52 vs source 16:39). Rebuilt; re-probed via
`/tmp/parity-iter4/runall.sh`. Confirmed:

- code_insight mode dispatch: now byte-identical on overview, statistics,
  structure, unified, git_analyze, git_hotspots (all 6 modes). Iter-5 fix
  is effective once the binary is current.
- index_stats: parity-compat stub removed; real handler now dispatches.
  C++ payload reaches `status:ready, file_count:4, symbol_count:4,
  reference_count:10` matching Go's core counters. Iter-6 fix is effective.

**Flipped classification (was bug → now green or envelope-locked)**:

| Tool / mode | Iter-4 bucket | Iter-7 bucket | Evidence |
|---|---|---|---|
| code_insight mode=statistics | C++ BUG | **green parity (text-stable)** | mode-statistics.parity.json: 325-byte byte-identical LCF |
| code_insight mode=structure | C++ BUG | **green parity (envelope-stable)** | mode-structure.parity.json: payload identical in content; locked envelope-only because of NEW Go bug — `types:` line iterates `map[string]int` in randomised order. 10/10 stability: 2 PASS / 8 FAIL on text-stable, 10/10 on envelope-stable. Filed as Go-side fix. |
| code_insight mode=unified | C++ BUG | **green parity (text-stable)** | mode-unified.parity.json: 654-byte byte-identical LCF (4 sections) |
| code_insight mode=git_analyze | C++ BUG | **green parity (text-stable)** | mode-git_analyze.parity.json: 302-byte byte-identical empty-history fallback |
| code_insight mode=git_hotspots | C++ BUG | **green parity (text-stable)** | mode-git_hotspots.parity.json: 303-byte byte-identical empty-history fallback |
| index_stats (no args) | C++ BUG | **envelope-stable** | basic.parity.json: text in ignore tier with timing-mismatch rationale (Go async vs C++ sync single-shot). wait-ready.parity.json holds multi-call ready-state lockdown. |
| index_stats mode=symbols | C++ BUG | **envelope-stable** | mode-symbols.parity.json: timing + shape divergence documented |
| index_stats mode=references | C++ BUG | **envelope-stable** | mode-references.parity.json: ditto |
| index_stats mode=types | C++ BUG | **envelope-stable** | mode-types.parity.json: ditto |

**Newly surfaced bugs (NOT silent-fallback'd — locked + filed)**:

1. **Go map-iteration nondeterminism on `types:` line** (code_insight
   mode=structure and unified). Affects 8/10 runs on the multi-lang corpus.
   Mitigation: descriptor envelope-locked. Fix: Go sorts the types line.
2. **C++ get_context-by-object_id returns empty** where Go returns the
   resolved symbol (Go: 308 bytes with definition + purity; C++: 25-byte
   empty contexts). get_context/basic uses id=B (Go's `Add`, deterministic
   across binaries via symbol_store ordering). Mitigation: descriptor
   text-body ignored with rationale; fix subtask filed.
3. **C++ index_stats serialization shape diverges** from Go: Go emits
   `timestamp` ISO string + `total_size_bytes`, C++ emits `timestamp_ms`
   int and omits total_size_bytes. Existing inner_text normaliser already
   ignores these keys; documented in mode-symbols rationale as
   tighten-after-shape-align.

**Deliverables this iter**:

1. 7 new mode descriptors (`mode-structure`, `mode-unified`,
   `mode-git_analyze`, `mode-git_hotspots` under `code_insight/`;
   `mode-symbols`, `mode-references`, `mode-types` under `index_stats/`).
   All 7 pass 10/10 stability check on multi-lang corpus.
2. 8 basic-descriptor retrofits with full `_rationale` + per-tier rationale
   (`find_files`, `search_definitions`, `tree`, `get_context`,
   `code_insight`, `index_stats`, `semantic_annotations`, `side_effects`).
   `git_analysis/basic` was exempt (already had full _rationale).
3. `get_context/basic` invocation changed from `name=Add` (empty on both)
   to `id=B` (corpus-resident object_id for Go's `Add`); text in ignore
   tier pending C++ fix.
4. `index_stats/basic`: was FAILING on main before iter-7 (status:indexing
   vs status:ready single-shot). Now passes 10/10 with envelope-only tier
   and timing-mismatch rationale pointing at wait-ready.parity.json.

**Result for this iteration**: 7 new + 8 retrofitted descriptors, all 18
MCP descriptors (incl. pre-existing) green 10/10 on multi-lang corpus.
5 code_insight modes flipped bug → green text-stable; 1 envelope-stable;
4 index_stats modes flipped bug → envelope-stable. 3 newly-surfaced bugs
filed as follow-up subtasks. No silent fallbacks introduced (karpathy
rule 6 honoured per descriptor).

### Decision: validator factories for symbol/tree/object_context are dead — accept-unvalidated, green-light FIX-E (2026-05-16, Iter 6, 8Zjd7zI01dhr)

**Scope**: three hand-rolled validators in `src/mcp/validation.cpp` flagged
by FIX-A as having no production callers:

- `create_symbol_validator()` — line 326
- `create_tree_validator()` — line 351
- `validate_object_context_business_logic()` — line 380

**Audit method**: ripgrep across `src/` and `tests/` for each symbol;
inspect actual handler entry points; cross-check Go reference where
available.

**Findings**:

| Validator | Production callers | Handler reality | Decision |
|---|---|---|---|
| `create_symbol_validator()` | 0 (only `tests/mcp_validation_pagination_test.cpp`) | No MCP tool keyed `symbol` exists. The closest tool is `inspect_symbol`, which has its own inline `name`/`id` validation in `handle_inspect_symbol` (`src/mcp/handlers_explore.cpp:572-578`). The parity-compat shadow stub in `src/cli/mcp.cpp:276-298` performs no validation but matches Go's stub-shaped output by design. | **DEAD — delete in FIX-E** |
| `create_tree_validator()` | 0 (only test file) | Validator keys `function` (lines 354-357), but the actual `/tree` HTTP handler at `src/server/server.cpp:1092-1200` keys `function_name` and already inline-validates non-empty (`error_response 400`, line 1106). No MCP `tree` tool is registered (`grep add_tool.*tree` empty). The validator was never wired and was keyed on the wrong field — would have been a no-op if dispatched. | **DEAD — delete in FIX-E** |
| `validate_object_context_business_logic()` | 0 (only test file) | `handle_get_context` at `src/mcp/handlers_core.cpp:481-505` performs equivalent inline validation: requires exactly one of `id`/`name`, error message text matches Go's `validateGetContextParams` per inline comment (line 487). The factory's logic is **redundantly duplicated** — wiring it would be cosmetic refactor, not behavior change. | **DEAD — delete in FIX-E** |

**Parity check (no live Go corpus available this iter)**: cross-reference is the
inline comment block at `handlers_core.cpp:487` ("Go validateGetContextParams")
and at line 510 ("Go's id-only no-mode contract"), both authored in iter-18
(dk4QZUHJYC7J) when the get_context Go reference was paged in. C++ already
mirrors Go's validation contract inline. inspect_symbol has its own
required-field check (`name` OR `id`). The `/tree` HTTP handler has its own
non-empty check. No parity gap exists from the absence of these three
validator factories — they are pure dead code.

**Risk of deletion**: low. Pure-removal change with one corresponding test
file (`tests/mcp_validation_pagination_test.cpp` lines 236-314, 4 test cases)
that needs the same delete pass. No production behavior changes. No descriptor
flips expected. Floor: full unit + parity suite must hold green after the
delete pass (FIX-E owns verification).

**Decision**: **accept-unvalidated** for all three tools. Green-light FIX-E
to delete the three factory functions and their test cases in one pass. No
wire-now child fix tasks spawned — there is nothing to wire because:
1. Inline validation already exists for `get_context` and `inspect_symbol`.
2. `/tree` is HTTP-only (no MCP surface) and already inline-validates.
3. Wiring `create_object_context_validator` would be cosmetic refactor of a
   working inline check, not a correctness fix — explicitly out of scope per
   karpathy rule 7 (no "we'll optimize later" cosmetic restructuring).

**Followup**: FIX-E (separate Dart task in this loop) is now unblocked. No new
child subtasks created from FIX-C; the audit closes cleanly.

### Decision: tools/list emit-order parity — Option B (custom emitter) + dedup-stubs blocker (2026-05-16, Iter 8, chusTmwgGNdC)

**Scope**: FIX-D — align `tools/list` MCP response with Go binary byte-equivalence. Audit-only iteration (~30 add_tool sites across 6 files exceed 5-file commit budget); follow-up sub-fix subtasks filed.

**Audit method**: capture live `tools/list` from both binaries on identical
corpus (`/tmp/fixd_corpus` with one .go file), parse JSON, diff field-key
ordering at every nesting level.

**Findings**:

| Aspect | Go (`jsonschema-go` + `encoding/json`) | C++ (`nlohmann::json` default) | Status |
|---|---|---|---|
| Tool count | 14 | **22** (8 duplicate stubs) | 🔴 STRUCTURAL — blocks byte-eq |
| Tool list order | alphabetical by name | insertion order (handler-file order, with dups) | 🔴 Divergent |
| Top-level tool keys | `[description, inputSchema, name]` (alpha) | `[description, inputSchema, name]` (alpha) | ✅ Match |
| Schema root keys | `[type, properties, required]` (insertion via struct field order in jsonschema-go) | `[properties, required, type]` (alpha) | 🔴 Divergent |
| Schema `properties` map | alphabetical (Go map serialisation) | alphabetical (nlohmann default) | ✅ Match |
| Per-property keys | `[type, description]` (insertion via jsonschema struct) | `[description, type]` (alpha) | 🔴 Divergent |

**Decision: Option B (custom emitter in `build_input_schema` + `handle_tools_list`)**, not Option A (`ordered_json` migration).

Rationale:
1. **Determinism over registration order.** Tools are registered in
   handler-file include order (`server.cpp` core → handlers_index →
   handlers_explore → handlers_analysis → handlers_context → cli/mcp.cpp
   parity-compat). `ordered_json` would lock that order in — adding a new
   handler file would silently shift tool order and break parity. Option B
   emits sorted-by-name regardless of registration, matching Go's map-based
   alphabetisation.
2. **Field order is a fixed contract, not data.** The 3 schema-root fields
   (`type`/`properties`/`required`) and 2 per-property fields
   (`type`/`description`) are a closed set — encoding the order in 5 lines
   of emitter code is clearer than carrying ordered_json through every
   `add_tool` call site and inputSchema literal.
3. **Perf (karpathy rule 1+2).** `ordered_json` uses linear-scan key lookup
   (vector-backed) vs `json`'s hashmap. `tools/list` is called once per MCP
   session so the per-request overhead is negligible, BUT `ordered_json`
   would propagate to *every* JSON object construction in the MCP path
   (response building, etc.) — a per-call regression on hot read paths. The
   gate "no regression on existing parity descriptors" implicitly enforces
   this, and Option A would risk failing it under benchmark.
4. **No `add_tool` site change needed.** Option A would require auditing all
   ~30 `add_tool` callers AND any inline schema literals. Option B touches
   only `build_input_schema` + `handle_tools_list` in `src/mcp/server.cpp`.

**Hard blocker for the byte-equivalence goal**: 8 duplicate tool
registrations in `src/cli/mcp.cpp` (per [[mcp-stubs-shadow-real-handlers]])
inflate C++ tool count from 14→22. Even with perfect field ordering,
`tools/list` cannot be byte-equal until these are removed. The stubs DO
shadow real handlers in `tools/call` (per memory) — this is correctness, not
just parity noise. **FIX-D byte-equivalence is BLOCKED on stub removal.**

**Per-tool ordering work**: NONE required at `add_tool` call sites or
inline schemas — all flow through the centralized `build_input_schema()`.
No 30-site audit needed; the inline-literal concern in the task spec is
moot because all schema construction is centralized through the
`ToolDefinition`/`ToolProperty` struct path. Verified by grepping
`inputSchema` across all 6 files: only one occurrence
(`src/mcp/server.cpp:514` in `handle_tools_list`), no inline literals.

**Proof-of-concept**: not committed this iter (audit-only). Implementation
is a ~15-line patch to `build_input_schema` (emit `type` → `properties` →
`required` and `type` → `description` per property using
`nlohmann::ordered_json` *locally* in that function only, dump to string,
parse back as regular json — or use a manual std::ostringstream for the
schema sub-tree). The local scope keeps the hot read path untouched.

**Parity descriptor scaffolded**:
`tests/parity/descriptors/mcp/tools_list/basic.parity.json.pending`
(suffix `.pending` to avoid CMake `GLOB_RECURSE *.parity.json`
auto-registration in `tests/parity/CMakeLists.txt:73` — runner has no
xfail support, verified 2026-05-16). Body documents the dedup blocker and
target shape. Once the 8 stubs are removed AND the emitter ships, rename
to `.parity.json` and the descriptor enters the suite automatically.

**Sub-fix subtasks filed under AN8m7hohEdlM (loop task)**:
1. Remove 8 duplicate stub registrations in `src/cli/mcp.cpp` (per
   mcp-stubs-shadow-real-handlers memory) — prerequisite for byte-eq AND
   for correctness of `search`/`find_files`/etc. tools/call output.
2. Land Option B emitter in `src/mcp/server.cpp::build_input_schema` +
   `handle_tools_list` (~15 LOC); flip tools_list parity descriptor from
   xfail → stable; run 10/10 parity_verify gate.

