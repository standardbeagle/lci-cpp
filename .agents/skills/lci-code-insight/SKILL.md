---
name: lci-code-insight
description: "Use when: touching the code_insight MCP tool or any src/analysis analyzer (health, layers, modules, features, coupling, naming, clones, entry points, error-handling BETA, dead code, security, impact); debugging a reach/cycle/layer artifact; re-pinning code_insight goldens; comparing lci with SonarQube, CodeScene, CodeQL, lizard, madge."
---

# LCI Code Insight

`code_insight` is the MCP-only codebase intelligence report an agent calls at session start. One call over the in-memory index emits LCF text: summary, repository map, entry points, health, vocabulary, modules, dependencies, graph signals (load-bearing symbols, cycles, clusters, layer violations), statistics, and git change/hotspot analysis. Detailed sub-analyses add clones, dead code, security sinks, change impact, an @lci: annotation worklist, and a BETA error-handling/resource-management score. There is no CLI verb and no HTTP route for it; the only transport is `lci mcp`.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| MCP tool `code_insight` (schema + description) | `src/mcp/handlers_analysis.cpp:2317` | Registered by `register_analysis_handlers` (`:2242`), called from `src/mcp/runtime.cpp:97`. Params: mode, attributes, analysis, min_lines, threshold, target, flow, max_results, scope, base_ref, target_ref, time_window, file_pattern. |
| Handler dispatch | `src/mcp/handlers_analysis.cpp:321` `handle_code_insight` | mode default `overview` (`:328`); branches statistics `:444`, structure `:470`, unified `:509`, git_analyze/git_hotspots `:689`, detailed `:844`. |
| `analysis=` sub-modes | `src/mcp/handlers_analysis.cpp:852` | modules, layers, features, terms, errors, resources, clones, deadcode, security, impact, annotate. |
| `attributes=` scope | `src/mcp/handlers_analysis.cpp:171` | `shipping` (default: every attribute activating the Analysis gate), `all`, one name, or a list. Gate model: `include/lci/path_classifier.h:8-30`. |
| `.lci.kdl` `insight { error_report; entry_points }` | `src/config/config.cpp:335` `apply_insight` | Struct `InsightConfig` at `include/lci/config.h:154`. |
| Env `LCI_ERROR_REPORT` | `src/config/config.cpp:865` | off/capture/on; beats the file. |
| Error-report capture on `lci mcp` exit | `src/cli/mcp.cpp:237`, `write_error_report_capture` at `src/mcp/handlers_analysis.cpp:2380` | Writes `$XDG_STATE_HOME/lci/error-reports/<root-slug>.txt` when `error_report=capture`. |
| Framework entry registry | `share/lci/framework-entry-signatures.json` | Read by `src/analysis/entry_signatures.cpp`; matched via go.mod / composer.json / package.json identity. |
| Finding suppressions in source | `include/lci/analysis/finding_suppressions.h` | `lci-disable-next-line <rule>`, `lci-disable-line`, `lci-disable ... lci-enable`. |
| `info code_insight` | `tests/integration/goldens/mcp/info/basic.json` | Help derived from the registered ToolDefinition. |

## Code map

Entry (MCP) -> core analyzers -> index snapshots. Everything reads `MasterIndex` RCU snapshots; the engine caches results, so the tool keeps the exclusive lock (comment at `handlers_analysis.cpp:2372`).

