---
title: Reference-Port Discipline (Go → C++)
role: steering
scope: lci-cpp
written_at: 2026-07-13T16:35:47Z
epic: "CLX get_context ContextLookupEngine port (parent 01KT2JPFRYPSVA1YWN42RWHF71, slices S1–S8)"
sources:
  - port_map: "task-comment 01KXCYNRCFJECKP5EVTZ9A75B7 (parent, opus deep-read of 8 Go files)"
  - s4_rewind: "workflow-rewind 01KXDMXMAZ4X4WN7AM9J7X2358 (task 01KXCYQ5Y18RY4K1CR775DSEAV)"
  - s3_lease: "task-comments 01KXDNDTRRPPP4WYM8Z1NCBT88, 01KXDTWD6RH9M1SSDTNYQEQYXE, 01KXDY7ACM9KMYQD3QF39Q40YE (task 01KXCYQ5X7VF48KM3W2A2D0WJE)"
  - s8_close: "task-comment 01KXE55HVAC1W48YSA7X4XFQEV (parent), commit a44d98f"
  - commit_range: "9e086d3..a44d98f"
---

# Reference-Port Discipline (Go → C++)

Forward-usable rules for porting a live Go subsystem into the C++ tree. Distilled
from the CLX `get_context` ContextLookupEngine port (6261 Go LOC / 8 files → 7 new
C++ files). Each rule is a checkable gate, not a story.

## 1. Read the port-map trap list before writing each slice; the reviewer re-checks it against the diff

A per-epic port map with a NUMBERED trap list is a prerequisite artifact, not
documentation. Both the implementer (before coding) and the reviewer (before
advancing) MUST walk every trap against the current slice's diff.
> Enforcement: S4's only rewind — `class_variables` dispatched on `Class||Struct`
> instead of Go's `Class`-only gate — was a divergence the port map had ALREADY
> documented (SymbolType-gating trap). A pre-documented defect that still shipped
> to review is a process miss, not a coding miss.
> `source_event: port_map, s4_rewind`

## 2. Pin goldens to what the reference binary ACTUALLY emits — never to what its source appears to do

Reading reference source misleads: dead branches, `nil`-returning helpers, and
stub outputs all look live in source. Pin parity goldens by running the reference
binary and capturing real emissions.
- Drive the Go binary over its stdio MCP against the fixture corpus.
- Set `cwd` = the fixture directory so the repo's own `.lci.kdl` does not shadow it.
- Diff the captured emission into the golden; do not hand-write expected values.
> e.g. trap 10: `fan_in` via `findContainingFunction` returns `nil` in Go, so the
> real emission is empty — a source-reading port would have invented a value.
> `source_event: port_map (traps 5,6,10), s8_close, commit a44d98f`

## 3. Port only paths reachable from the live entrypoint; trace reachability first

~40% of the 6261 Go lines were dead tree-sitter AST helpers (`fan_in`,
`call_frequency`, dead AST walkers). The live path read almost only the reference
tracker; `graphPropagator`/`semanticAnnotator` fed one section; `trigramIndex`/
`componentDetector` were effectively unused.
- Before porting a file, trace which functions the live entrypoint reaches.
- Do not port a function you cannot reach from the live path. Absence of a caller
  is the signal, not a reason to preserve it "for completeness."
> `source_event: port_map`

## 4. Pin stubs; do not implement them (over-build guard)

When the reference returns a stub, the port reproduces the stub output — it does
not build the real feature. Pinned stubs from this epic: `return_values=[]`,
`test_coverage` always false/empty, `requires_tests` always true, similar-object
finders + smell predicates empty/false, `InterfaceInfo.methods=[]`,
`is_fully_implemented=true`, `ImportInfo.import_name` never set.
> Building past a reference stub is over-build and breaks parity goldens.
> `source_event: port_map (trap 5)`

## 5. Reproduce reference bugs bug-for-bug, with a TODO marker

Faithful parity includes the reference's bugs. Mark each with a TODO so it is a
recorded divergence-from-correct, not an accident. Bugs pinned this epic:
refactoring suggestions read code-smells before detection runs (always empty);
service-dep matcher always reports `servicePatterns[0]`; structure interface/
inheritance gates on `Class` only (excludes `Struct`); self-referential parameter
scope match.
> `source_event: port_map (trap 6)`

## 6. Cross-runtime determinism: sort before emit, compare set-wise, map ordinals explicitly

- Go map iteration order is nondeterministic → C++ MUST sort deterministically
  before emit; goldens compare set-wise.
- Reference-type `Context` fields that are NUMERIC STRINGS of a Go enum ordinal do
  NOT match C++ enum ordinals — map to the Go integers explicitly or goldens break.
- Go `nil` slices serialize as JSON `null`; choose `[]` and document the divergence
  ONCE, workspace-wide.
> Reinforces `.claude/rules/karpathy-principles.md` rule 4 (determinism).
> `source_event: port_map (traps 1,2,3)`

## 7. Extend the existing section/mode machinery; never duplicate it

The C++ tree already had `normalize_context_params`, `apply_context_lookup_mode`,
`section_allowed`, the reference-tracker snapshot API, `EnhancedSymbol`, the test
fixture, and the golden-spec dir. Reuse-first: extend the existing table, do not
stand up a parallel one.
> `source_event: port_map`

## 8. Sequence composite/AI sections last; pin their goldens only after upstreams land

Fill order is a hard dependency, not a preference: `basic_info → relationships →
variables → semantic → structure → usage → ai`. The AI section READS prior
sections, so its slice runs last and its golden is pinned only after every
upstream slice has landed.
> S8 (ai) correctly stayed blocked until S3 (relationships) and S6 (usage) landed.
> `source_event: port_map (fill order), s8_close`

## 9. Judge a foreign lease by work-evidence, not renewal liveness; document takeover

A lease renewed by an automated heartbeat looks alive while the session is dead.
Do NOT steal on liveness alone; do NOT wait forever on renewal timestamps either.
Take over only on positive absence-of-work evidence, and record it in a comment.
- Evidence that justified the S3 takeover: transcript idle 6.5h, zero commits in
  the task's file scope, zero task comments, clean working tree, renewals traced to
  heartbeat only.
- Release via the same-holder path and leave a takeover comment so a resuming
  original session checks the comment + `git log` before acting.
> `source_event: s3_lease`

## Known gaps (not rules — tracked follow-ups)

- `variable_context` returns empty for real self-hosted C++ symbols with obvious
  parameters (e.g. `handle_get_context`, `determine_severity`), despite the S4
  dispatch logic being language-agnostic. Suspected upstream C++ tree-sitter
  extractor gap (Parameter/local-Variable symbol kinds not emitted for C++ the way
  the Go extractor emits them for Go), NOT a defect in the S4 port. Follow-up filed
  on parent `01KXE55HVAC1W48YSA7X4XFQEV`.
