---
name: lci-get-context
description: "Use when: working on MCP get_context/context (manifest save/load), ContextLookupEngine sections, object IDs (base-63), source_excerpt/purity, the LCF compact format, the context-reduction claim, or comparing lci context delivery with Aider/Cursor/Serena/Cody/Copilot."
---

# lci get_context and context manifests

`get_context` turns object IDs (from `search`) or a symbol name into a per-symbol packet: location, signature, bounded `source_excerpt`, purity, and (name path) callers/callees/call_tree, or the full seven-section `CodeObjectContext` when a `mode`/section is requested. `context` saves a 2-5 KB manifest of file/symbol references with expansion directives and hydrates it back into source plus call-graph neighbours under a token budget. Both exist so an agent quotes code without opening files; the engine is a bug-for-bug port of the Go `ContextLookupEngine` with pinned stubs.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| MCP `get_context` tool definition | `src/mcp/handlers_core.cpp:287` | Params: id, name, mode, include_call_hierarchy, max_depth, include_ai_text, confidence_threshold, include_sections, exclude_sections; aliases symbol_id/object_id/object_ids/oid/symbol/path (line 313). Registered exclusive (not concurrent_ok). |
| MCP `get_context` handler | `src/mcp/handlers_get_context.cpp:333` `handle_get_context` | Dispatch from `handlers_core.cpp:322`. |
| MCP `context` tool definition | `src/mcp/handlers_context.cpp:676` `register_context_handlers`, `add_tool` at `:798` | operation save/load; refs items schema f/l/note/role/s/x (`:730-790`). |
| MCP `context` handler | `src/mcp/handlers_context.cpp:657` `handle_context`, save `:383`, load `:516` | Manifest files resolved under project root (`resolve_manifest_path` `:256`). |
| `info get_context` / `info context` | `src/mcp/handlers_core.cpp:130` | Registry-derived parameter listing; cannot lag tools/list. |
| CLI `lci inspect` | `src/cli/main.cpp:920` | Nearest CLI analogue (covered by lci-symbol-navigation). No CLI or HTTP route exposes get_context or manifests. |
| Fuzz target | `tests/fuzz/fuzz_get_context.cpp` | ID codec + full handler path. |

## Code map

Entry -> engine -> storage, in call order.

- `src/mcp/handlers_get_context.cpp` — `normalize_context_params` (:47, alias remap), `extract_oid_prefix` (:73, pastes `oid=VE,tG`), `apply_context_lookup_mode` (:100, mode presets; unknown mode -> full), `section_allowed` (:150), `autosearch_workflow_hint` (:181, symbol+path returns `_auto_search_triggered` payload, not context), `attach_purity` (:229), `attach_source_excerpt` (:242, `kMaxExcerptLines = 12`), `resolve_object_id` (:280, id path), name path with callers/callees/call_tree (:414-540), rich path constructing `ContextLookupEngine` (:546-620), id path loop (:660).
- `include/lci/core/context_lookup.h` — `ContextLookupEngine`: atomic config (max_context_depth 5, include_ai_text true, confidence_threshold 0.3), optional `GraphPropagator*`/`SemanticAnnotator*`, `get_context`, `filter_context_sections`, `per_component_time_ms`.
- `include/lci/core/context_lookup_types.h` — `CodeObjectID`, `ObjectReference`, `DirectRelationships`, `VariableContext`, `SemanticContext`, `StructureContext`, `UsageAnalysis`, `AIContext`, `LookupDiagnostics`, `CodeObjectContext` (to_json; empty vectors serialize `[]`, never `null`).
- `src/core/context_lookup.cpp` — `ContextLookupEngine::get_context` (:106) pins the ReferenceTracker snapshot once, resolves the target once, fills sections in fixed order (:148-155): relationships -> variables -> semantic -> structure -> usage -> ai. `filter_context_sections` (:179), `per_component_time_ms` (:239).
- `src/core/context_lookup_relationships.cpp` — `fill_direct_relationships`: callers/callees/incoming/outgoing refs with confidence, imported_modules, child objects.
- `src/core/context_lookup_variables.cpp` — `fill_variable_context`: globals, class vars, locals, parameters; return_values pinned `[]`.
- `src/core/context_lookup_semantic.cpp` — `fill_semantic_context`: entry-point deps (BFS over ref tracker), service deps, propagation labels, criticality; degrades to empty when propagator/annotator are null.
- `src/core/context_lookup_structure.cpp` — `fill_structure_context`: file/module/package, imports, exports, interfaces, inheritance, composition pattern.
- `src/core/context_lookup_usage.cpp` — `fill_usage_analysis`: fan_out real, fan_in/call_frequency pinned 0, test_coverage pinned false, requires_tests pinned true.
- `src/core/context_lookup_ai.cpp` — `fill_ai_context`: natural_language_summary, code_smells (long function, cyclomatic), refactoring_suggestions, best_practices, similar_objects pinned `[]`.
- `src/core/graph_propagator.cpp` (`GraphPropagator`, `include/lci/core/graph_propagator.h:109`) and `src/core/semantic_annotator.cpp` (`SemanticAnnotator`, `include/lci/core/semantic_annotator.h:66`) — label propagation and `@lci:` annotations; seeded in `src/mcp/runtime.cpp:16` `McpRuntime::warmup` for the semantic_annotations tool.
- `include/lci/idcodec.h` — base-63 alphabet (A-Z a-z 0-9 _), `encode_symbol_id`/`decode_symbol_id` (:150/:155), `encode_composite` (:190). Object IDs in `search` output (`o=XX`) are these.
- `src/mcp/handlers_context.cpp` — `manifest_to_json` (:63, compact Go keys t/c/v/p/r/s; ref keys f/s/l{s,e}/role/n/x; stats rc/tl/fc/rb), `manifest_from_json` (:121, verbose keys accepted with stderr warning), `validate_manifest` (:183), `hydrated_context_to_json` (:215), `save_manifest_to_file` (:285, temp+rename), `filter_refs_by_role` (:351), `parse_format` (:375), load loop with token budget (:585-640).
- `include/lci/context_manifest.h` — `ContextRef`, `ContextManifest`, `HydratedRef` (source, signature, purity), `HydrationStats` (tokens_approx, truncated), `FormatType`.
- `src/mcp/context_manifest_expander.cpp` / `include/lci/mcp/context_manifest_expander.h` — `parse_expansion_directive` (:20, `type[:depth]`), `ExpansionEngine::hydrate_reference` (:197, tokens = bytes/4), `apply_expansions` (:241; directives callers, callees, implementations, interface, siblings, tests, doc, signature), `expand_callers` (:339), `expand_callees` (:373).
- `src/mcp/response.cpp` — `dump_json_lossy` (U+FFFD on bad UTF-8, found by fuzz_get_context), `make_json_response`, `make_error_response`, `make_unavailable_response`.
- `src/mcp/formatter_compact.cpp` / `include/lci/mcp/formatter_compact.h` — `CompactFormatter::format_context_response` (:70) renders `LCF/1.0` `c=N` then `path:line` / `o= t= n= e= s= d=` lines. See traps: no production caller.
- `src/analysis/token_budget.cpp` — `TokenBudgetManager` budgets the code_insight `CodebaseIntelligenceResponse` (8000 base, clamp 4000-12000), not get_context; only caller `src/analysis/codebase_intelligence.cpp`.
- Tests: `tests/mcp_handlers_core_test.cpp` (GetContext* on HandlersFixture :885-2006, `FilterContextSections` :1139, `DirectRelationshipsFixture` :2182, `VariableContextFixture` :2465, `StructureContextFixture` :2591, `SemanticContextFixture` :2774, `AiContextFixture` :3069, `GetContextPurityTest` :464); `tests/mcp_handlers_context_test.cpp` (ContextManifestJson, ValidateManifest, ParseExpansionDirective, HydratedContextJson, ContextHandlerFixture :265-576); `tests/integration/real_project_context_test.cpp` (RealProjectGetContextTest, RealProjectContextManifestTest; skip without corpora); `tests/benchmarks/real_project_advanced_benchmarks.cpp:97` (get_context latency).
- Golden specs: `tests/integration/mcp/get_context/basic.spec.json`, `semantic_ai.spec.json`, `tests/integration/mcp/context/invalid-operation.spec.json`, `tests/integration/mcp/context_manifest/basic.spec.json`; goldens under `tests/integration/goldens/mcp/{get_context,context,context_manifest}/`. Corpus `multi-lang` resolves to `tests/parity/corpora/synthetic/multi-lang` (`tests/integration/spec_runner.cpp:192`).
- Docs: `docs/TOOLS.md:137` (get_context), `:164` (context); `docs/src/content/docs/mcp-server.mdx:39-40`; `docs/reviews/2026-09-03-review.md:200-210` (S7 findings); `docs/plans/2026-07-18-hardcoded-response-shape-ab-design.md`; `docs/plans/2026-08-27-enhanced-names-design.md` (planned `enhanced_name`/`desc` surfacing in get_context).
- Benchmarks: `benchmarks/repo-qa/ANALYSIS-comprehension-surface.md` (get_context row: parity, 320 historical calls, annotated arm not production-faithful); `benchmarks/repo-qa/ANALYSIS-response-shape.md` (scorecard withdrawn, no result yet); `benchmarks/repo-qa/discovery/runner/rendering.py:48-62,109` (why get_context is uncitable); `benchmarks/repo-qa/ANALYSIS-toolcalling-descriptions.md:78`.