- `src/mcp/handlers_analysis.cpp` (2400 lines) — `handle_code_insight`, `GatheredCorpus` (attribute-scoped file gathering, `:119`), `SameNameCallGrouping` (dynamic/unresolved call split, `:52`), the clones/impact/security/annotate/deadcode/errors branches inline, `register_analysis_handlers`, `write_error_report_capture`.
- `src/mcp/insight_sections.cpp` + `include/lci/mcp/insight_sections.h` — LCF emitters: `emit_lcf_header`, `emit_repository_map`, `emit_health`, `emit_modules`, `emit_statistics`, `emit_git_changes`, `emit_git_hotspots`, `emit_error_handling`, `emit_resource_management`, `emit_vocabulary`, `emit_summary`, `emit_entry_points`, `emit_next_steps`, `finalize_lcf`.
- `src/mcp/insight_graph.cpp` — `compute_graph_signals` (one `analysis::CallGraph` build yields load-bearing reach, cycles, clusters; header `:71-80`), `emit_load_bearing`, `emit_cycles`, `emit_layer_violations`, `emit_clusters`, `compute_import_dependencies` + `emit_dependencies` (import graph, Tarjan import cycles `:611`), `is_middleware_chain_call`.
- `src/analysis/codebase_intelligence.cpp` + `include/lci/analysis/codebase_intelligence.h` — `CodebaseIntelligenceEngine` (`analyze`, `build_overview`, `build_detailed`, `build_statistics`, `build_unified`, `build_structure`, `build_entry_points`, `is_valid_mode`, `calculate_importance_score`). Types in `include/lci/analysis/codebase_intelligence_types.h` (`HealthDashboard`, `ModuleAnalysis`, `LayerAnalysis`, `FeatureAnalysis`, `EntryPointsList`, `ErrorHandlingSummary`, `ResourceSummary`, `StatisticsReport`, `StructureAnalysis`).
- `src/analysis/call_graph.cpp` — `CallGraph`: iterative `tarjan_scc` (`:50`), Brandes betweenness (`:191`, double sigma), `louvain_communities` (`:390`). Uniform edge weight 1.0 by design.
- `src/analysis/health_analyzer.cpp` — `HealthAnalyzer`: complexity, hotspots, smells, `high-fan-in`; score formula `:279-335` (10.0 minus ratio deductions, no bonus).
- `src/analysis/module_analyzer.cpp` — `ModuleAnalyzer::analyze_graph`: Louvain communities as modules, directories as labels, misplaced/entangled/extension findings, `basis=dirs_vs_call_graph_communities`.
- `src/analysis/layer_analyzer.cpp` — name/path inference of Presentation/Application/Domain/Data layers (`layers=heuristic`).
- `src/analysis/feature_analyzer.cpp` — features = Louvain communities (`:107-139`); singletons reported as `orphan_components`.
- `src/analysis/coupling_analyzer.cpp` — package-level Ca/Ce, instability, cohesion.
- `src/analysis/naming_analyzer.cpp` — `VocabularyOutlier`, `AliasUsage`, `AmbiguousName`, `VagueName`, `NameInformation`, `NameFidelity`, `SynonymSplitMember`; `edit_distance_capped` (`:131`, heap rows). Helpers: `src/analysis/ci_vocabulary_analyzer.cpp` (domain categories), `src/analysis/english_words.cpp` + `src/analysis/data/scowl_english_words.txt` (SCOWL dictionary), `include/lci/analysis/word_match.h` (whole-word keyword match).
- `src/analysis/clone_detector.cpp` + `src/analysis/code_similarity.cpp` — `CloneDetector`, `normalize_code_content`, `code_structural_similarity`; structural stage caps at the 1500 largest functions.
- `src/analysis/entry_signatures.cpp` — `EntryPointHints`, 3-tier confidence (annotated / framework / heuristic).
- `src/analysis/error_handling_analyzer.cpp` — `ErrorHandlingAnalyzer::analyze` over `SideEffectAnalyzer` records; severity x confidence x fan-in rollup function -> module -> repo.
- `src/analysis/finding_suppressions.cpp`, `src/analysis/scope_set.cpp` (`ScopeSet`, `scope_from_paths`, `scope_from_unified_diff`; used by impact), `src/analysis/token_budget.cpp` (`TokenBudgetManager`).
- `include/lci/path_classifier.h` — `PathAttrRegistry`, `Capability::Analysis` gate.
- Tests: `tests/codebase_intelligence_test.cpp` (CIEngine, CIThresholds, HealthAnalyzer, TokenBudgetManager), `tests/analysis_test.cpp` (CouplingAnalyzer, EntrySignatures, FeatureAnalyzer, LayerAnalyzer, ModuleAnalyzer, NamingAnalyzer, ScopeSet, SynonymTable, EnglishWords), `tests/call_graph_test.cpp`, `tests/error_handling_analyzer_test.cpp` (EhAnalysisGateTest, EhContractTest, EhScoringTest), `tests/mcp_handlers_analysis_test.cpp` (CodeInsightTest, CodeInsightAttrTest, CodeInsightGitTest, CodeInsightGraphSignals, CodeInsightImportCycles, CodeInsightLayers, CodeInsightLoadBearing, CodeInsightEntryPoints, CodeInsightEntryPins, CodeInsightDeadCode, CodeInsightAnnotate, CodeInsightDynamic, CodeInsightLabelCoherence, ErrorHandlingSectionTest, SameNameCallGrouping), `tests/integration/real_project_analysis_test.cpp` (RealProjectCodeInsightTest, overview < 1000 ms at `:235`).
- Goldens: `tests/integration/mcp/code_insight/basic.spec.json` -> `tests/integration/goldens/mcp/code_insight/basic.json`, corpus `tests/parity/corpora/synthetic/multi-lang` (4 one-function files).
- Docs: `docs/TOOLS.md:429`, `docs/src/content/docs/error-handling-score.mdx`, `docs/plans/2026-08-26-insight-report-quality-plan.md`, `docs/plans/2026-08-17-error-handling-score-design.md`, `docs/plans/2026-06-02-naming-vocabulary-signal-design.md`, `docs/plans/2026-08-27-enhanced-names-design.md`, `docs/plans/2026-08-29-code-insight-multi-repo-qa.md`, `docs/reviews/2026-09-03-review.md` (S11 analyzers).
- Benchmarks: `benchmarks/repo-qa/ANALYSIS-insight-verification.md`, `ANALYSIS-insight-reverification.md`, `ANALYSIS-insight-repanel-2026-08-26.md`.

## Config knobs

| Key | Default | Field | Effect |
|---|---|---|---|
| `insight { error_report "off|capture|on" }` | `off` | `InsightConfig::error_report` (`include/lci/config.h:155`) | Gates `== ERROR HANDLING ==`, `== RESOURCE MANAGEMENT ==`, `analysis=errors|resources`. `capture` writes the report on `lci mcp` shutdown only. |
| `LCI_ERROR_REPORT` env | unset | same field, `src/config/config.cpp:865` | Overrides the file. |
| `insight { entry_points "Name" ... }` | empty | `InsightConfig::entry_points` (`config.h:161`) | Pinned entry points, `confidence=annotated`. |
| `attributes { ... }` block | shipped rules | `PathAttrRegistry` (`include/lci/path_classifier.h`) | Which files the Analysis gate admits; extra attributes may `activates "analysis"`. |
| `@lci:entry`, `@lci:labels[...]`, `@lci:exclude[deadcode]`, `.lci/annotations/*.json` | none | `SemanticAnnotator` (`src/core/semantic_annotator.cpp`) | Annotation inputs read by entry points, deadcode flow, annotate driver. |
| Tool params `min_lines` (6), `threshold` (0.9), `max_results` (50), `time_window` (30d), `scope` (staged) | per schema | `handlers_analysis.cpp:2324-2361` | Per-call. |

## Invariants and traps

