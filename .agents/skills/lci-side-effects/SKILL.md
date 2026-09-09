---
name: lci-side-effects
description: "Use when: working on function purity / side-effect analysis, the `side_effects` or `semantic_annotations` MCP tools, `@lci:` labels, callee-category classification (io/network/db/throw), transitive impurity propagation, or the `purity` block get_context emits; comparing lci purity analysis to Infer, CodeQL, Semgrep, Frama-C or effect systems."
---

# lci side effects, purity and semantic annotations

lci classifies every indexed function/method as pure or impure from two sources: AST facts recorded at extraction (writes to params/receivers/globals/closures, throws, channel ops, catch sites) and a conservative callee-name heuristic (io/network/db/throw/dynamic-call keywords). Impurity propagates upstream through the call graph with a fixpoint and confidence decay; a caller of an impure function reads impure with a provenance reason. The same per-function record carries error-handling and resource findings (beta, dark by default). `semantic_annotations` is the companion: author-written `@lci:` comment labels + JSON manifests, propagated across the same call graph.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| MCP `side_effects` (schema + registration) | `src/mcp/handlers_analysis.cpp:2279` | modes `symbol`, `file`, `pure`, `impure`, `category`, `summary`; default `summary` |
| MCP `side_effects` dispatch | `src/mcp/handlers_side_effects.cpp:742` | `handle_side_effects`; per-mode fns at `:414` symbol, `:521` file, `:572` pure/impure, `:610` category, `:664` summary |
| MCP `semantic_annotations` (schema + registration) | `src/mcp/handlers_analysis.cpp:2249` | `label` or `category` required; `min_strength`, `include_direct`, `include_propagated`, `max_results` |
| MCP `semantic_annotations` handler | `src/mcp/handlers_side_effects.cpp:240` | `handle_semantic_annotations`; propagated labels need a `GraphPropagator` and an index |
| `purity` block in `get_context` | `src/mcp/handlers_get_context.cpp:229` | `attach_purity`, Function/Method only; JSON shape at `:211` `purity_to_json` |
| `code_insight` HEALTH `purity:` lines | `src/mcp/insight_sections.cpp:142` | `total/pure/impure`, `effects:` line, and a canned `side_effects {"mode":"impure"}` query hint |
| `side_effects` summary `error_handling` + `resources` rollups | `src/mcp/handlers_side_effects.cpp:728` | only when `insight.error_report = "on"` |
| `info` tool listing | `src/mcp/handlers_core.cpp:169` and `:172` | one-line entries; tool descriptions say "See 'info side_effects'" |
| Runtime warmup (populate, propagate, seed) | `src/mcp/runtime.cpp:16` | `McpRuntime::warmup` runs the whole Phase 1b + Phase 2 + propagator seeding |
| Sink wiring, stdio MCP | `src/cli/mcp.cpp:122` | `set_side_effect_sink` must precede `index_directory` |
| Sink wiring, HTTP/socket server | `src/cli/server.cpp:355` | same rule; the socket server also serves MCP |
| Config `insight { error_report "on" }` | `include/lci/config.h:155` | parsed at `src/config/config.cpp:338` |

No CLI verb and no HTTP route exposes purity directly; the CLI reaches it only through `lci mcp`. Search filters do not read purity either.

## Code map

Entry to storage, in call order.

