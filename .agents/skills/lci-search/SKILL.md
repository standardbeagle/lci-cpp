---
name: lci-search
description: "Use when: touching lci search/grep CLI flags, the MCP search tool, POST /search, SearchEngine ranking, trigram/postings narrowing, synonym or fuzzy semantics, search goldens, or comparing lci search against ripgrep/Zoekt/Sourcegraph/embedding search."
---

# LCI Content Search

`lci search` (alias `s`), `lci grep` (alias `g`), the MCP `search` tool and `POST /search` all run one in-memory engine: candidate files are narrowed by a trigram bloom plus a token postings index, then verified by a literal or RE2 scan, then scored by file type and enclosing symbol. Results carry the enclosing symbol (name, type, object id, caller count) so an agent can answer from the hit lines without opening the file. Search is content only; file-path search is `find_files` (see lci-symbol-navigation).

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| CLI `lci search PATTERN [paths...]` (alias `s`) | `src/cli/main.cpp:98` (flags 98-281) -> `run_search` `src/cli/search.cpp:249` | Literal substring by default; `-E` RE2; `-i -j -w -l --count --max-count --patterns --invert-match --comments-only --code-only --strings-only --template-strings --context-filter --rank-by --group --light --ids/--no-ids --compact-search -e/--exclude --inc/--include -m/--max-lines --cpu-profile --mem-profile --compare-search (no-op notice)`. Full text: `build/release/src/lci search --help`. |
| CLI `lci grep PATTERN [paths...]` (alias `g`) | `src/cli/main.cpp:286` -> `run_grep` `src/cli/grep.cpp:175` | grep-shaped flags: `-n -C -i -j -e --inc --exclude-tests --exclude-comments -E -v --patterns -c -l -M`. `-E` is applied to rows returned by the literal engine; a pure-meta regex with no >=3-char literal seed is refused with an error (`src/cli/grep.cpp:255-276`). |
| MCP tool `search` | registered `src/mcp/handlers_core.cpp:219`; handler `handle_search` `src/mcp/handlers_search.cpp:361` | Params `pattern, patterns, max, max_per_file, output, path, filter, flags, include, symbol_types, semantic, languages`. `flags` CSV: `cs wb nt nc iv rx` (`handlers_search.cpp:430-435`). `max` default 15, clamp [1,100] (`:415-416`). Output grouped per file with `hits[]` (`:903`). |
| MCP tool `search_definitions` | none (not registered) | The golden `tests/integration/goldens/mcp/search_definitions/basic.json` pins `unknown tool "search_definitions"`; definition search reaches MCP only through `search` and `list_symbols`. |
| HTTP `POST /search` | route `src/server/server.cpp:1177`; handler `IndexServer::handle_search` `src/server/handlers_search.cpp:28`; body decode `server_request::decode_search` `include/lci/server/request_decode.h:105` | Body keys `pattern, paths, max_results, case_insensitive, declaration_only, max_context_lines`. A `paths` scope matching no indexed file is a 4xx error, not an empty success (`handlers_search.cpp:46-60`). Client side: `Client::search` `src/server/client.cpp:88`. |
| Config `search { ... }` and `synonyms { ... }` in `.lci.kdl` | `src/config/config.cpp:590` (search node), `:374-390` (ranking), `apply_synonyms` `:441` | See Config knobs. |

## Code map

Entry -> engine -> index, top to bottom.

- `src/cli/main.cpp` — CLI11 definitions for `search` (98-281) and `grep` (286-405); `SearchCommandOptions` / `GrepCommandOptions` filled here.
- `src/cli/search.cpp` — `run_search`: resolves scope paths, parses advanced query syntax (`query_parser::parse` at `:349`, post-filter `query_parser::apply_all` at `:631`), pages the server (`search_all_pages` `:169`, `kSearchServerCeiling = 100000` `:167`), unions `--patterns` (`search_union_patterns` `:70`), applies AST/grep filters, renders. Header comment `:1-40` is the `--json` shape contract.
- `src/cli/grep.cpp` — `run_grep` and `render_search_output` (`:69`); regex seed extraction and refusal `:255-276`.
- `src/cli/search_shared.h` — internals shared by `run_search` / `run_grep`.
- `src/cli/grep_filters.{h,cpp}` — pure row filters, unit-tested in `tests/cli_test.cpp`: `apply_word_boundary` (209), `apply_path_filters` (262), `apply_max_count_per_file` (329), `count_per_file_rows` (345), `files_with_matches_rows` (368), `apply_exclude_tests` (491), `apply_exclude_comments` (507), `regex_literal_seeds` (599), `regex_every_match_has_seed` (809), `regex_filter_results` (820), `split_literal_alternation` (890), `group_rows_by_file` (915).
- `src/cli/ast_filters.{h,cpp}` — `--comments-only/--strings-only/--code-only` predicates: `match_is_in_string_literal` (49), `match_is_in_comment` (224).
- `src/cli/rank_options.h` — `--rank-by relevance|recency|file-type` and `--context-filter`; `_rationale` for the Go aliases `proximity|similarity`.
- `src/cli/query_parser.h` — `file:*.cpp kind:function -deprecated foo` directive syntax; fuzzed by `tests/fuzz/fuzz_query_parser.cpp`.
- `src/cli/name_aggregation.h` — "name xN" caller/callee aggregation used by the compact renderer.
- `include/lci/cli/column.h` — THE column contract: 0-based byte offset, `kColumnUnknown = -1`; matches `rg --json`.
- `src/mcp/handlers_search.cpp` — MCP `handle_search`: schema validation (`search_schema_validator`), language -> extension table (`language_ext_table`), semantic fan-out via `expand_pattern_semantic` (`:596-610`), regex auto-fallback with 0.7 score scaling (`:622-630`), `include=` add-ons (breadcrumbs, refs, ids, safety, deps), per-file grouping (`:903`).
- `src/server/handlers_search.cpp` — HTTP handler; `src/server/client.cpp` — `Client::search` used by the CLI.
- `include/lci/search/search_engine.h` — `ContextExtractor` (59), `SearchEngine` (119), `SearchCoordinator` (183).
- `include/lci/search/search_options.h` — `SearchOptions` (88: `max_results, max_context_lines, case_insensitive, declaration_only, usage_only, full_function, exclude_tests, exclude_comments, word_boundary, invert_match, merge_file_results, max_count_per_file, use_regex, semantic, include_object_ids`), `SearchResult`/`SearchMatch`/`SearchContext`, constants `kMaxMatchesPerFile = 100` (23), `kNonSymbolPenalty` (48), `kBaseMatchScore = 100` (53), `line_is_comment_only` decl (255).
- `include/lci/search/symbol_type_alias.h` — `symbol_types=` alias normalisation.
- `src/search/engine.cpp` — `SearchEngine::search`, `find_matches`, `process_file`, `score_result` (882: base + non-symbol penalty + file-type score + pattern complexity), `looks_like_regex` (141).
- `src/search/engine_context.cpp` — `ContextExtractor::extract_{line,block,function}_context`.
- `src/search/search_coordinator.cpp` — pure helpers `merge/deduplicate/rank/unique_paths`; `line_is_comment_only` (50) is the ONE comment predicate shared by CLI and MCP.
- `src/indexing/master_index_search.cpp` — `MasterIndex::search_with_options` (78), `find_candidate_files` (97), `execute_search` (219). Comment at `:343-352` explains certified-absence narrowing; narrowing applied `:353-367`; path-scope filter applied index-side after it.
- `include/lci/core/trigram.h` — `TrigramBloom` (110), `TrigramIndex` (145) with `Narrowing` (`certifies_absent`, `informative`, `covered_files` 289), `TrigramIndex::narrow` decl (243). `PostingsIndex` is declared here too (`src/core/postings.cpp` implements it).
- `src/core/trigram.cpp` — `utf8_seq_step` (20) shared by `to_code_points_into` (49) and `compute_byte_offsets_into` (113); `TrigramIndex::Narrowing::certifies_absent` (819); `TrigramIndex::narrow` (843).
- `src/core/trigram_bucketing.cpp` — 256-way bucket assignment for parallel trigram merge.
- `src/core/postings.cpp` — `PostingsIndex::tokenize_content` (126), `find` (248), `narrow` (293: boundary-aware token runs; PARTIAL files self-nominate so a capped token set never certifies absence).
- `src/core/line_scanner.cpp`, `include/lci/core/line_scanner.h` — line offsets for context extraction and `line_at_offset`.
- `src/semantic/synonym_table.cpp` (`SynonymTable`, built-in dev-verb groups `:26`), `stemmer.cpp` (Porter), `name_splitter.cpp`, `fuzzy_matcher.cpp` — semantic layer. Consumers of the search area: `src/search/engine.cpp`, `src/mcp/handlers_search.cpp`, `src/mcp/handlers_find_files.cpp`.
- Tests, unit (`lci_tests`): `tests/search_engine_test.cpp` (SearchEngineRanking, SearchEngineRegexValidation, SearchFlagInvertMatch, SearchFlagNoComments, SearchSymbolTypeFilter, SearchCoordinatorTest, ContextExtractorTest, CommentPredicate), `tests/master_index_search_test.cpp` (MasterIndexSearchTest, narrowing at `:1082`), `tests/search_rg_differential_test.cpp` (SearchRgDifferentialTest: naive scanner + `rg --fixed-strings` oracle), `tests/trigram_test.cpp` (TrigramIndexNarrowTest, TrigramBloom, Utf8Walkers, TrigramHostileTest, PostingsTokenCapPolicyTest), `tests/semantic_test.cpp`, `tests/line_scanner_file_service_test.cpp`, `tests/cli_test.cpp` (GrepFilters*, RankOptions*, RegexLiteralSeeds, AlternationSeedTest, SplitLiteralAlternation, SearchPagingTest, SearchMetaRegexTest, ColumnContract, SearchFlagEffectTest).
- Tests, integration: `tests/integration/search_parity_test.cpp`, `tests/integration/real_project_phrase_test.cpp` (RealProjectPhraseMatchingTest, RealProjectCaseMatchingTest, RealProjectScoreValidationTest), `tests/integration/real_project_performance_test.cpp:77` (`FastapiSearchUnder5ms`), spec runner `tests/integration/spec_runner.cpp`.
- Goldens: `tests/integration/goldens/cli/search/` (basic, case-insensitive, compact, grep, json, no-results, regex), `tests/integration/goldens/cli/grep_compat/` (11 files), `tests/integration/goldens/mcp/search/` (basic, language-filter, unknown-param-guard), `tests/integration/goldens/mcp/search_definitions/basic.json`, `tests/integration/goldens/http/search.json`. `tests/integration/goldens/mcp/grep/basic.json` pins that `grep` is NOT an MCP tool (`unknown tool "grep"`).
- Fuzz: `tests/fuzz/fuzz_search_input.cpp`, `fuzz_query_parser.cpp`, `fuzz_trigram_extract.cpp`.
- Benchmarks: `tests/benchmarks/benchmark_main.cpp:114,130` (`BM_SearchSmallIndex`, `BM_SearchMediumIndex`), `tests/benchmarks/real_project_benchmarks.cpp:130,156` (chi, fastapi), `real_project_advanced_benchmarks.cpp:177`; baseline `tests/benchmarks/baseline/linux-x64.json`. Agent-level: `benchmarks/repo-qa/ANALYSIS-discovery-sweep.md`.
- Docs: `docs/TOOLS.md` (search section 72-109), `docs/src/content/docs/cli-usage.mdx` (17-45), `mcp-server.mdx` (128-145), `http-socket-api.mdx` (36, 55-62), `docs/src/content/docs/synonym-split-naming.mdx`, `README.md:3-4,102`.