- Attribute scoping is the D1 fix: sections analyze only files whose attribute activates `Capability::Analysis`; exclusions are listed in SUMMARY, never silent (`handlers_analysis.cpp:113-119`, `path_classifier.h:21-25`). Passing `attributes=all` re-admits tests and vendored code.
- Call-edge precision is the root cause under CYCLES, LOAD BEARING and LAYER VIOLATIONS. Bare-name resolution once inflated reach (NormalizeError reach=2547 vs 6 refs); stdlib-name collisions (Kotlin `apply`, Zig `initCapacity`, C++ `size`/`find`) fabricated cycles and layer violations. Fixed in rounds: foreign receivers take no name-only match, arity-preferring resolution, decl-literal receivers. Residual: same-arity overloads, `with()`-scope shadowing, struct-field receivers, Rust std-trait delegation cycles. Sources: `docs/plans/2026-08-26-insight-report-quality-plan.md` P1; memory `insight-verification-2026-08-16`.
- Dynamic dispatch is a lower bound: `SameNameCallGrouping` and `Snapshot::classify_same_name_calls` match by NAME and over-count common names like `Close`. Use for "might be dynamic", never to hard-suppress dead code (`handlers_analysis.cpp:52-66`).
- Dead-code depth is annotation-curated, not a verdict: outside C++ it was near 100% false positives, so `analysis=deadcode flow=true` emits a worklist of `@lci:` decisions that shrinks on re-run. C-family UNUSED FILES are excluded (decl/impl split divides evidence).
- Modules come from the graph, not directories: `ModuleAnalyzer::analyze_graph` uses Louvain communities, dirs are labels, `split_dirs` is the divergence signal; the old `coupling=0.3` constant is gone (memory `modules-from-graph-not-dirs`). REPOSITORY MAP stays dir-based on purpose. The user asked for Leiden refinement over Louvain (deterministic seed, resolution parameter); not built.
- Layers are heuristic; LAYER VIOLATIONS require majority-flow evidence (downward >= 4 edges and 3:1 dominance) and open with `layers=heuristic`.
- Health score is ratio-based by design; 10.00 is reachable only with zero penalties (`health_analyzer.cpp:279-287`). Purity is deliberately excluded from the score. A "saturated" 10.00 on a tiny corpus is expected, not a defect.
- Error report is BETA and ships dark. With the gate closed, `analysis=errors` answers `available=false` with reason and hint, not an error (`handlers_analysis.cpp:1973-2001`). Findings are syntactic plus name heuristics; there is no CFG/dataflow, so leaks are "no release syntactically visible", never proven (`docs/plans/2026-08-17-error-handling-score-design.md`).
- Classification rule changes need a before/after corpus diff in BOTH directions and a RED pinning kept positives and removed false positives; word-boundary matching broke the `*Sync` family once (`.claude/rules/test-iteration-discipline.md` rule 7). Same rule for layer/module keyword tables and `word_match.h`.
- Hash-order determinism REDs must run in separate processes (abseil salt is per-process); applies to `SideEffectAnalyzer::results()` consumers (`.claude/rules/karpathy-principles.md` rule 4).
- Git modes return `available=false` on a non-git root, never fake zeros; single-commit repos use `git diff-tree --root`; a root untracked inside an outer repo is refused (`tracks_any` gate).
- Stale persistent index servers shadow new tool definitions: run `lci shutdown --all` after a rebuild before re-testing `tools/list` or new params.
- One-shot `lci mcp` on a large repo builds the index inline and may answer `index unavailable` before it is ready (`docs/plans/2026-08-29-code-insight-multi-repo-qa.md` section 1).
- The golden corpus (`multi-lang`, 4 files, 6 symbols) exercises no graph, module, vocabulary or git section; goldens cannot catch regressions there. Real-corpus tests are `SKIP_IF_NO_REAL_PROJECT`-gated (`tests/helpers/real_project_helpers.h:57-99`) and pass-by-skip in CI.
- Findings from the 2026-09-08 mapping (fixed or filed): see `docs/reviews/2026-09-08-skill-mapping-findings.md`.
- The tool holds the MCP exclusive lock for its whole run because `CodebaseIntelligenceEngine` caches results in shared mutable state (`handlers_analysis.cpp:2372-2375`); a long unified run on a big repo blocks other tool calls. No mutex is taken on index read paths (RCU snapshots).
- Sections consume each other: HEALTH purity, LOAD BEARING, clusters, vocabulary and entry points all filter through the same attribute scope and `test_scaffold` marker; changing the scope logic in `GatheredCorpus` moves every denominator at once (D4 denominator coherence, memory `insight-verification-2026-08-16`).
- Dogfood gap: locating a directory by name (`multi-lang`) needed `find`; `lci` has no CLI directory search (`find_files` is MCP-only).

## Probe recipes

Binary: `build/release/src/lci`. Corpus: `tests/parity/corpora/synthetic/multi-lang`.

```
# one-shot MCP call (no initialize needed on stdio)
printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"code_insight","arguments":{"mode":"unified"}}}' \
  | build/release/src/lci mcp -r tests/parity/corpora/synthetic/multi-lang | tail -1

# detailed sub-analyses
... "arguments":{"mode":"detailed","analysis":"clones","min_lines":4}
... "arguments":{"mode":"detailed","analysis":"deadcode","flow":true}
... "arguments":{"mode":"detailed","analysis":"impact","scope":"wip"}
... "arguments":{"mode":"detailed","analysis":"annotate","target":"entry"}
... "arguments":{"mode":"git_hotspots","time_window":"90d"}
... "arguments":{"attributes":"all"}

# BETA error report
LCI_ERROR_REPORT=on <same pipeline> ... "arguments":{"mode":"detailed","analysis":"errors"}

# self-repo run (indexes ~5k files; expect seconds, then shut the server down)
... | build/release/src/lci mcp -r .
build/release/src/lci shutdown --all
```

Tests (build only `lci_tests`):
```
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='CodeInsight*:CIEngine*:HealthAnalyzer*:*Analyzer.*:Eh*:CallGraph*:ErrorHandlingSectionTest*'
ctest --test-dir build/release -R lci_integration_suite --output-on-failure   # goldens
ctest --test-dir build/release -R lci_real_project_suite                       # needs real_projects/
```
Navigation: `build/release/src/lci def compute_graph_signals -r .`, `lci refs ErrorHandlingAnalyzer`, `lci search louvain`.

## Product comparison

Comparable products: SonarQube (rules, cognitive complexity, duplication, quality gate), CodeScene (hotspots from git churn x complexity, knowledge maps), SciTools Understand (dependency graphs, metrics, architecture views), CodeQL (dataflow queries, security sinks), lizard and radon (cyclomatic complexity, maintainability index), madge / dependency-cruiser (JS import graphs, cycles, layer rules), Structure101 (architecture layering, tangles), fallow (TS/JS codebase-intelligence: dead code, dupes, boundaries).

lci claims and how to check them:

| Claim | Where made | Measure | Existing numbers |
|---|---|---|---|
| Session-startup overview of a repo in one call | `README.md:122`, `docs/TOOLS.md:431` | run the unified probe on a real corpus; time it | `tests/integration/real_project_analysis_test.cpp:235` bound 1000 ms; multi-repo timings in `docs/plans/2026-08-29-code-insight-multi-repo-qa.md` |
| Context reduction / accuracy gain vs reading files directly | tool description `handlers_analysis.cpp:2320` | no committed measurement defined | treat any specific percentage as unbacked |
| Judgment sections certified on chi/guzzle/pocketbase (LOAD BEARING, CYCLES, HEALTH ranking) | `benchmarks/repo-qa/ANALYSIS-insight-repanel-2026-08-26.md`, memory | re-run the lci-blind explorer + judge protocol from `ANALYSIS-insight-verification.md` | 3 reports under `benchmarks/repo-qa/ANALYSIS-insight-*.md` |
| Real graph algorithms, deterministic | `call_graph.h` header, `karpathy-principles.md` rule 4 | run twice in separate processes, diff LCF | `tests/call_graph_test.cpp`, `CodeInsightGraphSignals` |

Comparison axes:

| Axis | lci | Check the competitor | Notes |
|---|---|---|---|
| Complexity metrics | cyclomatic per function from tree-sitter, 13+ languages, in-memory | `lizard <dir>`, `radon cc`, SonarQube measures API | lizard/radon are per-file CLIs with CSV/JSON; lci is LCF over MCP only |
| Duplication | `analysis=clones`, normalized-token + structural similarity, caps at 1500 largest functions | SonarQube duplication blocks, PMD CPD, fallow dupes | lci reports function-level classes, not line blocks |
| Dependency graph and cycles | import graph (Tarjan) + call graph SCCs; import cycles need an extractor per language | `madge --circular`, `dependency-cruiser --validate`, Understand dependency browser | lci: PHP via composer PSR-4, Go via go.mod, Zig @import; Zig relative imports open |
| Architecture layers | name/path heuristic + majority-flow violation gate | Structure101 declared layers, dependency-cruiser rules, ArchUnit | lci has no user-declared layer rules; competitors do (unverified, from model knowledge: dependency-cruiser rule files) |
| Modules / communities | Louvain over reference graph, dirs as labels, divergence findings | Understand architecture views, CodeScene component views | competitors are declaration-driven; lci infers |
| Hotspots | `git_hotspots` churn over a window + `LOAD BEARING` reach + Brandes brokers | CodeScene hotspots (churn x complexity) | lci does not multiply churn by complexity in one score |
| Security sinks | `analysis=security`: spelling table of exec/eval/deser ranked by entry-reach depth | CodeQL taint queries, semgrep rules | lci has no dataflow; reachability-unknown labeled on zero-entry sets |
| Error handling | BETA syntactic findings (empty-catch, log-and-swallow, ...) with confidence tiers | SonarQube rules, CodeQL | lci refuses leak-freedom claims (no CFG) |
| Dead code | annotation-curated worklist, not a verdict | fallow, ts-prune, knip, CodeQL unused | dynamic-dispatch tail is the limit everywhere |
| Naming quality | vocabulary outliers, aliases, misspellings against SCOWL, synonym splits | no direct equivalent in the listed tools | unique axis; FPs on acronyms/flag names tracked |
| Output | LCF text, token-budgeted, agent-oriented | JSON/HTML/SARIF dashboards | lci has no JSON emitter for this tool (err-lookup integration blocked on it) |

Honest gaps vs the field: no persisted reports or trend history (index is in-memory; see README); no user-declared architecture rules; no dataflow or taint analysis; complexity is cyclomatic only (no cognitive complexity); clone detection is function-granular; `code_insight` is MCP-only with LCF output and no CLI or JSON form; Louvain without Leiden refinement can emit disconnected communities; recall of call edges is a lower bound under dynamic dispatch (`unresolved_same_name_calls` is surfaced, not resolved).

## Related skills

lci-feature-map (router), lci-search, lci-symbol-navigation, lci-get-context, lci-parsing-languages, lci-side-effects, lci-git-analysis, lci-indexing-pipeline, lci-server-lifecycle, lci-mcp-server, lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation
