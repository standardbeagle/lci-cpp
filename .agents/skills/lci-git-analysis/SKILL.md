---
name: lci-git-analysis
description: "Use when: touching or tracing `lci git-analyze`, MCP `git_analysis`, `code_insight mode=git_analyze|git_hotspots|analysis=impact`, HTTP /git-analyze, src/git/* (Provider, Analyzer, FrequencyAnalyzer, PatternDetector), git log/diff parsers, hotspot or churn output; or comparing lci's change analysis against CodeScene, code-maat, git-of-theseus, hercules, gitinspector."
---

# LCI Git Analysis

Analyzes a git change set (staged, working tree, one commit, or a range) against the live index and reports duplicates of existing functions, naming problems, and complexity/length/nesting findings scoped to the changed hunks. A second engine reads `git log` over a time window and reports churn hotspots, multi-author collision zones, and a churn x complexity risk matrix. All surfaces shell out to the `git` binary (argv exec, no shell) and read the same in-memory index the rest of lci uses.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| CLI `lci git-analyze` (alias `ga`) | `src/cli/main.cpp:829` registers; `src/cli/commands_query.cpp:33` `run_git_analyze` | Flags `-s/--scope`, `-b/--base`, `-t/--target`, `-f/--focus`, `--threshold`, `-m/--max-findings`, `-j/--json`. Talks to the server over the socket (`/git-analyze`), 5 min client timeout. Text mode prints summary, duplicates, naming only (see traps). |
| MCP `git_analysis` | `src/mcp/handlers_index.cpp:493` `add_tool`; handler `handle_git_analysis` at `src/mcp/handlers_index.cpp:357` | JSON payload from `git::report_to_json`. Params: scope, base_ref, target_ref, focus[], similarity_threshold (0.8), max_findings (20). Non-git root returns `available=false`, not isError. |
| MCP `code_insight mode=git_analyze` | `src/mcp/handlers_analysis.cpp:689` mode dispatch; schema `src/mcp/handlers_analysis.cpp:2318` | Same `git::Analyzer`, rendered as LCF `== GIT CHANGES ==` via `emit_git_changes` (`src/mcp/insight_sections.cpp:354`). Fixed defaults (threshold 0.8, max 20, all focus). |
| MCP `code_insight mode=git_hotspots` | `src/mcp/handlers_analysis.cpp:689` (else branch), `src/mcp/insight_sections.cpp:441` `emit_git_hotspots` | Only production surface of `git::FrequencyAnalyzer`. Params `time_window` (7d/30d/90d/1y, default 30d), `file_pattern`. Appends `== RISK MATRIX ==` (churn x max complexity, CodeScene-style quadrants) computed inline in `handlers_analysis.cpp`. |
| MCP `code_insight mode=detailed analysis=impact` | `src/mcp/handlers_analysis.cpp:913` | Blast radius: `Provider::get_changed_scope` hunks x symbol spans, then transitive callers. Default scope `wip`. |
| HTTP `POST /git-analyze` | route `src/server/server.cpp:1217`; handler `IndexServer::handle_git_analyze` `src/server/server_endpoints.cpp:590` | Body `{scope (required), base_ref, target_ref, similarity_threshold, max_findings, focus[]}`; 400 on bad scope; 200 `{available:false}` on non-git root; `{"report": ...}` otherwise. Client wrapper `Client::git_analyze` `src/server/client.cpp:311`, request struct `include/lci/server/server.h:163`. |
| MCP `info` listing | `src/mcp/handlers_core.cpp:175` | One-line entry only. |

## Code map

Entry -> core -> storage. `lci def <name> -r .` resolves every symbol named here.