## Config knobs

No `.lci.kdl` key or env var is read by get_context, the engine, or the manifest handlers (`grep getenv src/mcp/*.cpp src/core/context_lookup*.cpp` hits only the code_insight error-report path). Everything is per-request:

- `mode` presets (`handlers_get_context.cpp:100`): full -> max_depth 5 + include_ai_text; quick -> depth 2, no ai, sections relationships+structure; relationships / semantic (semantic+ai) / usage / variables.
- `max_depth` clamped [1,10] on the name path (`:417`), [1,20] in the engine (`context_lookup.h`).
- `confidence_threshold` default 0.3; `include_ai_text` default true.
- `context.max_tokens` (0 = unlimited; `handlers_context.cpp:585`), `format` full|signatures|outline, `filter`/`exclude` by role.
- Engine defaults `include/lci/core/context_lookup.h`: `max_context_depth_{5}`, `include_ai_text_{true}`, `confidence_threshold_{0.3}`.
- Indirect: `FeatureFlagsConfig.enable_relationship_analysis` (`include/lci/config.h:137`) has no reader in src; `SearchConfig.default_context_lines`/`max_context_lines` (`config.h:119,127`) govern search snippets, not this tool.

## Invariants and traps

Port traps (`.claude/rules/reference-port-discipline.md`; CLX port map comment `01KXCYNRCFJECKP5EVTZ9A75B7` cited in each section file header):
- Trap 1 sort before emit: every map-derived bucket is sorted (`context_lookup_variables.cpp:39`, `context_lookup_structure.cpp:108`). Trap 2: empty lists emit `[]` (`context_lookup_types.h:13`). Trap 3: ref `context` is the decimal string of the Go RefType ordinal; C++ enum order happens to match (`context_lookup_relationships.cpp:15,106`).
- Trap 4: `VariableInfo.type` is the real type string (`context_lookup_variables.cpp:94`). Trap 5 pinned stubs: return_values `[]`, test_coverage false, requires_tests true, similar_objects `[]`, smell predicates false, `InterfaceInfo.methods` `[]`, is_fully_implemented true, import_name unset (`context_lookup_usage.cpp:45`, `context_lookup_ai.cpp:9`, `context_lookup_structure.cpp:13`).
- Trap 6a: refactoring suggestions read code_smells before detection runs (`context_lookup_ai.cpp:19`). 6b: service deps always report `api_call` (`context_lookup_semantic.cpp:11,269`). 6c: interface/inheritance/child-object gates are Class-only, Struct excluded (`context_lookup_structure.cpp:17,138`; `context_lookup_relationships.cpp:27`). 6d: parameter scope match is self-referential (`context_lookup_variables.cpp:13`).
- Trap 8: snapshot pinned once, target resolved once, `stable_sort` not bubble sort (`context_lookup.h`, `context_lookup.cpp:131`). Trap 9: import_style "direct" in relationships vs "default" in structure (`context_lookup_relationships.cpp:547`).
- Trap 10 (empirically verified against the Go binary): fan_in and call_frequency are always 0 (`context_lookup_usage.cpp:12-44`). The Go tree is deleted (`memory/go-reference-tree-deleted.md`), so this verification cannot be re-run.
- `performance.component_breakdown` is total/7 for all seven fields, bug-for-bug (`context_lookup.h` `per_component_time_ms`, `handlers_get_context.cpp:600-625`). Do not replace with real timing without a parity decision.
- `filter_context_sections` zeroes but never drops keys; unknown tokens are ignored (`context_lookup.cpp:179`).
- Relationships buckets `parent_classes`/`implementing_types` are unreachable-empty by Go's outer gate (`context_lookup_relationships.cpp:31-40`).

Handler contracts:
- Exactly one of `id`/`name`; both or neither is an error (`handlers_get_context.cpp:378-390`). `symbol`+`path` returns the auto-search workflow hint, not context (`:349`). Name path resolves without `mode` since PR #9 (`memory/mcp-real-repo-bug-hunt.md` BUG 2; spec `_rationale` in `get_context/basic.spec.json`).
- `mode` is ignored on the id path: `{"id":"Dxo","mode":"quick"}` returns the compact envelope (test `GetContextModeWithIdFallsThroughToIdPath`, probed live). The rich `context`/`metadata`/`performance` envelope requires `name` plus mode or sections (`:546`).
- Callers/callees/call_tree are bare NAMES (`get_caller_names`, `include/lci/core/reference_tracker.h:454`); call_tree recurses into the first same-name match only. This is why `benchmarks/repo-qa/discovery/runner/rendering.py:48-62` routes every citation-graded caller question to `callers` (bench-harness rule 10; product gap `01M1VR4TBD7GXAXWBQXV2MCR49`).
- `source_excerpt` is capped at 12 lines with `truncated` (`:244`); purity comes from `SideEffectAnalyzer` only when wired (`GetContextPurityTest`).
- Rich path constructs `ContextLookupEngine engine(indexer)` at `:555` and never calls `set_graph_propagator`/`set_semantic_annotator`; the only callers are tests (`tests/mcp_handlers_core_test.cpp:2776-2847`). In production `propagation_labels`, `criticality`, and annotator-backed semantic fields are always empty even though `McpRuntime` owns a seeded propagator/annotator (`src/mcp/runtime.cpp:55-70`).
- Purity `reasons` on the live probe reported `writes to global 'params'` for a by-reference PARAMETER of `apply_context_lookup_mode` (`handlers_get_context.cpp:100`); the analyzer classifies mutation of a reference parameter as a global write. Owned by lci-side-effects, visible here.
- Registered exclusive: the engine + analyzer path is unaudited for concurrent reads (`handlers_core.cpp:325`). Karpathy rule 3 (no mutex on read path) still holds via RCU pins.
- `docs/reviews/2026-09-03-review.md:206`: nine advertised get_context params have no reader; `append=true` swallows load errors (partly fixed: `AppendToCorruptManifestErrorsAndLeavesFile`); token budget checked only on the next iteration.

Manifest contracts:
- Compact Go keys are the contract; verbose keys load with a one-line stderr warning (`handlers_context.cpp:22`). `l` is `{s,e}` on the wire while the tool schema advertises `{start,end}` (`:748-760`) and `note` while the wire key is `n` (`:87`).
- Token accounting is bytes/4 (`context_manifest_expander.cpp:235,282`); `tokens_approx` is an estimate, not a tokenizer count.
- `format` is parsed (`:375`) but `extract_symbol_source` ignores it (`context_manifest_expander.cpp:122`, parameter commented out): `signatures` and `outline` return the full body (probed live).
- Save/load paths must stay inside the project root (`SaveRejectsPathEscapingProjectRoot`).

Findings from the 2026-09-08 mapping (fixed or filed): see docs/reviews/2026-09-08-skill-mapping-findings.md.

Dogfood gaps: `lci def`/`refs` handled every lookup here; `lci refs ContextLookupEngine` lists definitions and static calls but not the construction site in `handlers_get_context.cpp:555` (constructor call not counted as a reference).

## Probe recipes

Binary: `build/release/src/lci`. MCP is newline-delimited JSON-RPC on stdio; `-r` sets the root.

```
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"probe","version":"0"}}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_context","arguments":{"name":"apply_context_lookup_mode","include_call_hierarchy":true,"max_depth":2}}}' \
 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_context","arguments":{"name":"apply_context_lookup_mode","mode":"full"}}}' \
 '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_context","arguments":{"id":"Dxo"}}}' \
 | build/release/src/lci mcp -r .
```
Object IDs come from `search` output (`o=XX`); `Dxo` is the id the probe above returned for `apply_context_lookup_mode` on this tree and changes with the index.

Manifest round trip:
```
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"probe","version":"0"}}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"context","arguments":{"operation":"save","to_string":true,"task":"probe","refs":[{"f":"src/mcp/handlers_get_context.cpp","s":"apply_context_lookup_mode","x":["callers"]}]}}}' \
 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"context","arguments":{"operation":"load","max_tokens":400,"from_string":"{\"t\":\"probe\",\"v\":\"1.0\",\"r\":[{\"f\":\"src/mcp/handlers_get_context.cpp\",\"s\":\"apply_context_lookup_mode\",\"x\":[\"callers\"]}]}"}}}' \
 | build/release/src/lci mcp -r .
```

Tests (build only what you run, `.claude/rules/test-iteration-discipline.md`):
```
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='HandlersFixture.GetContext*:GetContextPurityTest.*:FilterContextSections.*:DirectRelationshipsFixture.*:VariableContextFixture.*:StructureContextFixture.*:SemanticContextFixture.*:AiContextFixture.*:ContextHandlerFixture.*:ContextManifestJson.*:ParseExpansionDirective.*'
ctest --test-dir build/release -R 'lci_integration_suite' --output-on-failure
LCI_UPDATE_GOLDENS=1 ctest --test-dir build/release -R lci_integration_suite   # re-pin goldens (spec_runner.cpp:726)
```
Real-corpus tests (`RealProjectGetContextTest`, `RealProjectContextManifestTest`) skip without corpora (`memory/real-project-suite-skips-in-ci.md`). Dogfood navigation: `build/release/src/lci def handle_get_context -r .` and `lci refs ContextLookupEngine -r ...` resolve every symbol named in this skill.
Fuzz: `tests/fuzz/fuzz_get_context.cpp` (uniform ASan build required, `memory/fuzz-targets-and-findings.md`).

## Product comparison

Comparable tools for agent context delivery: Aider repo-map (tree-sitter tags ranked by PageRank into a token-budgeted map), Cursor codebase indexing (embedding index over chunks, retrieval on demand), Claude Code native Read/Grep (whole-file or regex reads, no index), Continue.dev context providers (@codebase embeddings, @file, @repo-map), Serena MCP (LSP-backed symbol overview, find_symbol with body, find_referencing_symbols), Sourcegraph Cody context (SCIP precise code intel plus embeddings/keyword retrieval), GitHub Copilot workspace/@workspace (local index plus repo embeddings). Facts about these products are `(unverified, from model knowledge)`; verify against each vendor's docs before quoting.