## Config knobs

All in `include/lci/config.h`; parsed in `src/config/config.cpp`. No environment variable affects search.

| `.lci.kdl` key | Struct field | Default | Consumed? |
|---|---|---|---|
| `search { max_results }` | `SearchConfig::max_results` | 100 | yes |
| `search { max_context_lines }` | `SearchConfig::max_context_lines` | 100 | yes |
| `search { enable_fuzzy }` | `SearchConfig::enable_fuzzy` | true | NO. Header comment `config.h:121-126`: parsed for Go parity, not read by the engine. |
| `search { merge_file_results }` | `SearchConfig::merge_file_results` | true | yes |
| `search { ensure_complete_stmt }` | `SearchConfig::ensure_complete_stmt` | false | yes |
| `search { include_leading_comments }` | `SearchConfig::include_leading_comments` | true | yes |
| `search { ranking { enabled code_file_boost doc_file_penalty config_file_boost require_symbol non_symbol_penalty } }` | `SearchRankingConfig` (`config.h:109`) | true / 50 / -20 / 10 / false / -30 | yes (`config.cpp:374-390`) |
| `synonyms { clear-all; clear "a" "b"; group "get" "fetch" }` | `Config::synonyms` (`config.h:182`, default `SynonymTable::build_default`) | curated dev-verb set | yes, MCP `semantic=true` fan-out |
| (none) | `SemanticConfig` (`config.h:83`), `SemanticScoringConfig` (`config.h:90`) | see struct | NO KDL node parses them and no code outside `config.h` reads them (dead declarations). |
| `default_context_lines` | `SearchConfig::default_context_lines` | 0 | declared; not in the `search` parser branch (`config.cpp:404-432`). |

