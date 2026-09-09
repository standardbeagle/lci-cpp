---
name: lci-parsing-languages
description: "Use when: adding or fixing a language grammar, extractor, or extension mapping; asking what lci extracts for a given language (symbols, refs, imports, receiver types, side effects, catch sites); debugging zero-symbol files or parse skips; comparing lci language coverage to ctags, LSP, SCIP, ast-grep, semgrep."
---

# LCI parsing and per-language extraction

lci parses source with vendored tree-sitter grammars and walks each tree once with
`UnifiedExtractor`, producing symbols, scopes, references (call refs qualified by a
syntactically inferred receiver type), imports, complexity, type relationships, and
optional side-effect and catch-site facts. Trees are discarded after the walk; the
index holds only the extracted records. 13 grammars plus Svelte via script masking.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| Indexing parse step (every `lci index`/server start/reindex) | `src/indexing/pipeline_processor.cpp:39` `run_unified_extraction` | ext -> grammar -> PooledParser -> UnifiedExtractor; records `parse_skip_reason` |
| Parse-skip warnings in index progress | `src/indexing/pipeline.cpp:163` | oversize/minified/parser_unavailable/parse_failed reported as recoverable `ErrorType::Parse`; unsupported_grammar deliberately silent |
| `.lci.kdl` `index { max_parse_file_size }` | `src/config/config.cpp:225` | files above it are trigram-only, no symbols |
| MCP `index_stats` `overview.language_breakdown` / `files_by_language` | `src/mcp/handlers_index.cpp:82` `get_files_by_language`, `:223`, `:278` | language names come from `to_string(LangId)` |
| MCP `debug_info mode=files` | `src/mcp/handlers_index.cpp:277` | per-file symbol list |
| HTTP `POST /fileinfo` | `src/server/server.cpp:1185`, handler `src/server/server_endpoints.cpp:202` | file_id, path, symbol_count |
| MCP `search`/`grep` `languages` filter | `src/mcp/handlers_search.cpp:109-128` | separate name->extension alias table (see traps) |
| MCP `semantic_annotations` (`@lci:` comments) | `src/mcp/handlers_side_effects.cpp:250`; engine `src/core/semantic_annotator.cpp:97` `extract_annotations`, `:202` `populate_from_index` | RE2 patterns at `semantic_annotator.cpp:80-89` |
| MCP `side_effects` / `code_insight` error-handling (extractor sink) | `src/cli/mcp.cpp:122`, `src/cli/server.cpp:355` `set_side_effect_sink` | sink only attached for MCP/server runtimes; plain indexing pays nothing |
| `git_analysis` re-parse of blobs at a ref | `src/git/analyzer.cpp:187-200` | second consumer of parser + extractor |
| Svelte components | `src/indexing/pipeline_processor.cpp:54-62`, `src/parser/svelte_script.cpp:76` `mask_svelte_script` | markup masked to spaces, script parsed as JS/TS at original positions |

## Code map

