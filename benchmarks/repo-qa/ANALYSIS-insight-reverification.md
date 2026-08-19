# code_insight re-verification after D1–D7 fixes (2026-08-19)

Follow-up to `ANALYSIS-insight-verification.md` (2026-08-16). All seven ranked
defects were fixed and merged to main by 2026-08-19 (full gate 2236/2236
green), plus the new `== ERROR HANDLING ==` and `== RESOURCE MANAGEMENT ==`
sections. This round re-captures fresh `code_insight` output (unified +
structure, MCP stdio, binary at `aef284f`) for the same three beachhead repos
and verifies it with a multi-model evaluator panel, per the plan recorded in
the 2026-08-16 report.

## Evaluator panel

| Arm | Model | chi | guzzle | pocketbase |
|---|---|---|---|---|
| harness | claude-fable-5 (Explore subagent, Read/Grep only, lci-blind) | done | done | done |
| codex | openai/gpt-5.6-sol via opencode | done | done | done |
| kimi | kimi-for-coding (K2.7) via opencode | done | done | done |
| glm | glm-5.3 | FAILED | FAILED | FAILED |

The glm arm never produced a verdict: `zai-coding-plan/glm-5.3` was blocked by
Z.AI's fair-usage rate limit on every attempt, and the `opencode-go/glm-5.3`
fallback timed out at 900 s on all three repos plus a final 1740 s attempt on
chi. The panel is therefore 3 independent evaluators × 3 repos = 9 verdicts,
not the planned 12. Convergence across the three arms is high enough that the
missing arm does not change any conclusion below; every residual defect named
here was found independently by at least two arms.

Raw artifacts (session scratchpad, regenerate via `capture_insight.py` +
`run_evaluators.py` there): captures `<repo>.{unified,structure}.lcf`,
verdicts `<repo>.{claude,codex-sol,kimi}.{json,txt}`.

## What the fixes actually bought (D1–D7 disposition)

- **D1 file-level segmentation: FIXED.** All three repos report
  `excluded_from_analysis` and no symbol from `*_test.go`, `_examples/`,
  `tests/`, or vendored `uplot` appears in any section. The pocketbase
  single-letter vendor entry points and vendor CYCLES/STATISTICS pollution
  are gone. All 3 arms confirm on all 3 repos.
- **D1 residue — call-EDGE contamination: NOT fixed** (see R2 below).
  Excluded files no longer contribute *symbols*, but their *references* still
  count: guzzle `MockHandler::getLastRequest` ranks entry-point #2 and
  high-fan-in [high] on 100% test-only call sites; chi reach numbers match
  test-inclusive counts (kimi: "InsertRoute has only 1 production caller but
  appears as a high-fan-in entry point").
- **D2 name-collision reach: NOT fixed** (R1 below — the dominant residual).
- **D3 health-score saturation: FIXED for HEALTH** (scores now 8.33 / 7.05 /
  7.39 and hand-confirmed hotspots still rank), **but the defect re-shipped
  inside the two new sections** (R3 below).
- **D4 count inconsistencies: mostly fixed**, residue remains: guzzle module
  file counts (18 reported vs 45 on disk), pocketbase HEALTH
  high-complexity=17 vs STATISTICS high=26, file/language censuses slightly
  off (kimi).
- **D5 library-first entry points: FIXED on chi** (NewRouter/Get/Use/Mount
  lead the list), **partial elsewhere**: pocketbase still omits
  `pocketbase.New`/`Start` from the shown 12 and lists an internal codegen
  `main` while dropping `examples/base/main.go`; guzzle ranks
  `CurlFactory::create` and a test-double accessor above `Client::send`.
- **D6 vocabulary: NOT fixed where it matters** (R4 below). The machinery
  now emits misspelling/convention-mismatch signals, but all three named
  target defects from the 2026-08-16 round are STILL missed and the new
  signals are mostly false positives.
- **D7 brokers/cycles: partial.** Betweenness is now non-zero and chi's
  `handle` (mux.go:416) betweenness=1.00 is a genuine, hand-confirmed
  articulation point (all 3 arms). But guzzle's broker ranking inverts the
  real topology (`setCookie` 1.00 vs `sendAsync` 0.33), and CYCLES is now
  12/13 findings false (R2).

## Residual defects, ranked

### R1 — Name-collision symbol resolution still inflates reach/fan-in/dependencies (high, systemic, unanimous)
The single root cause behind most remaining wrong numbers. Bare-name
aggregation credits same-named symbols across receivers/packages/stdlib:
- chi: `Middlewares.HandlerFunc` reach=68 vs **2** production call sites
  (conflated with `http.HandlerFunc`, 34 files); `endpoints.Value` reach=49
  vs **4** (conflated with `context.Context.Value`).
- pocketbase: `GetOk` reach=322 vs **4–5** call sites (5-line generic map
  accessor); `GetRaw` 311 vs 55; `NewTaggedHook` 295 vs 66; DEPENDENCIES
  `core depended_on_by=1820` vs 165 importing files; `tools/router` 710
  vs 30 (kimi counted).
- guzzle: private one-line `invalidBody` reach=8 vs 2 callers.
LOAD BEARING, high-fan-in smells, DEPENDENCIES counts, and CYCLES all corrupt
from this one defect. Fix direction (all arms agree): disambiguate by
receiver/package before computing reach, cycles, and fan-in; compute reach
over resolved edges only.

### R2 — CYCLES: 12/13 findings are delegation artifacts (high)
Every chi cycle (5/5) and guzzle cycle (3/3), and 4/5 pocketbase cycles, are
same-method-name delegation across distinct types (decorator/embedding/
interface dispatch: `compressResponseWriter.Write` → wrapped writer,
`CookieJar::toArray` → `SetCookie::toArray`, `apis.Serve` →
`http.Server.Serve`). The only real cycle found (pocketbase
`ReadFrom`→`WriteTo`) is documented in source as deliberately handled. The
section is non-actionable as shipped. Same root cause as R1.

### R3 — New ERROR/RESOURCE sections ship the D3 defect they were born after (high)
Score formulas are insensitive to their own findings:
- chi: ERROR score=10.00 with handled_ratio=0.00; RESOURCE score=10.00 with
  acquisitions=0 *and* guarded_ratio=0.50 (ratio over zero acquisitions).
- guzzle: ERROR 9.88 with two [high] api-reachable swallows and
  handled_ratio=0.23; RESOURCE 10.00 with acquisitions=0 while `curl_init`,
  `curl_multi_init`, `fopen` (and their releases) sit in source.
- pocketbase: ERROR 9.96 with 98 findings/53 swallows/handled_ratio=0.10;
  RESOURCE 10.00 contradicting its own released_ratio=0.76.
Detection quality is also mixed both directions:
- Misses: curl/stream lifecycle (guzzle), sync.Pool acquire/release (chi),
  chi's gzip/flate constructor-error swallows (codex found real swallow
  sites the section reports as swallow_sites=0).
- False positives: guzzle `processMessages` classified catch-and-continue
  though it `reject($e)`s the deferred (kills the derived api-reaches-swallow
  paths); deliberate idioms flagged [high] (destructor must-not-throw,
  redaction fallback); pocketbase UI `app.modals.open` counted as unreleased
  resource acquisitions (all 4 leak findings false).
Individually real findings do exist (pocketbase `_ =` drops confirmed by 2
arms) — the aggregates and the acquisition model are what's broken.

### R4 — Vocabulary still misses the target misspellings, emits false ones (med-high)
Still missed, second round in a row: `SupressNotFound` (chi, exported API +
filename + doc comment — the highest-value naming defect in the corpus),
`nullifyMisingField` (pocketbase, 8 usages), `isSeperatorRune` (pocketbase),
guzzle's snake_case `add_*` family (StreamHandler.php:756–926) +
`src/functions.php` snake_case free functions in a camelCase codebase.
Meanwhile the section spends its budget on false positives: chi
`NewCompressor→compress` and `Panic→panicf`, guzzle `transfer→transform`
(core HTTP domain word), pocketbase `HasSubscription→subscriptions` (correct
singular predicate), plus domain nouns (`expires`, `secure`, `promise`,
`tick`) flagged as obscure. Only true hit across 3 repos:
`marhshalWithoutEscape`. Likely cause: outlier list is fan-in-gated/capped,
so low-fan-in-but-exported misspellings never surface, and the dictionary
check runs on whole names rather than camelCase-split tokens.

### R5 — Entry-point visibility gate missing (med)
`api:` tags symbols by exported *spelling*, not reachability: chi lists
methods on unexported receivers (`endpoints.Value`, `*node.InsertRoute`,
`*compressResponseWriter.Close`); pocketbase lists a nested local JS function
and an object-literal getter; guzzle leads with an internal factory and a
test-double accessor while `Client::__call`'s magic verb surface and
ClientTrait's 15+ HTTP-verb methods collapse into "… and N more exported".
`binaries:` lists pocketbase's internal codegen tool and drops the real
`examples/base/main.go` (example-tagging dropped the binary instead of
labeling it).

### R6 — Guzzle cross-module dependency graph unbuilt (med)
coupling=0.00/max=0.00, `src depended_on_by=1` on a tree where Handler/*
imports root Utils/Client and Cookie is consumed by Middleware — 2 arms
independently conclude the module graph was never constructed for PHP;
MODULES cohesion and CLUSTERS modularity are then computed over that empty
graph. Also guzzle module file counts wrong (18 vs 45 PHP files under src/).

### R7 — Cosmetics/quality (low)
Blank symbol names in chi CLUSTERS c0 (`HandlerFunc, , `); cluster membership
frequently semantically incoherent (all arms, all repos); risk score flat at
7 across cc=28/33/70 (no triage discrimination).

## What is now trustworthy

Consistent across all arms and repos: complexity hotspots and their rankings
(chi `findRoute` cc=37, guzzle `applyHandlerOptions` cc=70, god-class flags),
file-type/dir censuses, file-level test/example/vendored exclusion, module
split + dependency *direction*, chi's top broker, and individually-cited
`_ =` dropped-error sites. The mechanical layer has held since round 1 and
the D1 file-level fix genuinely landed.

## Gate verdict

**Still not ready to wire into err-lookup RepoEntry.** The mechanical layer
is solid, but every judgment-bearing consumer surface (reach, brokers beyond
chi, cycles, error/resource scores, vocabulary, entry-point api tags) fails
independent verification, mostly from two roots: bare-name symbol resolution
(R1 → R2, parts of R5/R6) and evidence-insensitive score formulas (R3).
Fix order: R1 (resolution) → R3 (score formulas + acquisition model) → R4
(token-split spellcheck + drop the fan-in gate for exported symbols) → R5
(visibility gate) → R6 (PHP module graph) → R7. Re-run this same panel after
R1+R3 before the err-lookup real-repo round.