lci's claims in this area:
- Goal: cut the tokens an agent spends reading source versus opening whole files (`README.md:3`, `docs/src/content/docs/index.mdx:36`; `code_insight` description `src/mcp/handlers_analysis.cpp:2320`). No committed measurement backs the specific percentages quoted in those places — `git log -S'79.8'` and `grep -rn '79\.8' benchmarks/repo-qa/*.md docs/` both return nothing; treat any number there as an inherited Go-era figure. To re-measure: for a frozen task bank, sum tokens of the tool payloads an agent reads (`get_context` + `search` responses) against the bytes a `grep -n` + Read workflow would open for the same answer; harness pieces exist in `benchmarks/repo-qa/scripts/bench.py` (flat `input`/`output` counters, bench-harness rule 6), and the arm-disjointness caveat in `.claude/rules/bench-harness-oracle-independence.md` rule 12 applies (treatment must not also have grep).
- "signature, file path, line numbers, callers/callees, references, source_excerpt ... replace opening the source file" (tool description, `handlers_core.cpp:289-294`). Measure with the comprehension harness: `benchmarks/repo-qa/ANALYSIS-comprehension-surface.md` shows get_context at parity (delta 0.000, 320 historical calls), annotated arm not production-faithful.
- "2-5KB manifest ... instant full context" (`handlers_context.cpp:689-695`). Measure with the save probe above: `stats.size_bytes`/`rb` in the manifest and `tokens_approx` on load.
- Latency: `tests/benchmarks/real_project_advanced_benchmarks.cpp:97` times get_context on real corpora; `tests/integration/real_project_performance_test.cpp`. No committed get_context latency table exists under `docs/performance/`.

| Axis | lci | How to check the competitor | Notes |
|---|---|---|---|
| Unit of context | Symbol packet by object id or name; bounded 12-line excerpt; seven optional sections | Aider: ranked tag map, no bodies; Serena: `find_symbol` with `include_body`; Cody/Copilot: retrieved chunks | lci excerpt cap forces a follow-up Read for long bodies |
| Call graph in context | Bare caller/callee names, depth-limited tree; locations only via `callers` tool | Serena `find_referencing_symbols` returns locations; Cody SCIP returns precise refs | Citation-graded questions cannot use get_context (rendering.py:48) |
| Token budgeting | `context.max_tokens` bytes/4 estimate, truncation flag; get_context has no budget | Aider `--map-tokens`; Cursor/Copilot hidden | No tokenizer; estimate only |
| Persistence/handoff | Manifest JSON file or string, roles, expansions, append | Aider none; Serena memories (markdown); Cursor none exposed | Format flag not honored (trap above) |
| Purity/side effects | `purity` block per function from SideEffectAnalyzer | None of the listed tools expose purity | lci-only axis; see lci-side-effects |
| Semantic labels | `@lci:` annotations + propagation exist but are not wired into get_context | Serena/Cody: none comparable | Wiring gap listed above |
| Determinism | Sorted buckets, `[]` for empty, goldens byte-stable | Embedding retrievers are non-deterministic by construction | Good for golden testing |
| Output format | JSON string inside MCP text; LCF only in code_insight | Serena: JSON; Aider: text map | `CompactFormatter::format_context_response` is unused in production |

Honest gaps vs the field: no embeddings or natural-language retrieval (exact/trigram/name lookup only, see lci-search); call hierarchy without locations; several sections are pinned Go stubs (fan_in, test_coverage, similar_objects, code smells beyond two) so `mode=full` carries dead weight tokens; `include_ai_text` produces templated prose, not model output; no cross-repo context; no disk persistence of the index (README), so a fresh server re-indexes before the first get_context.

## Related skills

lci-feature-map (router), lci-search, lci-symbol-navigation, lci-parsing-languages, lci-code-insight, lci-side-effects, lci-git-analysis, lci-indexing-pipeline, lci-server-lifecycle, lci-mcp-server, lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation
