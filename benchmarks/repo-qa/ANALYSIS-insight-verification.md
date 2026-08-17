# code_insight verification vs independent manual exploration (2026-08-16)

Purpose: gate for the err-lookup repo-report feature. `code_insight` output
(unified + structure) was captured for all 11 `real_projects/` corpora; for a
3-repo beachhead (chi/Go small, guzzle/PHP low-minefield, pocketbase/Go
447-file high-minefield) an independent LLM explorer (Read/Grep only, no lci,
never saw the captures) produced a baseline, and a judge verified every
discrepancy against source. Oracle independence per
`.claude/rules/bench-harness-oracle-independence.md`; note the pre-existing
`repo-profiles.json` is computed BY lci (`scripts/profile_repos.py`) and is
therefore NOT a valid oracle for this purpose.

Artifacts (session scratchpad, regenerate via MCP stdio capture):
captures `<repo>.{unified,structure}.lcf`, baselines `<repo>.json`,
verdicts `<repo>.json` (19 + 13 + 18 findings).

## Verdict: mechanical censuses are accurate; every judgment-bearing section is compromised

Trustworthy across all three repos:
- Raw cyclomatic complexity rows (guzzle `applyHandlerOptions` cc=70,
  chi `findRoute` cc=37 — both hand-confirmed as the repos' real hotspots).
- File-type censuses and top_dirs in STRUCTURE.
- `aliases_in_use` vocabulary table.
- One genuine win: on pocketbase lci found a second `package main`
  (`plugins/jsvm/internal/types/types.go:1287`) the human baseline missed.

## Ranked defect list

### D1 — No test/example/vendored/generated segmentation (high, systemic)
Symbols from test, example, and vendored/minified code are analyzed as
production code and pollute nearly every section:
- chi: 12/12 shown ENTRY POINTS are `_examples/*/main.go`; example symbols in
  LOAD BEARING (#3), HEALTH high-fan-in (struct fields of a demo model),
  DEPENDENCIES (`_examples/custom-handler depended_on_by=359` — Go cannot
  import package main).
- guzzle: 4/12 ENTRY POINTS are PHPUnit test methods; top LOAD BEARING is a
  test-double method; `tests/Handler` classified `type=API Layer`.
- pocketbase: vendored minified `ui/public/libs/uplot/uplot.iife.js` supplies
  5/12 ENTRY POINTS (`e`, `t`, `l`, `n`, `el`), 100% of CYCLES, and 100% of
  STATISTICS high_complexity; `tests/` symbols in VOCABULARY and
  LAYER VIOLATIONS.
- Classification exists (`classify_module_by_path`, structure code/test
  counts) but is per-language-heuristic (PHP structure mode: `tests=0` on a
  repo with 39 test files) and applied nowhere else.

FIX (user-specified requirement): `.lci.kdl` directory/filename attribute
tagging (`test`, `example`, `vendored`, `generated`, `docs`) with sane
defaults per language; EVERY code_insight section filters to production by
default and segments/labels the rest.

### D2 — Name-collision reference resolution inflates reach (high, systemic)
Bare-name symbol resolution credits unrelated same-named symbols:
- chi: `writer` (5-line unexported accessor, 1 caller file) reach=441; 2-line
  demo handler `Get` reach=421.
- guzzle: test bootstrap shim `curl_setopt` reach=76; test-double
  `isSeekable` (0 callers) reach=88 top load-bearing.
- pocketbase: `NormalizeError` reach=2547 vs 6 real non-test references;
  `core.App`/`BaseApp`/`hook.Trigger`/`Record` absent from top-5.
LOAD BEARING and `depended_on_by` are not credible until resolution is
receiver/scope-aware or reach is computed over resolved edges only.

### D3 — Health/maintainability score saturation (high)
`score=10.00`, `debt=0.00` on all three repos, coexisting with the same
report's cc=70 function, risk=7 symbols, purity 0.27, and a 115-method god
object. The aggregate contradicts its own inputs; also
`smells: high-complexity=1` vs `distribution: high=3` in the same guzzle run.

### D4 — Internal count inconsistencies across modes/sections (med)
- dirs: chi 4 (structure) vs 18 (unified) vs ~21 (filesystem); guzzle 5 vs 9
  vs 11; pocketbase 12 vs 74 vs 174.
- symbols: chi 730 vs 857 vs 1233; guzzle structure symbols=16 vs unified 1205.
- pocketbase `core` 123 (unified) vs 134 (structure, correct).
- MODULES cohesion vs STATISTICS cohesion: 0.12 vs 0.34 (guzzle) — two
  different aggregations under one name.

### D5 — Entry-point ranking heuristic wrong for libraries (med)
`func main` / exported-“api” ranking buries the canonical library API
(`chi.NewRouter`, `Client::send`, `pocketbase.New`) under demo binaries,
test methods, and an elided "... and N more exported" tail.

### D6 — Vocabulary misses real defects, flags anti-signal (med)
Missed misspellings baseline found trivially: `SupressNotFound` (chi,
exported), `nullifyMisingField` (pocketbase, 8 occurrences), `isSeperatorRune`
(pocketbase); missed guzzle's snake_case `add_*` family (11+ methods in a
camelCase codebase). Meanwhile flags core API verbs (`Use`, `Mount`, `Group`)
and domain words (`seekable`, `effective`) as outliers. Caught 1 of 4 real
typos (`marhshalWithoutEscape`, labeled unknown-verb). Cosmetic: outlier
locations render as `(:220)` with no filename.

### D7 — Dead or unactionable graph outputs (med/low)
- brokers: betweenness=0.00 for every symbol on every repo (metric unwired or
  graph broken).
- CYCLES: bare symbol names, no membership/paths; on pocketbase 100% vendor
  artifacts; Go import cycles impossible.
- LAYER VIOLATIONS: chi both rows false positives (middleware pattern tagged
  as violation); pocketbase 4/8 are Test* functions.

## Fix order
D1 (config tagging + section-wide enforcement) → D2 (resolution/reach) →
D3 (score de-saturation) → D4 (single source of truth for counts) →
D5/D6/D7. Re-verify after fixes with four independent evaluators: this
harness (Claude) + glm-5.3 + opencode/kimi-k3 + codex 5.6-sol medium
(configs exist in err-lookup `configs/`), then extend to err-lookup's real
corpus and only then wire the report into err-lookup `RepoEntry`.