Entry -> parse -> extract -> index:
- `include/lci/language_map.h` — header-only single source of truth: `kLangMap` (extension -> `LangId`, `LangFamily`, `is_code`), `language_info`, `language_info_for_path`, `to_string(LangId)`. There is no `.cpp`.
- `include/lci/parser/parser.h`, `src/parser/parser.cpp` — `parser::Language` enum (13 + `Tsx`), `language_from_extension` (`parser.cpp:27`, `.tsx` special-cased before the table), `get_ts_language`, `make_parser`, `UniqueParser`/`UniqueTree` RAII.
- `include/lci/parser/parser_pool.h`, `src/parser/parser_pool.cpp` — `ParserPool` (thread_local at `parser_pool.cpp:40`, `ts_parser_reset` on release), `PooledParser` guard.
- `include/lci/parser/svelte_script.h`, `src/parser/svelte_script.cpp` — `SvelteScriptInfo`, `mask_svelte_script`; `lang="ts"` selects the TS grammar.
- `include/lci/parser/unified_extractor.h` — `UnifiedExtractor`, `ExtractionResults` (symbols, blocks, imports, scopes, references, declarations, complexity, field_types, `depth_limit_hit`), `set_side_effect_sink`, `local_var_types_`, `kotlin_property_types_`, `zig_module_aliases_`, `kMaxVisitDepth = 512`.
- `src/parser/unified_extractor.cpp` — `init` (:172), `extract` (:235, file/folder scopes, Cython `scan_cython_callables` post-pass :296), `get_results`/`take_results` (:304/:310), `visit_node` (:329, depth guard :337, env save/restore :361), `swaps_local_type_env` (:547), `is_function_node` (:556), `process_scope_node` (Kotlin fieldless name fallback :699), `process_symbol_node` (:844, node-type dispatch).
- `src/parser/unified_extractor_symbols.cpp` — `extract_function` (:76), visibility (`scan_declared_visibility` :179, Rust `is_rust_test_scaffold` :137), `extract_method`/`extract_class`/`extract_type_declaration`/... , C/C++ locals+params (:1156-1218), JS/Python/Go import extraction (:913-940), `extract_signature` (:983), `extract_doc_comment` (:1019), `count_complexity_point` (:1040).
- `src/parser/parse_methods.cpp` — Java (:28-92), C# (:104-226), PHP (:265-329), Zig struct (:380), C/C++ specifiers/namespace/include/using (:426-532), Rust use (:544), Kotlin object/import (:556/:586).
- `src/parser/unified_extractor_references.cpp` — `process_reference_node` dispatch (:110), one `process_<lang>_reference` per language (Go :397, JS :575, Python :708, Java :840, C# :900, Rust :988, PHP :1080, Kotlin :1172, Ruby :1303, Zig :1352; C-family inline in the dispatcher), `create_call_reference` (:1541, carries arg count).
- `src/parser/unified_extractor_types.cpp` — `process_type_relationships` (:13): Go, JS/TS, Python, PHP only.
- `src/parser/unified_extractor_side_effects.cpp` — writes, throws, guards, catch dispatch (:392: `catch_clause`/`except_clause`/`rescue`/`catch_block`), Go error-drop (:334), Zig `try`/`errdefer` (:416).
- `src/parser/unified_extractor_catch_sites.cpp` — `process_catch_site` (:239), `cause_fidelity` (:170), `errors_are_message_only` (:128: Go, Zig, C-family).
- `src/parser/unified_extractor_internal.h` — `first_named_child_typed` (:19) and other tree helpers.
- `include/lci/types.h:16` `SymbolType` (26 variants); `include/lci/symbol.h` `Symbol`, `FieldType`, `EnhancedSymbol` (`receiver_type` :127); `include/lci/scope.h` `BlockType`, `ScopeType` (:62), `ScopeInfo`, `SymbolScopeType`; `include/lci/side_effects.h` `CauseFidelity` (:309), `CatchSiteInfo` (:406); `include/lci/indexing/pipeline_types.h:65` `ParseSkipReason`.
- `src/indexing/pipeline_processor.cpp` — parse entry; `src/indexing/pipeline_scanner.cpp:302` `FileScanner::detect_language` (routes through language_map).
- `src/core/import_resolver.cpp` — `ImportResolver::extract_file_imports` (:12): line-based, family-gated cross-file import graph; `src/core/reference_tracker.cpp:1280` `resolve_reference_target` (receiver-typed `Type.M` match :1323), `derive_receiver_type` (:1157).
- `src/core/semantic_annotator.cpp` — `@lci:` comment metadata.
- `cmake/TreeSitterGrammars.cmake` — 13 grammar pins (`declare_ts_grammar`), scanner kinds, TypeScript/TSX and PHP sub-grammar builders; core tree-sitter in `CMakeLists.txt:451`.
- Tests: `tests/language_extraction_test.cpp` (45: one `LanguageExtractionTest.<Lang>` per language, `ScopeTypeResolution.*`, `IndexProfile.StageBreakdown` env-gated), `tests/unified_extractor_test.cpp` (47), `tests/language_map_test.cpp` (8, incl. `ParserAgreesWithTable`, `GitLanguageAgreesWithTable`), `tests/parser_pool_test.cpp` (13), `tests/svelte_extraction_test.cpp` (8), `tests/side_effect_extraction_test.cpp` (166), `tests/integration/real_project_languages_test.cpp` (7 receiver-type checks on real corpora: gson, serilog, ripgrep, guzzle, okhttp, sinatra, zls; skip without corpora), goldens `tests/integration/goldens/mcp/index_stats/basic.json`, `tests/integration/goldens/http/fileinfo.json`.
- Docs: `README.md:169` Languages section, `docs/src/content/docs/index.mdx:45`, `docs/plans/2026-06-17-scope-type-resolution.md` (receiver-type design + status), `docs/plans/2026-08-17-ast-retention-ir-design.md` (parse-and-discard vs IR), `docs/reviews/2026-09-03-review.md` S8 (parser findings).

## Per-language capability matrix

Grounded in the dispatch sites above and `tests/language_extraction_test.cpp`.

| Language (exts) | Symbols | References | Cross-file imports (`import_resolver.cpp:22-31`) | Receiver-type call resolution | Type relationships (`_types.cpp:13`) | Side effects | Catch sites |
|---|---|---|---|---|---|---|---|
| Go `.go` | yes | yes | yes | yes | yes | yes (+ error-drop, panic) | no catch construct; error-return tracked |
| Python `.py .pyw .pyi .pyx .pxd` | yes (Cython cpdef/cdef recovered by line scan) | yes | yes | yes (`self`/`cls`) | yes | yes | `except_clause` |
| JavaScript `.js .jsx .mjs .cjs` | yes | yes | yes | yes (`this`, `new T()`) | yes | yes | `catch_clause` |
| TypeScript `.ts .tsx .mts .cts` | yes (TSX grammar for `.tsx`) | yes | yes | yes (typed params) | yes | yes | `catch_clause` |
| Rust `.rs` | yes (pub/`mod tests` visibility) | yes | yes | yes (`self`, `let x: T`) | no | yes | no catch construct |
| C `.c` | yes (locals, params) | yes | yes (`#include`) | yes (`this` n/a; `T x;`) | no | yes | none |
| C++ `.cpp .cc .cxx .h .hpp .hh .hxx .h++` | yes | yes | yes | yes | no | yes | `catch_clause` |
| Java `.java` | yes | yes | no | yes | no | yes | `catch_clause` |
| C# `.cs` | yes (record, property, delegate, event) | yes | yes | yes | no | yes | `catch_clause` |
| PHP `.php .phtml` | yes (trait, namespace, const) | yes | yes | yes (`$this`) | yes (trait use -> extends) | yes | `catch_clause` |
| Kotlin `.kt .kts` | yes (fieldless-grammar fallback; object) | yes | no | yes (class property map) | no | yes | `catch_block` |
| Zig `.zig` | yes (`const X = struct`) | yes (`@import` alias qualifies calls) | yes | yes | no | yes (`try`, `errdefer`) | none |
| Ruby `.rb` | yes (module, class, method) | yes (bare no-paren calls not emitted) | no | yes (`T.new`) | no | yes | `rescue` |
| Svelte `.svelte` | via JS/TS on masked script | via JS/TS | JsTs family | as JS/TS | as JS/TS | as JS/TS | as JS/TS |

Swift, Scala, Lua, Vue and the rest of the `is_code` tail in `kLangMap` are text-searchable only (`ParseSkipReason::UnsupportedGrammar`).

## Config knobs

| Key (`.lci.kdl`) | Struct field (`include/lci/config.h`) | Default | Effect |
|---|---|---|---|
| `index { max_parse_file_size }` | `IndexConfig::max_parse_file_size` (:25) | 2 MB | above it: trigram only, `Oversize` skip; 0 disables |
| `index { max_file_size }` | `IndexConfig::max_file_size` (:20) | 10 MB | file not indexed at all |
| `index { data_file_token_cap }` | `IndexConfig::data_file_token_cap` (:34) | 4096 | only `!is_code` files per language_map |
| `performance { parallel_file_workers }` | `PerformanceConfig::parallel_file_workers` (:57) | 0 = auto | parser pool is per thread, so this is the parser count |
| (no key) minified-bundle gate | `is_trigram_hostile` `src/core/trigram.cpp:499` | built in | `MinifiedBundle` skip |
| env `LCI_PROFILE_DIR` | test-only | unset | enables `IndexProfile.StageBreakdown` per-file parse/extract/trigram timings |
| env `LCI_UPDATE_GOLDENS=1` | test-only | unset | regenerates integration goldens |

No config knob enables or disables a language; extension coverage is the compiled `kLangMap`.

## Invariants and traps

- Every per-language dispatch reads `lang_`/`family_` set from `language_info` in `init()`, never raw `ext_` (`unified_extractor.h` field comment; `.claude/rules/bench-harness-oracle-independence.md` rule 8: `.hh` vs `.cpp` must produce identical output; pinned by `LanguageExtractionTest.MjsEmitsReferences/PyiImportProducesReferences/HhHeaderEmitsReferences`).
- Nested function entry snapshots and restores `local_var_types_` (`unified_extractor.cpp:361`); Go `func_literal` is excluded on purpose (closures inherit). `kotlin_property_types_` is class-scoped and cleared on class entry (`_references.cpp:1179`); the review flagged clear-without-restore as observable across a local class (`bench-harness-oracle-independence.md` rule 8, second bullet).
- `visit_node` depth guard at 512 frames sets `depth_limit_hit` and drops the subtree; symbols outside it are intact (`unified_extractor.h` `kMaxVisitDepth`).
- tree-sitter-kotlin (fwcd 0.3.8) is fieldless: `name` field lookups return null; use `first_named_child_typed` (`docs/plans/2026-06-17-scope-type-resolution.md` prerequisite gaps).
- Receiver-type resolution is the SCIP base case only: syntactic env, no type checker, unknown receiver degrades to the bare name, never a fabricated edge (`docs/plans/2026-06-17-scope-type-resolution.md`). `foreign_receiver` marks dynamic dispatch (`reference_tracker.h:364`).
- Zero symbols is never silent: every skip sets `ParseSkipReason` (`pipeline_processor.cpp:32-38`), but `UnsupportedGrammar` is not reported as a warning (`pipeline.cpp:158-165`).
- Cross-file import resolution is a separate LINE-BASED walker in `import_resolver.cpp`, not the tree-sitter `imports` vector. Families covered: Go, JsTs, Python, Rust, CSharp, CFamily, Php, Zig; Java, Kotlin, Ruby are not. `README.md:171-173` lists only six (omits PHP and Zig): doc drift.
- `ExtractionResults.imports` (tree-sitter import symbols) has no consumer outside `src/parser/` and the unit tests (grep `.imports` in `src/` hits only `context_lookup_structure.cpp:296`, a different field). Two producers for one fact (`karpathy-principles.md` rule 4 class).
- `src/mcp/handlers_search.cpp:109-128` keeps its own language->extension alias table (has `rake`, `gemspec`, `dart`, `vue`; python lacks `pyx`/`pxd`; `h` listed under `c` while `kLangMap` says Cpp). This is the drift class `language_map.h` was introduced to remove.
- Findings from the 2026-09-08 mapping (fixed or filed): see `docs/reviews/2026-09-08-skill-mapping-findings.md`.
- `pipeline_processor.cpp:110` and `git/analyzer.cpp:200` call `get_results()` (copies every vector); `take_results()` exists for exactly this caller (`unified_extractor.h` comment).
- `tests/fixtures/sample_{cpp,go,js,py}.txt` are copied at build (`tests/CMakeLists.txt:440`) but no test reads them; language tests use inline `kGoSrc`-style constants.
- Any test touching env/locale must use `tests/helpers/portable_env.h` and skip on `_WIN32`; the Windows leg runs the full `lci_tests` (`.claude/rules/test-iteration-discipline.md` rule 8, `.github/workflows/ci.yml`).
- Known per-language gaps (from the plan status): Ruby bare no-paren calls are `identifier`, not emitted as calls; Kotlin/Zig constructor calls emit a bare Call on the type name. Real-project audit (`benchmarks/repo-qa/ANALYSIS-insight-verification.md` D2) found stdlib-name call collisions inflating reach (Kotlin `apply`, Zig `initCapacity`, C++ `find`).
- Trees are never retained; AST re-query needs a re-parse (`docs/plans/2026-08-17-ast-retention-ir-design.md`, option B compact IR recommended, not built).
- Dogfood gap: `lci def UnifiedExtractor` returns the header class line only; locating the per-TU member definitions still needs grep.

## Probe recipes

```sh
L=build/release/src/lci
# Which files parsed and per-language counts for a root
$L status -r /path/to/repo
# MCP index_stats language_breakdown (server must be running for the root)
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"index_stats","arguments":{"mode":"overview"}}}' | $L mcp -r /path/to/repo
# Symbols for one file (checks the extractor for that language)
$L symbols -f 'src/parser/*.cpp' -k all -r .
# Receiver-type qualified callers
$L refs mask_svelte_script -r .

# Unit tests, direct gtest binary (fastest)
build/release/tests/lci_tests --gtest_filter='LanguageExtractionTest.*:ScopeTypeResolution.*'
build/release/tests/lci_tests --gtest_filter='UnifiedExtractorTest.*:LanguageMap.*:ParserTest.*:ParserPoolTest.*:Svelte*'
build/release/tests/lci_tests --gtest_filter='SideEffectExtraction.*'
# Same via ctest
ctest --test-dir build/release -R 'LanguageExtractionTest|LanguageMap|ParserPoolTest' -j4
# Per-file parse/extract/trigram stage timings over a corpus
LCI_PROFILE_DIR=/path/to/repo build/release/tests/lci_tests --gtest_filter='IndexProfile.StageBreakdown'
# Real-corpus receiver-type checks (skip without corpora under benchmarks/repo-qa/.work)
build/release/tests/lci_real_project_tests --gtest_filter='RealProjectLanguages.*'
# Goldens: tests/integration/goldens/{mcp,http,cli}; refresh with LCI_UPDATE_GOLDENS=1
ctest --test-dir build/release -R lci_integration_suite --output-on-failure
```

Build only what you run: `cmake --build build/release --parallel --target lci_tests`.

## Product comparison

Comparable tools: universal-ctags (symbol extraction, 100+ languages, regex/parser mix), tree-sitter based matchers (ast-grep, semgrep), per-language LSP servers (gopls, pyright, rust-analyzer, clangd, jdtls, ...), Sourcegraph SCIP indexers (scip-go, scip-typescript, scip-python, scip-java, scip-clang, rust-analyzer SCIP), GitHub code navigation (tree-sitter tag queries + stack-graphs precise nav for a few languages), Serena MCP (LSP-backed).

lci claims in this area:
- "Tree-sitter parsing across 13 languages" (`README.md:4`, `:169-173`; `docs/src/content/docs/index.mdx:45-47`). Measure: `ParserTest.AllGrammarsLoad`, `LanguageExtractionTest.AllParsersCreate`, and `cmake/TreeSitterGrammars.cmake` pin list.
- "Cross-file import resolution for Go, JavaScript, Python, Rust, C#, C/C++" (`README.md:171-173`). Measure: `import_resolver.cpp:22-31` family switch (code also covers PHP, Zig).
- Receiver-type call resolution for all 13 languages (`docs/plans/2026-06-17-scope-type-resolution.md` Status). Measure: `ScopeTypeResolution.*` + `RealProjectLanguages.*`; precision numbers per corpus were not found in-repo beyond the pass/fail tests.
- Parse cost share: "parse is ~58% of index CPU" (`include/lci/config.h:23`). Measure: `IndexProfile.StageBreakdown` with `LCI_PROFILE_DIR`; numbers in `docs/performance/` are suite timings, not per-language parse rates.

| Axis | lci | How to check competitor | Notes |
|---|---|---|---|
| Language count | 13 grammars + Svelte masking (`kLangMap`) | `ctags --list-languages`; ast-grep/semgrep docs; SCIP indexer list | ctags covers far more languages (unverified, from model knowledge); lci trades breadth for uniform extraction depth |
| Symbol kinds | 26 `SymbolType`s, visibility, parameter_count, signature, doc comment | `ctags --list-kinds-full=<lang>` | ctags has per-language kinds; no references |
| References / call graph | syntactic refs + receiver-typed calls, `foreign_receiver` for dynamic | LSP `textDocument/references`; SCIP occurrence roles | LSP/SCIP are type-checker precise; lci is heuristic, no generics, no overload resolution beyond arity |
| Cross-file resolution | line-based import graph, 8 families | scip-* symbol monikers; stack-graphs | Java/Kotlin/Ruby unresolved cross-file in lci |
| Incremental parse | none; full re-parse per file, tree discarded | tree-sitter `ts_parser_parse` with old tree; LSP incremental sync | see ast-retention plan |
| Error tolerance | tree-sitter error recovery; Cython recovered by line scan | same for ast-grep/semgrep/GitHub | LSP servers fail harder on broken code |
| Side effects / catch-site classification | extractor sink, 166 tests | semgrep rules, SonarQube/CodeQL queries | lci has no dataflow; syntactic only |
| Zero-symbol diagnostics | `ParseSkipReason` per file | ctags `--verbose`; LSP logs | |

Honest gaps vs the field: no type checker or semantic model (name + receiver-type heuristic); no cross-file imports for Java, Kotlin, Ruby; Swift/Scala/Vue/Lua text-only; no incremental parsing; no persisted ASTs or IR (parse-and-discard); grammar versions pinned and vendored, so new syntax lags upstream; Svelte markup expressions, styles and component tags are not indexed (script block only).

## Related skills

lci-feature-map (router), lci-search, lci-symbol-navigation, lci-get-context, lci-code-insight, lci-side-effects, lci-git-analysis, lci-indexing-pipeline, lci-server-lifecycle, lci-mcp-server, lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation.