- `src/cli/commands_query.cpp` — `run_git_analyze`: validates scope, loads config, ensures server, calls `/git-analyze`, prints text or JSON.
- `src/mcp/handlers_index.cpp` — `handle_git_analysis`: scope validation, `Provider::create`, `tracks_any` gate (untracked root nested in an outer repo is refused), runs `Analyzer`, emits JSON.
- `src/mcp/handlers_analysis.cpp` — `handle_code_insight` git branches: `git_analyze`, `git_hotspots` (+ RISK MATRIX), `analysis=impact`.
- `src/mcp/insight_sections.cpp` — `emit_git_changes`, `emit_git_hotspots` (LCF renderers); `include/lci/mcp/insight_sections.h:133`.
- `src/server/server_endpoints.cpp` — `IndexServer::handle_git_analyze` (HTTP).
- `include/lci/git/types.h` — `AnalysisScope`, `AnalysisParams` (defaults: Staged, focus {duplicates,naming,metrics}, 0.8, 20), `ChangedFile`, `SymbolInfo`, `DuplicateFinding`, `NamingFinding`, `MetricsFinding`, `MetricsThresholds` (complexity 10, long_function 100, deep_nesting 4, growth 50), `AnalysisReport`, `AntiPattern*`.
- `src/git/types.cpp` — `get_language_from_path`, `detect_case_style`, `is_valid_case_style`, `calculate_risk_score`, `determine_*_severity`, `generate_top_recommendation`.
- `include/lci/git/provider.h`, `src/git/provider.cpp` — `Provider`: `create` (rev-parse --show-toplevel), `tracks_any`, `get_changed_files` (`diff --name-status --no-renames -z`, `diff-tree --root` for commits), `get_changed_scope` (-U0 hunks -> `ScopeSet`), `get_diff_stats`/`parse_numstat`, `get_file_content` (WORKING/STAGED/ref via `show`), `get_base_ref`/`get_parent_commit` (object-format-aware empty tree for root commits), `run_git` (argv exec through `subprocess::run_capture`, `include/lci/core/subprocess.h:14`). Free parsers `parse_name_status`, `parse_status`, `is_safe_ref`.
- `include/lci/git/analyzer.h`, `src/git/analyzer.cpp` — `Analyzer::analyze` -> `parse_changed_files` (UnifiedExtractor over changed files, intersected with hunk `ScopeSet`; Added files whole) -> `get_existing_symbols` (index snapshot, bodies only when duplicates in focus) -> `find_duplicates` (normalize once, exact-hash classes, set-size-ratio bound then `token_set_similarity` Jaccard) -> `check_naming` (`naming_findings_from_report` filters the report-side `NamingAnalyzer` signals to changed symbols; `check_case_style`) -> `check_metrics` (complexity, length, nesting, growth vs existing same-name symbol) -> `build_report`. `report_to_json` and `normalize_rel` live in `src/git/serialize.cpp`.
- `include/lci/git/frequency_analyzer.h`, `src/git/frequency_analyzer.cpp` — `HistoryProvider::get_commit_history` (`git log --numstat -z --format=%H%x00%an%x00%ae%x00%at%x00%s --since=<UTC Z> --no-merges`), `parse_commit_history` (NUL-delimited, layout verified against git 2.43), `parse_rename_path`, `FrequencyAnalyzer::analyze` -> `aggregate_by_file` -> `find_hotspots` / `find_collisions` / `calculate_ownership`; `should_exclude_from_churn` (default lockfile/vendor/build exclusions, include/exclude globs), `parse_time_window`, `calculate_volatility_score`, `calculate_collision_score`. `FrequencyCache` is declared and tested but never constructed by production code.
- `include/lci/git/pattern_detector.h`, `src/git/pattern_detector.cpp` — `PatternDetector` (god object, switch factory, enum aggregation, barrel file, config aggregation, registration function). No production caller; only `tests/git_test.cpp` uses it.
- `include/lci/analysis/scope_set.h` — `ScopeSet` used for hunk scoping (`scope_from_unified_diff`).
- Tests: `tests/git_test.cpp` (116 tests; suites GitTypes, GitResults, PatternDetector, GitProvider, GitProviderParse, GitAnalyzer, GitAnalysis, GitFrequency, FrequencyTypes, ChurnFilter, CommitHistoryParse, ChangeFrequencyParams, FrequencyCache, HistoryProvider, FrequencyAnalyzer, GitReportToJson); `tests/mcp_handlers_analysis_test.cpp:1159` `CodeInsightGitTest` (real temp git repo, staged 130-line function, 3-commit churn file); `tests/mcp_server_test.cpp:463`, `:862`; `tests/server_test.cpp:1287`; `tests/integration/mcp_tools_integration_test.cpp:301-332`.
- Integration specs and goldens: `tests/integration/cli/git/git-analyze.spec.json` (materializes `tests/integration/cli/git/fixture/` into a fresh repo so `wip` is deterministically empty) -> `tests/integration/goldens/cli/git/git-analyze.json`; `tests/integration/http/git-analyze.spec.json` -> `tests/integration/goldens/http/git-analyze.json` (stable tier empty, everything ignored); `tests/integration/mcp/git_analysis/invalid-scope.spec.json` -> `tests/integration/goldens/mcp/git_analysis/invalid-scope.json`. `tests/integration/mcp/KNOWN_DIVERGENCE.md:28` lists `analyzed_at`/`analysis_time_ms` as envelope-only.
- Benchmarks: `benchmarks/repo-qa/comprehension/surface/tool-surface.json:518` (schema snapshot, asserted by `benchmarks/repo-qa/tests/test_tool_surface.py`); `benchmarks/repo-qa/ANALYSIS-comprehension-surface.md` (git_analysis row is an idealized arm); `benchmarks/repo-qa/ANALYSIS-toolcalling-descriptions.md:69` (pre-commit-change-quality selection 6/6).
- Docs: `docs/TOOLS.md:493` (`git_analysis`), `docs/TOOLS.md:436-441,473-480` (code_insight git modes), `docs/src/content/docs/http-socket-api.mdx:45`, `docs/src/content/docs/mcp-server.mdx:49`, `docs/plans/2026-08-28-git-analyze-audit.md` (audit verdict and fixed defects), `docs/reviews/2026-09-03-review.md:229` (S10 git parsing findings, all landed per `git log -- src/git`).