MCP-side caps that are not config: `max` default 15 clamp 100 (`handlers_search.cpp:415`); CLI server page ceiling 100000 (`search.cpp:167`); `kMaxMatchesPerFile = 100` (`search_options.h:23`).

## Invariants and traps

- Certified-absence narrowing: a file is skipped ONLY when an index that covers it proves the pattern absent; PARTIAL postings, uncovered trigram files, phrases with spaces, identifier substrings and case-sensitive mixed-case all fall through to the verify scan. The old "any non-empty index result is a narrowing" logic returned silent zeros for months. Source: `src/indexing/master_index_search.cpp:343-352`, `src/core/postings.cpp:278-283`.
- The standing oracle for that class is `tests/search_rg_differential_test.cpp`: seeded random patterns from real content, checked against a naive scanner and `rg --fixed-strings` (rg 14.1.0 is on this host). It shares no mechanism with the index. Rule: `.claude/rules/bench-harness-oracle-independence.md` rule 5.
- Two producers, one stream: `to_code_points_into` and `compute_byte_offsets_into` MUST step with the shared `utf8_seq_step` (`src/core/trigram.cpp:20`); a desync made `TrigramBloom::build` drop trigrams and `certifies_absent` certify present text absent. Pin equal lengths per invalid-UTF-8 class (`Utf8Walkers` in `tests/trigram_test.cpp`). Source: `.claude/rules/karpathy-principles.md` rule 4.
- Regex seeds: `regex_literal_seeds` unions every >=3-char literal run per alternation branch; `regex_every_match_has_seed` decides whether the trigram fast path is lossless. Escape classes (`\x41`, `\pL`, `\Q..\E`) once fooled both because the checker mirrored the extractor. `lci search -E` with an unseedable pattern does a full RE2 scan; `lci grep -E` refuses it (`src/cli/grep.cpp:256`). Pinned by `AlternationSeedTest.UnseededBranchForcesFallback` in `tests/cli_test.cpp`.
- Column contract: 0-based byte offset everywhere, `kColumnUnknown` for invert rows (`include/lci/cli/column.h`). Changing what an emitted field MEANS requires `grep -rl <field> tests/integration/goldens/` before the gate; `grep_compat` goldens drifted once. Source: `.claude/rules/test-iteration-discipline.md` rule 4.
- `FastapiSearchUnder5ms` is an absolute wall-clock assertion inside the gate; a lone failure under host load is a load artifact, confirm in isolation before forcing. Source: `.claude/rules/test-iteration-discipline.md` rule 6.
- MCP `search` caps at 100 results by design; `truncated:true` plus a `dirs` histogram tell the caller to narrow with `path=`. Renderers must honour `truncated`/`total_matches` or a cap reads as recall loss. Source: `docs/TOOLS.md:72-109`, `.claude/rules/bench-harness-oracle-independence.md` rule 9.
- Semantic fan-out (`semantic=true`, multi-word pattern, no `patterns`) expands via `SynonymTable`; synonym-injected patterns match case-insensitively. Regex auto-fallback fires only when the literal pass returns fewer than `max` and `looks_like_regex` is true; those rows are scaled by 0.7 and deduped against literal hits (`src/mcp/handlers_search.cpp:596-630`).
- Trigram maps cover only incrementally indexed files; bulk-indexed corpora are narrowed by per-file case-folded `TrigramBloom`s (about 2.4% false positives) plus postings. Hostile/high-entropy files stay unfiltered (`TrigramHostileTest`).
- `--rank-by` accepts Go aliases `proximity|similarity` that both map to `relevance` (`src/cli/rank_options.h`). `--compare-search` only prints a notice. `--light` and `enable_fuzzy` are parity shells.
- Read path is lock-free (RCU snapshots); no mutex on `/search`. Source: `.claude/rules/karpathy-principles.md` rule 3, `src/server/handlers_search.cpp:53-55`.
- Known gaps from `docs/reviews/2026-09-03-review.md` S12 (verify against the tree before citing, some are fixed): pure-meta regex full-scan path ignoring `scoped_paths`/`--exclude`/`--max-count`; post-filters capped by the server page; any token starting with `-` treated as an exclusion (`->next` needs `--`). The `nc`/`iv`-never-read finding (S5) is fixed: `handlers_search.cpp:433-434` set the options.
- Findings from the 2026-09-08 mapping (fixed or filed): see `docs/reviews/2026-09-08-skill-mapping-findings.md`.
- Dogfood: `lci def SearchEngine` and `lci refs certifies_absent` both worked for this map; `lci search` could not answer "which KDL keys does config.cpp parse" (needed grep with a regex over `child.name ==`), a gap for structured-pattern search.

