# Error/Exception-Handling Scores (function → class → module → repo)

Status: research/design (2026-08-17). Feasibility surveyed against the current
tree; every claim below cites the code it stands on. Goal: an err-lookup-style
evaluation lci can generate — hierarchical error-handling scores plus concrete
highlights for **swallowed errors** and **potential resource leaks**.

## Feasibility verdict

Largely assembly. The propagation, aggregation, and emission layers already
exist and are reusable as-is; the new work is (a) a set of AST detection
branches at an existing hook point, and (b) a scoring/rollup module cloned
from the HealthAnalyzer/ModuleAnalyzer skeleton. Two hard constraints shape
the design:

1. **ASTs are parse-and-discard** (`UniqueTree` released at scope end —
   `pipeline_processor.cpp:61-68`, `mcp.cpp:64-72`). All detection must run
   during extraction. The warmup side-effect pass
   (`mcp.cpp:173-187 populate_side_effects_from_ast`) already re-parses every
   file once; new detectors ride that same pass at zero extra parse cost.
2. **No CFG/dataflow layer.** Signals are syntactic + name-heuristic +
   graph-propagated. That bounds precision: we can say "acquire with no
   release on any path *syntactically visible*", not prove leak-freedom.
   Findings are therefore emitted with a confidence tier, never as verdicts.

## Detection signals (new — all at `process_side_effect_node`, `unified_extractor_side_effects.cpp`)

The hook is already called for every node inside a tracked function
(`visit_node` → `unified_extractor.cpp:348`, function context via
`se_func_depth_`). New branches + `record_*` APIs on SideEffectAnalyzer:

### Swallowed errors
| Signal | Detection | Confidence |
|---|---|---|
| Empty catch | `catch_clause`/`except_clause`/`rescue`/`catch_block` with empty or comment-only body | high |
| Catch-and-continue | catch body with no throw/raise/return-error/log call | high |
| Broad catch | caught type is `Exception`/`Throwable`/bare `except:`/`catch (...)` | medium |
| Log-and-swallow | catch body's only call is a log-category callee (existing `classify_callee_category` io/log prefixes) | medium |
| Go dropped error | `_ = err` blank assign; call whose error result is unassigned; `err` assigned but next use is not a check (`if err != nil` condition inspection inside `if_statement`) | high/medium |
| Rethrow-without-cause | `throw new X(...)` in catch body without the caught variable as argument (Java/C#/PHP/JS) | low |

### Resource leaks
Acquire/release pairing per function, syntactic:
- Acquire: callee matches acquisition table (open/connect/lock/acquire/
  malloc/new-with-Close-type per language) — extends the existing
  `classify_callee_category` prefix mechanism (`side_effect_analyzer.cpp:481`).
- Release credit: matching release callee, OR `defer`/`errdefer`/`finally`/
  `ensure`/`using`/`with` context (Go `record_defer` exists; the rest are new
  nodes: `finally_clause`, `ensure`, `using_statement`, `with_statement`,
  Zig `errdefer`).
- Finding: acquire with zero release credit in the same function → potential
  leak (medium); release present but not in defer/finally while function also
  throws/returns-error between acquire and release lines → leak-on-error-path
  (low/medium, line-order heuristic, no CFG).

### Propagation hygiene
- `kThrow` transitive propagation already works
  (`propagate_transitive`, decay 0.95 — `side_effect_analyzer.cpp:364-458`).
- New: seed "swallows" and "leaks" labels into the existing `GraphPropagator`
  (four modes, `graph_propagator.h`) — a public API function transitively
  reaching a swallow site scores worse; weight findings by the CallGraph's
  exact `incoming_reach` (post-D2 fix) so a swallow in a load-bearing
  function costs more than one in a leaf.

### Gap-closing prerequisites (found by survey, cheap)
- `record_error_return` / `record_try_finally` have **zero production
  callers** (tests only) — `returns_error`, `try_finally_count`,
  `error_return_lines` are dead fields today; wire them first.
- Go `panic`, Ruby `raise`/`rescue`, Kotlin `throw`, Zig `try`/`errdefer`
  have no precise node handling (generic `throw_statement` misses their
  grammars); add per-grammar branches.
- `ThrowSiteInfo.type` is empty except Rust macros — leave typed-exception
  propagation out of scope (needs type resolution; "impossible without
  re-architecture" tier).

## Scoring model

Per function: start 1.0, subtract per finding, weighted by
severity × confidence × normalized fan-in. Kind-gated (functions/methods
only) and **production-only via D1 path attributes** — test/vendored/example
code never scores (a test's empty catch is fine).

Rollups reuse existing skeletons:
- function → class/file: group by container symbol
  (`FileSymbolData`, HealthAnalyzer pattern).
- file → module: `ModuleAnalyzer::analyze` grouping via
  `CouplingAnalyzer::get_package_name` (`module_analyzer.cpp:80-159`), adding
  `error_handling_score` beside cohesion/coupling.
- module → repo: monotone non-saturating formula per the D3 precedent
  (`calculate_overall_health_score` shape) — a repo with a cc-style extreme
  (one function leaking in every path) cannot score 10.
- Aggregation counters: clone the `tally_purity` template
  (`handlers_analysis.cpp:1476-1511`) into `ErrorHandlingSummary`.

Determinism (karpathy rule 4): findings sorted by (file, line, signal);
set-wise golden comparison.

## Surfacing

- `code_insight` new section `== ERROR HANDLING ==`: repo score, per-module
  scores, top-N findings as `severity signal symbol (file:line) [o=id]
  reason=...` — locations always carry filenames (D6 lesson).
- `side_effects` MCP tool already returns JSON; extend its per-function
  payload with the new `ErrorHandlingInfo` fields — this is the natural
  err-lookup ingestion path (err-lookup execs the binary; JSON preferred over
  LCF parsing).
- Every finding carries its evidence line(s) → clickable, and gives the
  multi-model evaluation harness concrete claims to verify (same
  capture/baseline/judge method as ANALYSIS-insight-verification.md; oracle =
  lci-blind LLM reading the flagged functions).

## Out of scope (needs re-architecture — declined for now)
- Post-hoc AST queries (trees are freed) — everything extracts inline.
- True dataflow ("was err checked before overwrite") — no CFG layer.
- Exception-type propagation — no type resolution on throw sites.
- Independent scoring of nested closures — analyzer folds nested effects
  into the enclosing function (`unified_extractor.cpp:305-317`).

## Phasing
1. Wire dead fields (`record_error_return`, `record_try_finally`) + missing
   throw/rescue/panic grammar branches; RED via per-language fixtures.
2. Swallow detectors + Go error-drop detection.
3. Acquire/release pairing tables + leak findings.
4. Scoring module + rollups + `== ERROR HANDLING ==` / JSON emit.
5. Verification round on the 3-repo beachhead (lci-blind judge over flagged
   sites), then err-lookup corpus.