- `src/parser/unified_extractor.cpp:391` — `begin_function` opened on the OUTERMOST tracked function only (`se_func_depth_`); nested functions and closures fold into the enclosing context. `:427` calls `process_side_effect_node` on every node.
- `src/parser/unified_extractor_side_effects.cpp` — the AST feed. `record_lvalue_write` (`:34`), `register_function_signature` (`:79`, parameters + receiver), `process_side_effect_node` (`:176`, local-declaration registration per language, assignments, calls, throws, Go channel/defer, Zig/Kotlin/Rust/PHP special cases), `is_se_guard_node` (`:542`, defer/finally/using/with scopes), `is_se_branch_node` (`:562`, mutually exclusive arms for the torn-write rule).
- `src/parser/unified_extractor_catch_sites.cpp` — catch/except/rescue facts into `CatchSiteInfo`.
- `include/lci/side_effects.h` — the vocabulary: `side_effect::k*` bitfield (16 flags, `kWriteMask`/`kIOMask`/`kUncertaintyMask`), `PurityLevel` 1..5, `PurityConfidence`, `AccessPattern`, `PatternViolation`, `EhSignal`, `CatchSiteInfo`, `ResourceOp`, `WorkOp`, `SideEffectInfo` (per-function record: `categories`, `transitive_categories`, `is_pure`, `purity_score`, `impurity_reasons`, `error_findings`, `resource_findings`), `categories_to_strings`.
- `include/lci/analysis/side_effect_analyzer.h` — `SideEffectAnalyzer` (Phase 1 lifecycle `begin_function`/`record_*`/`end_function`, `populate_from_index`, `propagate_transitive`, `fixpoint_truncated`, `take_results`/`merge_results` for the per-worker drain), `SideEffectAnalyzerConfig` (hard-coded defaults, no config reader), free classifiers.
- `src/analysis/side_effect_analyzer.cpp` — `make_result_key` (`:88`, `file:line:column`), `end_function` (`:112`, classifies unresolved callees itself so kIO does not depend on the merge path), `populate_from_index` (`:463`, Phase 1b heuristic over `ref.get_callee_names`, OR-s into AST records, skips `declaration_only`), `categories_to_propagate` (`:524`), `propagate_transitive` (`:537`, sorted-symbol fixpoint, decay 0.95 floor 0.3, `kMaxIterations=100`, reasons "calls impure X (cats)"), `classify_callee_category` (`:659`, word-boundary matcher + decoration-suffix stem rule + keyword tables).
- `src/analysis/side_effect_classifiers.cpp` — `classify_resource_callee` (`:25`), `classify_work_callee` (`:123`), `classify_work_pairing` (`:194`, undo-cost: uncompensated transaction, torn write, irreversible-before-fallible), `name_promises_a_sentinel` (`:374`), `is_cleanup_method` (`:413`), `classify_catch_site` (`:446`), `classify_resource_pairing` (`:581`).
- `src/analysis/error_handling_analyzer.cpp` — repo rollup of the per-function findings (`ErrorHandlingAnalyzer::analyze`), consumed by both `side_effects` summary and `code_insight`.
- `src/indexing/pipeline_processor.cpp:229` — per-worker `SideEffectAnalyzer("generic")`, drained per file into the runtime analyzer under `side_effect_mu_`. Plumbed `MasterIndex::set_side_effect_sink` (`include/lci/indexing/master_index.h:222`) -> `Pipeline` -> `FileProcessor`.
- `include/lci/mcp/runtime.h:29` — `McpRuntime::side_effects` is the single shared analyzer; `annotator`, `propagator`, `ci_engine` live beside it.
- `src/core/semantic_annotator.cpp` — `make_patterns` (`:78`, RE2 for `@lci:labels[..]`, `category`, `tags`, `deps`, `provides`, `metrics`, `attr`, `exclude`, `loop-weight`, `loop-bounded`, `call-frequency`, `propagation-weight`), `extract_annotations` (`:97`), `populate_from_index` (`:202`), `load_manifest` (`:236`), `load_project_manifest` (`:377`; `.lci/annotations/*.json`, `.lci/annotations.json`, `.lci-annotations.json`, `lci-annotations.json`). Header `include/lci/core/semantic_annotator.h`.
- `src/core/graph_propagator.cpp` / `include/lci/core/graph_propagator.h` — `GraphPropagator` (`seed_label`, `propagate`, `get_labels`), decay 0.8 default; seeded in warmup with `impure` (strength 1.0) and every direct `@lci:` label.
- `src/mcp/handlers_side_effects.cpp` — `category_name_to_bit` (`:31`, accepts all 16 names plus aliases), `side_effect_to_json` (`:66`), `sorted_results` (`:220`, sorts by (path, line) before `max_results` truncation), the mode functions, `handle_semantic_annotations`.
- `src/mcp/handlers_get_context.cpp:211` — `purity_to_json`: `is_pure`, `purity_score`, `confidence`, `local_effects[]`, `transitive_effects[]`, `reasons[]`.
- `src/analysis/entry_signatures.cpp`, `src/core/context_lookup_semantic.cpp` — read the same analyzer for the `semantic`/`ai` get_context sections (`side_effects_ready` flag in `include/lci/core/context_lookup_types.h:415`).

