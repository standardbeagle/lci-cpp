# code_insight report quality plan

Source: 2026-08-26 four-repo field run (slop/Go 155f, agnt/Go+JS 1211f,
track/C# 796f, lci-cpp/C++ 4938f) driven over MCP stdio, sections rated for
agent usefulness. Confirms residuals R1 (reach inflation) and R2 (fake
cycles) from the 2026-08-19 3-model verification on real corpora, and adds
new findings. The parameter-allowlist bug found in the same run (git-mode
params rejected by the unknown-parameter guard) is fixed separately.

What already earns its tokens (do not regress): REPOSITORY MAP,
problematic_symbols, BROKERS betweenness, GIT HOTSPOTS on active repos,
SUMMARY attribute exclusions, aliases_in_use.

## P1 — Call-edge precision (root cause under CYCLES and LOAD BEARING)

Evidence:
- lci-cpp `size -> size` (deleted_file_tracker.h): `size()` calls
  `absl::flat_hash_set::size()`; name-based resolution makes it a self-edge.
  Same shape: track `Equals -> Equals`, `GetHashCode -> GetHashCode`,
  `SendAsync -> SendAsync`. All 5 reported "cycles" in both track and
  lci-cpp are single-node; none is an architectural cycle.
- track LOAD BEARING: minimal-API lambda `Add` reach=318; four WorkTrackDb
  log helpers all reach=236. Name-collision edges inflate reach.
- cycles, reach, and betweenness all consume the same polluted graph;
  brokers survives because betweenness demotes leaf noise — reach does not.

Slices (each RED/GREEN, discrimination tests per
`.claude/rules/bench-harness-oracle-independence.md`):
1. Self-loop gate: emit a single-node cycle only when the call site
   resolves to the same symbol_id with a self/implicit receiver (use the
   receiver_type resolution added in eb85054). Fixture: two types with
   same-named methods calling each other's — assert NO cycle; a genuinely
   recursive function — assert the cycle survives.
2. Cycle ranking: multi-node SCCs first; genuine direct recursion moves to
   a separate `recursion=` line (it is a property, not a design smell).
3. Reach over resolved edges only: drop or down-weight name-fallback edges
   whose target name has fan-in above a collision threshold; carry a
   confidence marker on displayed reach values.
4. Re-run the four-repo field check as the acceptance test: track must not
   report `Add` in LOAD BEARING; lci-cpp/track CYCLES must be empty or
   real.

## P2 — VOCABULARY outlier noise (R4)

Evidence: misspelling suggestions `fail -> tail`, `constant -> content`,
`external -> internal`, `unscoped -> scoped`, `serializer -> serialize`;
obscure-token flags on ordinary words (`opacity`, `less`, `expect`,
`matching`, `loop`).

- Misspelling: suggest only when the token is not itself a real word —
  check against a small embedded wordlist plus the repo's own identifier
  vocabulary; require a shared stem; never suggest a token whose meaning
  inverts (`external -> internal`). If precision cannot be demonstrated on
  the four-repo corpus, delete the misspelling class and keep
  aliases_in_use (the valuable half of the section).
- Obscure-token: gate on corpus frequency — a token used across many
  modules is project vocabulary, not obscurity.

## P3 — Per-section value tuning

- detailed:modules: every module reports `type=General` (classifier
  effectively dead) and cohesion 0.02–0.10 carries no signal. Implement
  real type classification (entry/api/domain/infra from imports + naming)
  or drop the column; recalibrate or drop per-module cohesion from this
  view.
- LAYER VIOLATIONS: slop labels interpreter helpers "Presentation Layer"
  and reports 8 false violations. Gate the section on explicit layer
  config in `.lci.kdl`; with heuristic layers emit a `layers=heuristic`
  disclaimer or suppress the section below a confidence bar.
- Score scales: HEALTH `score=6.05` (0–10) beside STATISTICS
  `maintainability=92.65` (0–100) with no legend. Label units on both
  (`/10`, `/100`) or unify.
- ENTRY POINTS: C# minimal-API lambdas surface as `Add`; use route/signature
  metadata where present and demote trivially-named exports. Replace
  "... and 498 more exported" with a per-module count line.

## P4 — Re-panel gate (R3)

R3 (new-section score saturation) plus R1 fixes gate the insight quality
panel: after P1 and P2 land, re-run the 3-model verification
(insight-verification-2026-08-16 protocol) before declaring the report
trustworthy.

## Sequencing

P1 (one resolver fix serves cycles + reach) → P2 (cheap, loudest noise) →
P3 section-by-section as touched → P4 re-panel. Every slice lands with a
discrimination test: inject the false-positive shape the fix targets and
assert it stays gone.