## Probe recipes

Binary: `build/release/src/lci`; root flag `-r .`.

```sh
L=build/release/src/lci; R=$(git rev-parse --show-toplevel)
$L search "certifies_absent" -r $R                       # literal, symbol-aware output
$L search "narrow(" -i -j --group -r $R src/core         # scoped, JSON, grouped per file
$L search -E 'utf8_seq_step|to_code_points_into' -r $R   # seeded regex fast path
$L search -E '\d{4}-\d{2}' -r $R                         # pure-meta: full RE2 scan
$L grep "TrigramBloom" -c -r $R                          # grep-shaped count per file
$L grep -E '^\s*$' -r $R ; echo "exit=$?"                # grep refuses unseeded regex
$L search "fetch user" -r $R                             # phrase with space (silent-zero class)
$L search --rank-by recency --context-filter function "search" -r $R
printf '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"search","arguments":{"pattern":"score_result","output":"ctx:2","flags":"cs","include":"refs"}}}\n' | $L mcp -r $R
curl --unix-socket /tmp/lci-$(id -u).sock -X POST http://localhost/search -H 'Content-Type: application/json' -d '{"pattern":"score_result","max_results":5}'
```

Targeted tests (build `lci_tests` only, never a clean rebuild):

```sh
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='SearchEngine*:MasterIndexSearch*:SearchRgDifferentialTest.*:Trigram*:Utf8Walkers.*:PostingsTokenCapPolicyTest.*'
build/release/tests/lci_tests --gtest_filter='GrepFilters*:RankOptions*:RegexLiteralSeeds.*:AlternationSeedTest.*:SplitLiteralAlternation.*:SearchPaging*:SearchMetaRegex*:ColumnContract.*:SearchFlagEffectTest.*'
ctest --test-dir build/release -R lci_integration_suite --output-on-failure   # all goldens, ~53 s
LCI_UPDATE_GOLDENS=1 ctest --test-dir build/release -R lci_integration_suite  # re-pin after an intended contract change
```

Specs live beside the goldens: `tests/integration/mcp/search/*.spec.json`, `tests/integration/cli/`, `tests/integration/http/`. Micro-benchmarks: `build/release/tests/lci_benchmarks --benchmark_filter=Search` (target `lci_benchmarks`, `tests/CMakeLists.txt:324`, Linux only), baseline in `tests/benchmarks/baseline/linux-x64.json` (`BM_SearchSmallIndex` 12576.5, `BM_SearchMediumIndex` 13169.2, units as recorded there).

## Product comparison

Comparable tools: ripgrep and GNU grep (stateless text scan), Zoekt and Sourcegraph (trigram index with symbol ranking, persistent, multi-repo), GitHub code search (trigram-like sparse n-gram index, hosted), livegrep and csearch (trigram index over a corpus, CLI/web), Cursor and other embedding-based codebase search (semantic vector retrieval, no exact match guarantees).

lci claims in this area:

| Claim | Where made | How to measure | Existing numbers |
|---|---|---|---|
| Sub-millisecond search | `README.md:3`, `docs/TOOLS.md:74`, MCP tool description `src/mcp/handlers_core.cpp:220` | `lci_benchmarks --benchmark_filter=Search`; `FastapiSearchUnder5ms` (asserts under 5 ms on the fastapi corpus, in-process, `tests/integration/real_project_performance_test.cpp:77`); time `lci search --json` and read `time_ms` | `tests/benchmarks/baseline/linux-x64.json`; the 5 ms test bound is the only pinned latency figure. Note the CLI path adds server round trip and spawn; "sub-millisecond" is the in-process engine, not the CLI wall clock. |
| ~80% context reduction versus grep | `README.md:3-4`, `docs/src/content/docs/index.mdx:36` | `benchmarks/repo-qa` tokens column, baseline (grep) vs treatment (LCI MCP) | No checked-in artifact in this repo reproduces 80%; `grep -rn '79.8\|80%' benchmarks/ docs/performance/` is empty. Treat as unmeasured until re-run. |
| Search results are grouped and symbol-annotated so agents skip file reads | `src/mcp/handlers_core.cpp:220-228` | Count `read` tool calls after `search` in an agent trace (`benchmarks/repo-qa` tool-call column) | `benchmarks/repo-qa/ANALYSIS-discovery-sweep.md:88-99`: `control_literal_string_search` F1 0.840 for LCI vs 1.000 for grep (grep wins, 12/12, p=0.0005); baseline recall 1.000 on every family. |
| Literal search never returns a silent empty | `src/indexing/master_index_search.cpp:343-352` | `SearchRgDifferentialTest`, `lci_integration_suite` | Differential oracle passes on the self corpus; rg is on this host. |

Comparison axes:

| Axis | lci | How to check the competitor | Notes |
|---|---|---|---|
| Index model | in-memory trigram bloom per file + token postings + verify scan; rebuilt on server start (`README.md:161`) | Zoekt/livegrep/csearch: on-disk trigram index; rg/grep: none; GitHub: hosted sparse n-gram (unverified, from model knowledge) | lci has no disk persistence. |
| Exact-match soundness | certified-absence narrowing, rg-differential oracle | rg is itself the exact oracle; Zoekt uses trigram + verify (unverified, from model knowledge) | Embedding search offers no soundness contract. |
| Regex | RE2, seeded fast path, full scan for pure-meta in `search`, refusal in `grep` | rg: full Rust regex incl. lookaround absent; GNU grep: PCRE via -P; Zoekt: RE2-class | Test with `\pL`, `\x41`, `\Q..\E`, alternations with one unseeded branch. |
| Case folding | `-i` folds; blooms are case-folded, stored trigram maps case-sensitive | rg `-i`, `-S` smart case | lci has no smart-case. |
| Symbol awareness | each hit carries enclosing symbol, type, object id, caller count; ranking boosts code files, penalises non-symbol lines | Zoekt ranks symbols via ctags; Sourcegraph adds SCIP precise nav; rg has none | Compare on "find the definition of X" precision, `control_unique_name_definition` in the discovery sweep (F1 0.729 vs 0.292). |
| Synonym/semantic expansion | KDL `synonyms` groups, multi-word fan-out, Porter stemmer and name splitter in `src/semantic/` | Cursor/embedding search: vector similarity; rg/Zoekt: none | lci expansion is deterministic and auditable; `enable_fuzzy` is a no-op. |
| Scope filters | positional paths, `--inc/-e` regex, `languages`, `symbol_types`, `nt/nc` flags, `--comments-only/--strings-only/--code-only` | rg `-g`, `-t`, `--type-add`; Zoekt `file:` `lang:` atoms | lci `-e` is exclude, not pattern (differs from grep). |
| Result caps and paging | MCP hard cap 100 with `truncated` + `dirs` histogram; CLI pages to 100000 | rg `-m`, `--max-count`; Zoekt `num`/`max_match_display` | A renderer must honour `truncated`. |
| Latency measurement | in-process bench + 5 ms gate; CLI wall clock includes socket + spawn | `hyperfine rg PATTERN` on the same corpus, warm cache | Compare like with like: lci engine vs rg cold and warm. |
| Incremental update | watch mode via efsw + debouncer; blooms rebuilt per file | rg none needed; Zoekt reindex; Sourcegraph scheduled | See lci-indexing-pipeline. |
| Multi-repo | one server per root | Zoekt/Sourcegraph/GitHub: fleet-wide | lci is single-corpus by design. |

Honest gaps versus the field: no disk persistence (`README.md:161`); no smart-case; no multi-repo; `grep -E` refuses unseeded regexes rather than scanning; MCP results capped at 100; the 80% context-reduction claim has no reproducing artifact in the repo; the only agent-level literal-search measurement shows grep ahead on F1 (`ANALYSIS-discovery-sweep.md:92`); `SemanticConfig`/`SemanticScoringConfig`/`enable_fuzzy` are declared but inert.

## Related skills

lci-feature-map (router), lci-symbol-navigation, lci-get-context, lci-parsing-languages, lci-code-insight, lci-side-effects, lci-git-analysis, lci-indexing-pipeline, lci-server-lifecycle, lci-mcp-server, lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation
