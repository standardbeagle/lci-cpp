# code_insight re-panel after R1/R2/R4 fixes (2026-08-26)

Follow-up to `ANALYSIS-insight-reverification.md` (2026-08-19). This round
verifies the graph-precision and vocabulary work that landed 2026-08-26
(`40e4cb4` foreign-receiver resolution gates, `d8a24a0` SCOWL dictionary +
Porter2 morphology, `3caeda5`/`42b42b6` section tuning), against the same
three beachhead repos, fresh MCP-stdio captures (unified + structure).

## Evaluator panel

| Arm | Model | chi | guzzle | pocketbase |
|---|---|---|---|---|
| harness | claude-fable-5 (Explore subagent, Read/Grep only, lci-blind) | done | done | done |
| luna | opencode-go/gpt-5.6-luna | done | done | TIMEOUT ×2 (900 s, 1500 s) |
| kimi | (all routes) | FAILED | FAILED | FAILED |

The kimi arm was unobtainable on every route: kimi-for-coding quota
exhausted for the billing cycle, alibaba-coding-plan token expired,
opencode-go/kimi-k3 and baseten/Kimi-K3 timed out at 900–1200 s on all
agentic runs (a plain prompt probe succeeded, so the stall is in
tool-use runs, not auth). Panel = 5 verdicts. chi and guzzle findings
below carry two independent arms; pocketbase findings carry one and are
marked (1-arm).

Raw artifacts in the session scratchpad: `capture_insight.py`,
`evaluator_prompt_template.md`, `<repo>.{unified,structure}.lcf`,
`<repo>.{luna,kimi}.txt`, plus the three subagent verdict JSONs.

## Disposition of the 2026-08-19 residuals

### R1 name-collision reach inflation — FIXED (unanimous where covered)

- chi: all top-4 LOAD BEARING entries reconstructed by hand by both arms;
  `Chain` reach=24 / `handle` reach=19 match real transitive cones;
  `Middlewares.HandlerFunc`=68 and `endpoints.Value`=49 inflations gone.
- guzzle: 28/28 cited file:lines verified; no name-collision inflation;
  brokers (`requestAsync`/`sendAsync` betweenness=1.00) hand-confirmed.
  If anything reach now UNDER-counts (ClientTrait verb methods not
  credited through the abstract declaration).
- pocketbase (1-arm): `GetOk` reach=322 gone entirely; `GetRaw` 311→147 vs
  53 direct production sites (plausible transitive); one residue:
  `dualDBBuilder.Select` reach=125 likely absorbs external `dbx` builder
  calls (unindexed-library edge class).
- Open residue: DEPENDENCIES `depended_on_by` (import-graph based, not
  call edges) still inflated — pocketbase `core` 1820→1388 vs ~165
  importing files. Different mechanism, untouched by the call-edge fix.

### R2 fake cycles — PARTIAL

Fixed: every single-node false self-loop is gone; the new `recursion=`
line is 12/12 correct on spot-check (chi `addChild`/`findRoute`/
`findPattern`, pocketbase 4/4); guzzle fabricates nothing.
Open: multi-node same-method-name delegation chains survive, because the
foreign-receiver gate kills wrong SELF-edges but same-name method chains
across distinct wrapper types still link (chi `Flush -> Flush -> Flush`
and `Hijack` across wrap_writer types, 2 of 3 cycles; pocketbase 3 of 5
delegation + 1 outright false via stdlib `maps.Clone` name collision;
genuine: chi `walk`, pocketbase `arrVal/extractNestedVal/mapVal`).
New gap found: guzzle emits NO cycles/recursion despite two real
`self::` recursions in Client.php — PHP static-self recursion apparently
produces no resolvable self call edge at all.

### R4 vocabulary noise — IMPROVED on Go, BROKEN on PHP

- Fixed: fail→tail-class corrections gone; chi catches its one real
  exported misspelling (`SupressNotFound`) with only 3 mild FPs;
  aliases_in_use verified accurate and useful on all repos.
- Broken (guzzle): 14/15 outliers are PHP magic methods
  (`__construct`/`__call`/`__destruct`) flagged `convention-mismatch=
  snake_case` — language-mandated names must be exempt. The repo's real
  defect (StreamHandler's 11 snake_case `add_*`/`parse_proxy` privates
  beside camelCase siblings) is missed by both arms' account. (Luna
  nuance: `functions.php` snake_case globals are documented deprecated
  compat wrappers — not defects.)
- Open (pocketbase, 1-arm): obscure-token fires on standard technical
  acronyms/terms (PKCE, HMAC, gzip, subnet, keydown, filepath, goja,
  pseudorandom, backtick) — an acronym/tech-term allowance is still
  needed; `isSeperatorRune` (tools/tokenizer) missed a third round.

### R5 entry-point visibility gate — STILL BROKEN (both arms, all repos)

The worst remaining section. Trivial-name demotion helped, but `api:`
still tags by exported spelling, not user reachability: chi surfaces
methods on unexported receivers (`node.InsertRoute`,
`basicWriter.Discard`) while `NewMux`/`URLParam`/`Use`/`Get`/`Route`/
`Mount` hide in "…more exported"; guzzle leads with MockHandler TEST
doubles and cookie accessors while `Client::send`/`sendAsync` (its own
brokers at 1.00 — a self-contradiction), `HandlerStack::create`,
`Pool::batch`, and the Middleware factory family are absent; pocketbase
(1-arm) promotes anonymous admin-UI JS object getters and a generated
type-stub `main` while `pocketbase.New`/`Start`/`Execute` are missing.

### R3 error/resource score saturation — NOT RETESTED

Tier-1 unified output no longer carries those sections (beta-gated);
this round could not evaluate them. Still open per the plan.

### R6 guzzle module graph — CONFIRMED STILL BROKEN

`coupling=0.00 max=0.00` while 17 cross-module `use GuzzleHttp\...`
imports are grep-verifiable.

## New findings this round

1. PHP purity misclassification: guzzle `global_writes=725` in a codebase
   with zero `global`/`$GLOBALS` statements — `$this->` property mutations
   counted as global writes, driving ratio=0.26.
2. Denominator disagreements persist (D4 residue): guzzle symbols=447 vs
   complexity distribution 360 vs purity total 1033; chi 439 vs 235 — the
   scopes differ without saying so.
3. Score-frame tension: `score=7.12/10` beside `maintainability=92.39/100`
   and `debt=0.01` with 4 god-classes reads as two unrelated instruments
   (labels landed; calibration didn't).
4. pocketbase (1-arm): a nameless closure in high_complexity
   (middlewares_cors.go:172 anonymous handler) — unactionable finding;
   STATISTICS prints absolute paths where every other section is
   relative; declared exclusions (ui/ vendored, plugins/ generated) still
   supply entry points, clusters, and vocabulary outliers — the
   excluded-attribute contract is inconsistent across sections.

## Gate verdict

The numeric spine is now trustworthy: reach, brokers, health hotspots,
problematic_symbols, and the recursion list all survive independent
adversarial verification on two arms. The judgment-section gate stays
CLOSED on five items, ranked: (1) entry-point visibility/reachability
gate (R5), (2) PHP handling — magic-method exemption + `$this->` purity
class + `self::` recursion edges, (3) same-name delegation chains in
CYCLES (needs receiver-typed edge resolution, not just self-gating),
(4) denominator/scale coherence (D4), (5) R3 re-test once the error
report leaves beta. Fix (1)–(2) and re-run this panel; the harness for
one round is ~30 min with the Claude arm alone.

## Addendum — same-day fixes and single-arm regression check

The R5 and PHP-triad items were fixed the same day and re-verified by an
independent lci-blind auditor against a fresh guzzle capture:

- ENTRY POINTS: FIXED. Three-tier grounding shipped (.lci.kdl
  `insight { entry_points }` / `@lci:entry` -> annotated; framework
  signature registry matched via go.mod/composer.json/package.json ->
  framework; otherwise a labeled heuristic that asks for annotations).
  Guzzle now leads with Client::send/request, the ClientTrait verbs, and
  HandlerStack::create under `confidence=framework`; MockHandler is gone.
- Magic methods: FIXED (42 dunder declarations in guzzle src, zero
  flagged) and the previously-missed StreamHandler add_*/parse_proxy
  snake_case cluster now flags via the per-language repo-level
  convention fallback.
- PHP recursion: both real self:: recursions listed; the verifier's one
  new false positive (Psr7\Utils::modifyRequest name-collision) was
  fixed by marking explicit-class scoped calls foreign.
- global_writes: root cause was systemic, not PHP-specific —
  add_local_variable had NO callers, so reassigned locals counted as
  global writes in every language. Declaration + loop/catch-binding
  registration and blank-identifier discard landed: guzzle 725->113,
  chi ->53, pocketbase 1943->426. Residual: the counter still includes
  some non-global state (by-ref params, list destructuring); rename or
  further tighten before treating it as shared-mutable-state evidence.
- Open items for the next full panel: exported-count inflation in
  "... and N more exported" (guzzle claims 360 exported vs ~202 public
  functions), acronym allowance for obscure-token (PKCE/HMAC/cidr),
  delegation-chain cycles, D4 denominators, R3 beta re-test.