## Config knobs

There are no git-specific `.lci.kdl` keys and `src/git/*` reads no environment variables (`grep getenv src/git` is empty). What matters:

- `project.root` (`include/lci/config.h`, `Config::project.root`) is the path handed to `Provider::create`; git resolves upward to the toplevel, and `tracks_any(root)` decides whether a nested root is analyzable.
- Per-request params only: `AnalysisParams` (`include/lci/git/types.h:37`) and `ChangeFrequencyParams` (`include/lci/git/frequency_analyzer.h:175`; `min_changes` 2, `min_contributors` 2, `top_n` 50, include/exclude patterns, `skip_default_exclusions`). Only `time_window` and `file_pattern` are reachable from any surface.
- `MetricsThresholds` (`include/lci/git/types.h:287`) are compile-time defaults; no surface overrides them.
- Host `git` binary on PATH is a hard dependency; git 2.43 is the version the `-z` parsers were captured against.

## Invariants and traps

- Argv exec stops shell injection, not git option injection: every user ref is screened with `is_safe_ref` and every pathspec terminated with `--` at the call site (`src/git/provider.cpp:46` comment; history in memory `run-git-shell-injection`: the old popen concat broke `%H|%an` and was fixed in cd6b21c, then replaced by `subprocess::run_capture`).
- `git log -z` and `diff --name-status -z` parsers are pinned to bytes captured from git 2.43, including the leading `\n` before the first numstat entry and rename entries as empty path + old NUL new NUL (`src/git/frequency_analyzer.cpp:523`; `.claude/rules/bench-harness-oracle-independence.md` rule 9a). Re-capture before trusting a hand-imagined layout.
- `--since` must carry a trailing `Z`; git parses zone-less ISO dates in local time (`src/git/frequency_analyzer.cpp:436`; commit e5fe825; test `GitFrequency.SinceIsInterpretedAsUtcRegardlessOfLocalTz`).
- Root commits: `diff-tree --root` and an object-format-aware empty tree hash; a shallow clone lacking parents makes `commit`/`range` fail with a message that names the refs (`src/mcp/handlers_analysis.cpp:740-748`; memory `insight-verification-2026-08-16`, "git_analysis false zeros"; commits 93e8813, e5fe825).
- Untracked root nested in an outer repo is refused (`available=false`), tracked monorepo subdirs pass (`tracks_any`, `src/mcp/handlers_index.cpp:394`; commit 83e5569). Same gate in code_insight and HTTP.
- Non-git root is an absent precondition, not an error: JSON `{available:false, reason, hint}` or LCF `== GIT ==` block, HTTP 200 (`docs/TOOLS.md:572`). Never fake zeros (`.claude/rules/karpathy-principles.md` rule 6).
- Findings are hunk-scoped: only symbols intersecting new-side `-U0` hunks count; a failed hunk fetch falls back to unscoped (over-report, never drop). Residue: the index reflects the working tree, so a historic commit can "duplicate" its own shifted copy (`docs/plans/2026-08-28-git-analyze-audit.md`, defect 1 update).
- Every sort feeding `resize(max_findings)` has a total-order tiebreak; hotspots output is byte-identical across processes (commits b4953c0, e1cf78b; `GitFrequency.HotspotsReportByteIdenticalAcrossProcesses`). Hash-order RED tests must fork (`.claude/rules/karpathy-principles.md` rule 4).
- Default scope `staged` on a dirty-but-unstaged tree reports 0 files; the CLI prints a note pointing at `-s wip` (`src/cli/commands_query.cpp:104`). MCP/HTTP print no such note.
- `code_insight git_analyze` and `git_analysis` run the same `git::Analyzer`; a bench answer key claiming one "has no notion of what changed" is false. Record it as surface redundancy (`.claude/rules/bench-harness-oracle-independence.md` rule 10a).
- `WORKING` ref file reads are confined to the repository (commit 7657a87; `GitProvider.GetFileContentWorkingRefusesPathTraversal`).
- Diff paths are read prefix-free and `core.quotePath`-unquoted so `diff.mnemonicPrefix` or non-ASCII names still scope symbols (commit 76b8eb3; `GitAnalysis.MnemonicPrefixAndQuotedPathStillScopesChangedSymbols`).
- `file_pattern` reaches git as one native pathspec, not an `ls-files` expansion (commit 0b91c7d).
- Half-built or dead, verified 2026-09-08: `PatternDetector` has no production caller (`lci refs PatternDetector` outside tests is empty); `ChangeFrequencySummary::anti_patterns_found` has no writer (`grep anti_patterns_found src/git` hits only the header) yet `emit_git_hotspots` prints `anti_patterns=` and `docs/TOOLS.md:477` advertises it, so it is always 0; `FrequencyCache`, `FrequencyAnalyzer::analyze_file`, `get_collision_risk` have no callers (the audit's "no caching" nit is true because the cache is unwired); the CLI text renderer prints no metrics issues section although JSON carries `metrics_issues` and `risk_score` counts them, and `-f` help lists only "duplicates, naming".
- Integration coverage is envelope-level: the CLI golden is an empty-diff report, the HTTP spec's stable tier is empty, and the MCP golden is the invalid-scope error. Real-finding assertions live only in `CodeInsightGitTest` and unit tests.
- Dogfood gap: `lci refs` reports "lexical-only matches" for symbols referenced only from comments, which is correct but means a dead-code check still needs `grep -rn` over `src/` to confirm zero callers.

## Probe recipes

Binary: `build/release/src/lci`. Root flag `-r <root>`.

```
# CLI, text and JSON (spawns or reuses the server for the root)
build/release/src/lci git-analyze -s range -b HEAD~3 -r .
build/release/src/lci git-analyze -s wip -j -r .
build/release/src/lci git-analyze -s commit -b HEAD -f duplicates --threshold 0.7 -m 5 -j -r .

# MCP over stdio (newline-delimited JSON-RPC)
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{}}}' '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"git_analysis","arguments":{"scope":"range","base_ref":"HEAD~3"}}}' | build/release/src/lci mcp -r .
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{}}}' '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"code_insight","arguments":{"mode":"git_hotspots","time_window":"90d"}}}' | build/release/src/lci mcp -r .

# Raw git the provider issues (compare parser input by hand)
git log --numstat -z --format=%H%x00%an%x00%ae%x00%at%x00%s --since=2026-08-01T00:00:00Z --no-merges | head -c 600 | od -c | head
git diff --cached --name-status --no-renames -z

# Unit tests (build only lci_tests; ccache is wired)
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='Git*:Frequency*:ChurnFilter.*:CommitHistoryParse.*:ChangeFrequencyParams.*:HistoryProvider.*:PatternDetector.*:CodeInsightGitTest.*'
ctest --test-dir build/release -R 'Git|Frequency|CodeInsightGit' -j4

# Integration goldens (all git specs run inside this one ctest entry, ~53 s)
ctest --test-dir build/release -R lci_integration_suite --output-on-failure
# Golden files: tests/integration/goldens/{cli/git,http,mcp/git_analysis}/

# Schema snapshot gate after any add_tool edit (rule 7 of test-iteration-discipline)
/usr/bin/python3 -m pytest benchmarks/repo-qa/tests/test_tool_surface.py
```

## Product comparison

Comparable tools: CodeScene (hotspots, code health, churn x complexity), code-maat (Adam Tornhill's git log analysis: churn, coupling, ownership), git-of-theseus (code age/survival plots), hercules (Go; burndown, couples, devs, sentiment), gitinspector (per-author stats), GitHub Insights (commit/contributor graphs), Sourcegraph Insights (search-based trend charts), SonarQube new-code quality gates, jscpd/PMD CPD (duplicate detection), Danger/reviewdog (pre-commit/PR annotation). None of these is verified here; treat their capability descriptions below as `(unverified, from model knowledge)`.

lci's claims in this area:

| Claim | Where made | How to measure | Existing numbers |
|---|---|---|---|
| "analyze working-set changes for duplicates, naming, complexity" | `README.md:105`, `docs/src/content/docs/index.mdx:76` | Run the CLI on a commit with a known copy-paste (audit used `is_function_like` duplicated across four analyzers); count true/false findings by hand | `docs/plans/2026-08-28-git-analyze-audit.md`: probe commit f7180a0 findings 27 -> 2 after hunk scoping, analysis 42 s -> 7.6 s -> 0.55 s |
| Hotspots are "working, worthwhile" | `docs/plans/2026-08-28-git-analyze-audit.md` | `code_insight mode=git_hotspots time_window=1y`, wall-clock the call; compare top-N with `git log --since -- . | sort | uniq -c` | 14 s on a 353-commit window (audit, one sample, not re-measured) |
| Git modes surface data Go computed but discarded | `docs/TOOLS.md:480` | Historical only; the Go tree is deleted (memory `go-reference-tree-deleted`), so this is unfalsifiable now | none |
| Pre-commit change-quality prompts select `git_analysis` 6/6 | `benchmarks/repo-qa/ANALYSIS-toolcalling-descriptions.md:69` | Re-run the selection bank; note native tools are the real competitor (rule 14) | 6/6 across arms |
| Comprehension gain from annotated git_analysis output | `benchmarks/repo-qa/ANALYSIS-comprehension-surface.md:37` | Idealized arm; the doc itself says it does not prove lci emits that format | +1.000, flagged as requiring capability work |

Comparison axes:

| Axis | lci | Checking the competitor | Notes |
|---|---|---|---|
| Change scoping | staged / wip / commit / range, findings intersected with `-U0` hunks | SonarQube "new code" period; CodeScene delta analysis on a PR; jscpd runs on whole files | lci's hunk scoping is element-level, not file-level |
| Duplicate detection | Jaccard over normalized token sets vs every indexed function, threshold 0.8, exact-hash classes first | jscpd / PMD CPD token windows; CodeScene code duplication metric; `code_insight analysis=clones` is lci's corpus-wide sibling | lci compares only changed symbols against the index; no cross-changed-symbol pairs unless both are indexed |
| Naming | report-side `NamingAnalyzer` signals (synonym split, ambiguous, vague, vocabulary outlier) filtered to changed symbols + case style per language | CodeScene has no naming lint; SonarQube naming rules are regex conventions | lci's signals depend on the name already existing in the index |
| Metrics on changed code | cyclomatic complexity, LOC, nesting, growth vs prior same-name symbol; fixed thresholds 10/100/4/50 | SonarQube cognitive complexity gate; CodeScene code health | thresholds not configurable from any surface |
| Churn hotspots | per-file change count, authors, lines over 7d/30d/90d/1y; default exclusions for lockfiles/vendor | code-maat `-a revisions`; CodeScene hotspots; hercules burndown | lci has no per-symbol churn surface (`FrequencyGranularity::Symbol` exists in the type but no caller sets it) |
| Churn x complexity | `== RISK MATRIX ==` normalized product with quadrant labels | CodeScene hotspot map is the reference; code-maat needs an external complexity join | lci's is inline in the MCP handler, not in `src/git` (`src/mcp/handlers_analysis.cpp:767-870`) |
| Collision / ownership | `find_collisions` (multi-author zones), `calculate_ownership` | code-maat `-a authors`, `-a entity-ownership`; gitinspector | `calculate_ownership` runs but `emit_git_hotspots` never prints ownership (`grep ownership src/mcp/insight_sections.cpp` hits only a comment); only hotspots and collisions reach the LCF |
| Temporal coupling | none | code-maat `-a coupling`, CodeScene change coupling | gap |
| Code age / survival | none | git-of-theseus, hercules burndown | gap |
| Caching / incremental | none; each hotspot call replays `git log` | CodeScene persists analyses; hercules caches | `FrequencyCache` exists, unwired |
| Delivery | CLI text/JSON, MCP JSON and LCF, HTTP JSON; no PR annotation, no charts | CodeScene/Sonar PR decorations; GitHub Insights charts | lci is agent-facing |
| Determinism | sorted with tiebreaks, byte-identical across processes (tested) | varies | pinned by test |

Honest gaps vs the field: no temporal coupling, no code age, no per-author or per-symbol churn surface, no persisted history or caching, thresholds and churn filters not user-configurable, anti-pattern detection is dead code advertised in docs, CLI text output hides metrics findings, integration goldens do not pin a single real finding, and every hotspot call pays a full `git log` over the window. Performance numbers exist only as single audit samples; nothing in `docs/performance/` or `tests/benchmarks/` times the git paths.

## Related skills

lci-feature-map (router), lci-code-insight, lci-symbol-navigation, lci-search, lci-get-context, lci-parsing-languages, lci-side-effects, lci-indexing-pipeline, lci-server-lifecycle, lci-mcp-server, lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation
