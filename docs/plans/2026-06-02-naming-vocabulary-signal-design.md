# code_insight: naming-quality / VOCABULARY signal + metric fixes

Date: 2026-06-02. Branch: `feat/code-insight-real-data`.

## Goal

Reduce wasted semantic searches caused by symbols whose names use rare/
unintuitive terms for common operations (PHP `explode`=split, `implode`=join).
Surface a codebase's odd vocabulary so an AI agent learns it up front instead
of searching for the obvious word and missing the real symbol. Plus make three
existing code_insight metrics meaningful.

## Part A — metric fixes (Go-faithful where Go is right)

1. **MODULES `coupling=0.30`** was the `ModuleBoundary` placeholder (Go does
   it too, but it is a constant, not data). Wire real per-module coupling from
   `CouplingAnalyzer::analyze().coupling.module_coupling[pkg]` (keyed by
   `getPackageName`, same as the module name now), and recompute
   `metrics.average_coupling`. Diverges from Go's constant → documented +
   masked in the `mode-unified` MODULES detail (already masked).
2. **Purity effects breakdown** — `tally_purity` now also counts
   `with_io_effects / with_global_writes / with_param_writes / with_throws`
   from each `SideEffectInfo`'s categories (same logic as `side_effect_summary`),
   so code_insight's HEALTH purity block emits the `effects:` line that
   `side_effects` already exposes. Absorbed by the existing purity mask.
3. **Smell relabel** — `shotgun-surgery` is detected purely from
   `incoming_refs > threshold` = high fan-in, the opposite of shotgun surgery.
   Rename to `high-fan-in`. Go keeps the wrong label → normalized on both
   sides in the descriptors (`(shotgun-surgery|high-fan-in)` → token).

## Part B — VOCABULARY section (new, C++-ahead-of-Go)

### Detection (combined, per user)

Per function/method symbol, split the name (existing semantic splitter) →
tokens; leading token = verb. Two outlier triggers:

- **non-standard verb**: leading verb is in NO `SynonymTable` group. The table
  (expanded, see below) is the "known common vocabulary" oracle.
- **obscure token**: a name token that is (a) corpus-rare (appears in ≤2
  distinct symbols), (b) not in the standard vocab = SynonymTable ∪ a small
  common-word stoplist, (c) alphabetic, length ≥ 3 (skip ids/abbrev noise).

A symbol is a **vocabulary outlier** if it has a non-standard verb OR an
obscure token. Rank outliers by importance = `incoming_refs` (fan-in); only
surface important ones (fan-in ≥ threshold or exported) so private helpers
don't flood. When the odd verb maps to a synonym group, show the group's
common members as the suggested search term ("explode → split, join").

### Output (`== VOCABULARY ==`, in overview + unified)

```
== VOCABULARY ==
outliers=<n>
  explode (strings.php:12) fan-in=23 verb=explode -> split,join [o=XX]
  munge   (proc.go:88)     fan-in=9  obscure=munge [o=YY]
aliases_in_use:
  delete: erase(8), drop(3)
  get: fetch(12), load(5)
```

`aliases_in_use`: for each synonym group present in the codebase, which member
terms actually appear as symbol verbs + counts — tells the agent which word
THIS repo uses for a standard concept (search `erase`, not `delete`). Both
parts sorted deterministically.

### SynonymTable expansion (shared, per user — affects search too)

Add conflict-free cross-language groups to `build_default()`, e.g.
`{split, explode, tokenize, divide}`, `{join, implode, concat, concatenate}`,
`{map, transform, convert}`, `{filter, select}`, `{reduce, fold, aggregate}`,
`{list, enumerate, glob, scan}`, `{copy, clone, duplicate, dup}`,
`{format, render, stringify}`, `{compare, diff}`, `{lock, acquire}`,
`{unlock, release}`. Keep groups disjoint (every word ≤1 group; `marshal`
already in `{encode,serialize,marshal}`). Re-run synonym unit + parity tests.

## Parity implication

VOCABULARY has no Go equivalent. Populated descriptors mask the whole
`== VOCABULARY ==` block + the `tokens=` line (C++ enhancement; not counted in
Go's `estimateLCFTokenCount`, so I keep the Go token formula and exclude
VOCABULARY from it to preserve `tokens=` parity, and mask the section body).
All Part-A masks documented with `_rationale`.

## Verification

Build; 9 code_insight descriptors 10/10 stable; full mcp parity 33/33; synonym
unit/parity green; full unit suite green. Add an oddly-named symbol
(`explode`-style) to the populated corpus to exercise the detector.