Tests.

- `tests/side_effects_test.cpp` — bitfield/enum vocabulary (`SideEffectCategoryTest`, `PurityLevelTest`, ...).
- `tests/side_effect_analyzer_test.cpp` — `SideEffectAnalyzerTest`, `ClassifyAccessSequence`, `ComputePurityLevel`, `CalleeWordBoundary` (both-direction pins for the boundary + stem rule), `TransitivePropagation` (sorted fixpoint, truncation reported, confidence floor), `GraphPropagatorTest`, `SemanticAnnotatorTest` (inline labels, manifests, excludes, memory hints), `ResourceCalleeTest`, `CatchClassifyTest`, `ResourcePairingTest`.
- `tests/side_effect_extraction_test.cpp` — 166 `SideEffectExtraction` cases: verbatim per-language fixtures pinning what the AST feed records (locals vs globals, loop/catch bindings, Zig writes, Kotlin call_suffix, PHP calls, sentinel names, cleanup methods).
- `tests/mcp_handlers_analysis_test.cpp` — handler-level side_effects/semantic_annotations tests, `ErrorHandlingSectionTest.PurityCountsOnlyAnalysisScope`.
- `tests/mcp_handlers_core_test.cpp:464` — `GetContextPurityTest`.
- `tests/integration/mcp/side_effects/basic.spec.json`, `tests/integration/mcp/semantic_annotations/basic.spec.json` — specs; goldens `tests/integration/goldens/mcp/side_effects/basic.json`, `tests/integration/goldens/mcp/semantic_annotations/basic.json`; purity also pinned in `tests/integration/goldens/mcp/get_context/basic.json`, `tests/integration/goldens/mcp/get_context/semantic_ai.json`, `tests/integration/goldens/mcp/code_insight/basic.json`.
- `tests/integration/real_project_side_effects_test.cpp` — `RealProjectSideEffectsTest` (chi, fastapi; skips without corpora).
- `tests/integration/real_project_feature_audit_test.cpp:405` — `FeatureAudit.ChiCodeInsightUnifiedReportsPurity`.
- `tests/fuzz/fuzz_mcp_dispatch.cpp` — fuzzes both tools' dispatch.

Docs and benchmarks.

- `docs/TOOLS.md:336` (`semantic_annotations`), `:391` (`side_effects`); mirrored at `docs/src/content/docs/mcp-server.mdx`.
- `docs/src/content/docs/error-handling-score.mdx` — the beta finding vocabulary, suppression directives, and the `side_effects` JSON twin (`:589`).
- `docs/plans/2026-08-17-error-handling-score-design.md`, `docs/plans/2026-08-30-cold-start-profiling.md` (why the AST pass moved into the index pipeline), `docs/reviews/2026-09-03-review.md:190` (S6) and `:246` (S11).
- `benchmarks/repo-qa/ANALYSIS-toolcalling-descriptions.md:76` — selection of `side_effects` for "what-does-this-routine-touch"; `benchmarks/repo-qa/ANALYSIS-comprehension-surface.md:27` — comprehension arm; `benchmarks/repo-qa/toolcalling/tasks/what-does-this-routine-touch.json`; live schema snapshot `benchmarks/repo-qa/comprehension/surface/tool-surface.json`.

## Config knobs

| Knob | Default | Where | Effect |
|---|---|---|---|
| `.lci.kdl` `insight { error_report "on" }` | `"off"` | `InsightConfig::error_report`, `include/lci/config.h:155` | adds `error_handling` and `resources` rollups to `side_effects {"mode":"summary"}` and the code_insight sections |
| `SideEffectAnalyzerConfig` (`trust_annotations`, `strict_mode`, `track_field_access`, `max_accesses_per_function=1000`) | compiled constants | `include/lci/analysis/side_effect_analyzer.h:16` | no `.lci.kdl` reader exists; `max_accesses_per_function` caps `record_access` |
| Annotation manifests | none | `SemanticAnnotator::load_project_manifest` | the four manifest paths listed in Code map |
| `# lci-disable-next-line <signal>` and file-level directives | none | `include/lci/analysis/finding_suppressions.h` | silence error/resource findings; count reported as `suppressed_findings` |

No environment variable affects this area. Language is always `"generic"` for the shared analyzer.

## Invariants and traps

- Conservative by design: "if we say it is pure, it IS pure" (`include/lci/analysis/side_effect_analyzer.h` class comment). Precision over recall everywhere in the classifiers (`side_effect_classifiers.cpp` comments on `classify_work_pairing`).
- Classification RULE changes need a before/after corpus diff, both directions, and a RED that pins kept positives and removed false positives together. `.claude/rules/test-iteration-discipline.md` rule 7 ("A change to a classification or matching RULE ..."). History: `85260f0` word-boundary rule silently flipped `readFileSync` (306 next.js sites) to pure; `052fc16` stem rule readmitted `<x>RequestContext` -> network. The corpus-diff replica is still not a committed script.
- `SideEffectAnalyzer::results()` is a salted `flat_hash_map`. Any emit that walks it must sort first; hash-order determinism tests must fork separate processes. `.claude/rules/karpathy-principles.md` rule 4. `sorted_results` (`handlers_side_effects.cpp:220`, commit `78dc364`) and the sorted fixpoint (`6c32c6b`) exist because of this.
- Empty analyzer is `error: analysis_unavailable`, never 100% pure (`7c66f2b`; karpathy rule 6). `summary` counts only `results()`, so unanalyzed languages simply do not appear.
- Sink must be attached BEFORE `index_directory` (`src/mcp/runtime.cpp:37`); otherwise the AST records are absent and only the heuristic pass fills functions, at lower fidelity and with no access patterns.
- Result key is `file:line:column`, but the extractor never passes a column (`unified_extractor.cpp:391` calls the 4-arg `begin_function`) and `populate_from_index`/`propagate_transitive` look up column 0 (`side_effect_analyzer.cpp:463` comment). Two functions on one line still collide in production; the column mechanism is half-wired.
- Closures fold into the enclosing function (`unified_extractor.cpp:385`). Go closure effects are therefore not attributed to the closure and can be missed for the outer function too: pocketbase `Bootstrap` reported `is_pure:true` with a write-only access pattern (memory `insight-verification-2026-08-16`, "Go closure purity FN"). Known gap, open.
- Watch-mode / incremental reindex does not refresh side-effect records; functions deleted from a file leave stale keys until the next bulk index (`docs/plans/2026-08-30-cold-start-profiling.md:40`; `merge_results` comment).
- Propagator seeding matches impure functions by `function_name` + `start_line` (`runtime.cpp:60`), not by id; anonymous functions seed nothing.
- `categories_to_propagate` deliberately drops closure/field writes, async and reflection from upstream propagation (`side_effect_analyzer.cpp:524`). `kFieldWrite`, `kAsync`, `kReflection`, `kIndirectWrite` have no producer in the extractor today (grep `kFieldWrite` under `src/`): the category names are accepted by `category` mode and always return empty.
- `propagate_transitive` caps at 100 iterations and reports `fixpoint_truncated()`, but no handler reads that flag; a truncated fixpoint is still emitted silently at the MCP surface.
- `SymbolInfo.is_pure`/`side_effects` in the git analyzer are never filled, so its PurityLost check is dead (`docs/reviews/2026-09-03-review.md:241`). Open.
- Doc drift: the empty-result hint says `@lci:label=...` (`handlers_side_effects.cpp`, visible in `goldens/mcp/semantic_annotations/basic.json`) but the parser only accepts `@lci:labels[...]` (`semantic_annotator.cpp:80`). The `category` error message lists 7 categories; `category_name_to_bit` accepts 16 and `docs/TOOLS.md` lists them all.
- A `semantic_annotations` label query returns propagated hits only when both a propagator and an index are wired; category queries are direct-only (propagator propagates labels, not categories).
- Tool descriptions and schemas are pinned in `benchmarks/repo-qa/comprehension/surface/tool-surface.json` and asserted by pytest, not ctest. Editing the `add_tool` block at `handlers_analysis.cpp:2249`/`:2279` requires `pytest benchmarks/repo-qa/tests/test_tool_surface.py` (`.claude/rules/test-iteration-discipline.md` rule 7, "A slice that edits an MCP tool schema ...").
- Dogfood: `lci def classify_callee_category -r <repo>` finds the anonymous-namespace definition; `lci refs populate_from_index` lists both the annotator and analyzer overloads without distinguishing them. No lci capability was missing for this map.

## Probe recipes

Binary: `build/release/src/lci`. The four-file fixture below is what the integration goldens use.

```bash
# summary, symbol (all flags), and a label query over the multi-lang fixture
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"probe","version":"0"}}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"side_effects","arguments":{"mode":"summary"}}}' \
 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"side_effects","arguments":{"mode":"symbol","symbol_name":"Add","include_reasons":true,"include_transitive":true,"include_confidence":true}}}' \
 '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"side_effects","arguments":{"mode":"category","category":"io","max_results":20}}}' \
 '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"semantic_annotations","arguments":{"label":"pure"}}}' \
 | build/release/src/lci mcp -r tests/parity/corpora/synthetic/multi-lang

# self-repo impure list with provenance (warmup takes the full index; expect tens of seconds)
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"probe","version":"0"}}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"side_effects","arguments":{"mode":"impure","include_reasons":true,"max_results":10}}}' \
 | build/release/src/lci mcp -r .

# purity block via get_context
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"probe","version":"0"}}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_context","arguments":{"name":"classify_callee_category"}}}' \
 | build/release/src/lci mcp -r .
```

Targeted tests (build `lci_tests` only, never the world):

```bash
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='SideEffect*:CalleeWordBoundary.*:TransitivePropagation.*:ComputePurityLevel.*:ClassifyAccessSequence.*'
build/release/tests/lci_tests --gtest_filter='SemanticAnnotatorTest.*:GraphPropagatorTest.*'
build/release/tests/lci_tests --gtest_filter='CatchClassifyTest.*:ResourcePairingTest.*:ResourceCalleeTest.*:GetContextPurityTest.*'
# goldens (all integration specs, ~53 s)
ctest --test-dir build/release -R lci_integration_suite --output-on-failure
# real corpora, skips without them
ctest --test-dir build/release -R lci_real_project_suite --output-on-failure
```

Before/after corpus diff for a classifier change: write a tiny driver that `#include`s `src/analysis/side_effect_analyzer.cpp` (anonymous-namespace access), run it over the identifier lists in `benchmarks/repo-qa/.work/exploration/*` at the pre-change sha (`git show <sha>:src/analysis/side_effect_analyzer.cpp`) and at HEAD, and report per-category gains AND losses. Recipe in `.claude/rules/test-iteration-discipline.md` rule 7 and rule 8 (ASan driver shape).

## Product comparison

Comparable tools for purity/effect analysis (unverified = from model knowledge, not measured; confirm before citing):

- Facebook Infer (Pulse): interprocedural, separation-logic, C/C++/Java/ObjC; reports impure functions with the modified access path. (unverified)
- CodeQL: dataflow/taint libraries, query-time whole-program DB; purity not first-class but some "side-effect free" predicates exist. (unverified)
- Semgrep taint mode: source/sink/sanitizer rules, intra-file by default (Pro: cross-file); no purity notion. (unverified)
- JetBrains IDEs: `@Contract(pure = true)` inference for Java/Kotlin, bytecode-based. (unverified)
- Frama-C (EVA/WP): sound value analysis with `assigns` clauses for C; correctness bar for "writes only X". (unverified)
- Effect systems (Haskell IO-in-type, Rust ownership, Koka/Eff): the theoretical bar, purity checked by the compiler not inferred.
- Other code-intelligence MCP servers (Serena, Sourcegraph MCP, GitHub code search, Cursor, Aider repo-map): symbols/references/text search only, no per-function purity record. (unverified; confirm via each server's `tools/list`)

lci's claims and how to check them:

| Claim | Where made | Measure |
|---|---|---|
| Detects param/global/closure/field writes, I/O, throws; transitive through call graphs | `docs/TOOLS.md:391`; tool description `handlers_analysis.cpp:2279` | run `mode: symbol` on a function with a known write; `include_transitive` on its caller; compare against a hand-read. `kFieldWrite` has no producer anywhere under `src/` (only `kClosureWrite` does, via `AccessTarget::Closure`), so "field writes" is a vocabulary claim not an emission |
| Never reports all-pure on an unanalyzed corpus | `docs/TOOLS.md:418` | `mode: summary` on an unsupported-language dir returns `analysis_unavailable` |
| Conservative: pure means pure | `include/lci/analysis/side_effect_analyzer.h` | take the `pure` list on a real corpus, sample N, hand-verify; the 2026-08-30 sweep found the Go closure counterexample (memory) |
| `@lci:` labels propagate across the call graph with strength/hops | `docs/TOOLS.md:336` | annotate one function, query `label` with `include_propagated`, check `hops`/`strength` on callers |
| Selection: models pick `side_effects` for "what does this routine touch" | `benchmarks/repo-qa/ANALYSIS-toolcalling-descriptions.md:76` | 4/5 under live descriptions, 1/6 to 2/6 under rewrites; competitor was native read/glob/bash |

Existing numbers: only the selection/comprehension bench cells above. There is no purity precision/recall measurement against a labelled corpus in `docs/performance/`, `tests/benchmarks/baseline/`, or `benchmarks/`; `benchmarks/repo-qa/ANALYSIS-comprehension-surface.md:44` states the side_effects annotated arm is "idealized and requires production capability work". Any accuracy figure quoted for this area today is an estimate.

Comparison axes:

| Axis | lci | Check the competitor | Notes |
|---|---|---|---|
| Analysis basis | tree-sitter AST facts + callee-name keyword tables, no dataflow, no types beyond receiver names | Infer/Frama-C: abstract interpretation; CodeQL: dataflow on a typed DB | lci is syntactic-heuristic by design (`docs/src/content/docs/error-handling-score.mdx:240`, "What the analysis will not claim") |
| Soundness direction | precision-first; pure claims meant to be trustworthy, impure list incomplete | Frama-C sound over-approximation; Infer aims for low FP | measure by sampling the `pure` list |
| Interprocedural | fixpoint over caller edges, decay 0.95, floor 0.3, 100 iterations | Infer summaries; CodeQL global dataflow | lci resolution is name-based; dynamic-dispatch ref recall was ~20% on the battery corpus (memory) |
| Languages | every tree-sitter grammar lci indexes (13+), one shared "generic" analyzer | Infer: C-family/Java; Frama-C: C; JetBrains: JVM | breadth vs depth |
| Query surface | MCP JSON, 6 modes, sorted and capped | CLI reports, SARIF, IDE inspections | none of the listed tools exposes an MCP purity tool |
| Latency | computed once at warmup, queries are map walks | whole-program analysis minutes to hours | warmup cost recorded in `docs/plans/2026-08-30-cold-start-profiling.md` |
| Annotations | `@lci:` inline + JSON manifests, propagated | JetBrains `@Contract`, Frama-C ACSL, Infer `@Pure`/nullability | lci labels are free-form; no checking against the inferred result (`trust_annotations` and `track_field_access` are dead knobs; only `strict_mode` is read, at `src/analysis/side_effect_analyzer.cpp:1053`) |
| Error-handling findings | beta, dark; catch-site, resource pairing, undo cost | SonarQube/CodeQL rule packs | `error_report` off by default |

Honest gaps vs the field: no dataflow, so aliasing and indirect writes are invisible (`kIndirectWrite` has no producer); closures are folded into the parent; incremental reindex leaves stale records; column keys not wired; fixpoint truncation not surfaced to the caller; no labelled ground-truth corpus, so precision/recall are unmeasured; annotations are never reconciled against inferred purity.

## Related skills

lci-feature-map (router), lci-search, lci-symbol-navigation, lci-get-context, lci-parsing-languages, lci-code-insight, lci-git-analysis, lci-indexing-pipeline, lci-server-lifecycle, lci-mcp-server, lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation.
