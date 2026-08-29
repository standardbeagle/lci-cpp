# code_insight multi-repo QA sweep (2026-08-29)

Five repos, every mode (overview, detailed×{modules, layers, features, terms,
errors, resources}, statistics, unified, structure, git_analyze, git_hotspots),
driven through one-shot stdio MCP with the release binary at HEAD (fca8961).

| Repo | Language | Scale | Result |
|---|---|---|---|
| pgvector | C | 27 files | all modes answered; 2 broken, 7 suspect |
| age (Apache AGE) | C + drivers | mid | all modes answered; 4 suspect |
| next.js | TS/JS | 19,293 files / 87k symbols | serving-layer blockers; content mostly ok |
| dotnet/runtime corpus | C#/C++/C | 950MB (budget-capped to 15,810 files) | serving + env blockers; content mostly ok |
| lci-cpp (self) | C++ | — | readiness flapping; git_analyze wrong range |

No crashes, no NaN, clean stderr (except one honest corpus-budget warning),
well-formed LCF everywhere, clean enumerated errors for invalid mode/analysis.
Overview/unified/statistics/structure/vocabulary content and the
error-handling FINDINGS lists are largely real and verifiable. The defects
below are ranked by severity.

## 1. Serving lifecycle — BLOCKER for one-shot agent clients

- A cold one-shot `lci mcp` call on a large repo can NEVER succeed: the
  process builds the index inline (minutes, ~2GB RSS on dotnet), dispatches
  the queued `tools/call` immediately, answers `-32603 index unavailable:
  index is still building; retry shortly`, then exits and DISCARDS the build.
  Three consecutive attempts on dotnet burned 282–436s each and produced
  nothing. `lci mcp` neither attaches to an already-running socket server for
  the same root nor waits for its own build before dispatching.
- Socket-generation collision (next.js): three lci processes held LISTEN on
  the same `/tmp/lci-1000-*.sock`; `lci status` said Ready while MCP clients
  got "still building" (different generations of the socket); 78 stale sock
  files in /tmp; `lci shutdown -r <root>` could not find a live auto-spawned
  `lci --root <root> server` — manual kill required.
- Self-repo: "index is still building" errors flapped nondeterministically
  for ~5 minutes interleaved with successes (possibly aggravated by the 5
  concurrent sweeps; still, readiness gating is not monotonic).
- Error-surface inconsistency: index-unavailable is a protocol-level error
  (-32603) while bad params are tool-level isError results.

## 2. LCI_ERROR_REPORT=on env override is dead — errors/resources unavailable

On next.js and dotnet, `analysis=errors|resources` returned
`available=false ... (insight.error_report=capture)` with a hint naming the
exact env var that WAS set on the process. The override is either dead code
or read somewhere other than the hint claims. (Worked on pgvector/age/self,
where the one-shot process built its own index.)

## 3. layers analysis is broken on every repo

- `modules=` counts SYMBOLS, not modules: Utility Layer claimed 2,594
  (pgvector, 27-file repo), 8,025 (age), 15,961 (self), 32,837 (next.js,
  more than its file count), 440,917 (dotnet) — modules mode says totals of
  25–817.
- All four architecture patterns (Layered, Microservices, MVC, Repository)
  are always detected, each at exactly confidence=0.80 — a constant, not a
  measurement. "Microservices" for a Postgres C extension / dotnet runtime.
- `violations=1` contradicts overview's LAYER VIOLATIONS count (7 on self,
  8 on dotnet).

## 4. Score saturation (confirms residual R3 across repos)

- error_handling: pgvector scored a perfect 10.00 with throwers=0,
  handled_ratio=0.00 — the C classifier sees no `ereport`/`elog` (Postgres
  error idiom), and zero signal defaults to a perfect score instead of
  "insufficient data". age scored 9.98 beside 2 high-severity findings; self
  9.99 beside 32 findings.
- resources: 10.00/9.99 on every repo while released_ratio ran 0.45–0.93 and
  guarded_ratio=0.00; on age only 13 acquisitions were found in a
  palloc/pfree-heavy codebase (detector blind to PG memory contexts). The
  detailed mode adds nothing over the overview section and its `next:` hint
  points at itself.
- features: avg_cohesion 0.98–1.00 saturated everywhere; labels degenerate
  ("General Feature" dominates; business-domain labels like "E-commerce" /
  "Product Management" for a graph DB extension; raw symbol names as
  feature names; dep rows with strength=0.00 emitted).

## 5. Cross-mode metric contradictions (all repos)

Same repo, same session: modules cohesion=0.05–0.17/coupling=0.30 vs
statistics cohesion avg 0.68–1.00/coupling 0.00–0.02 vs unified
`name_cohesion` (field renamed between emitters). pgvector statistics is
fully degenerate (coupling avg=max=0.00, cohesion avg=min=1.00). At least
one of the emitters is wrong, and none label their definitions.

## 6. git_analyze defects

- Change-type breakdown never reconciles: `files_changed=1..3` with
  added=modified=deleted=0 (pgvector, age, next.js) or modified=7 >
  files_changed=3 (self).
- Self-repo: scope=commit base_ref=HEAD~1 analyzed content NOT in the range
  (findings in src/git/analyzer.cpp for a docs-only commit) — wrong-diff or
  stale-cache defect, needs a dedicated look.
- Missing ref (shallow clone, dotnet): returns raw JSON
  `{"error":"git change analysis failed"...}` — not LCF, and hides git's
  actual "unknown revision" cause.
- String `focus` was accepted everywhere (array not needed).

## 7. Determinism (karpathy rule 4)

CLUSTERS differ between overview and unified on the same unchanged corpus
(pgvector: c0 size 46 vs 47, different c1 exemplars) — Louvain community
assignment is not deterministic across invocations. age saw differing
high_complexity top-3 lists between statistics and unified.

## 8. Smaller, consistent nits

- terms: `count` == `terms` in every row on every repo — one field is
  redundant. Category classifier throws "Database"/"Authentication"/
  "HTTP/API" at repos with none of those.
- Absolute host paths leak in massive_files / high_complexity while every
  other section is repo-relative (same class as the goldens-portability
  rule).
- C headers counted as cpp (pgvector: cpp=6 in a zero-C++ repo; age
  similar); structure file-type census doesn't reconcile with SUMMARY langs
  and category sums miss files silently (self: 4,288 benchmark files
  uncategorized; dotnet: hotspots files_analyzed counts skipped files).
- statistics exemplars ignore the shipping attribute filter (vendored zstd
  tops dotnet high_complexity).
- TS entry-point heuristic is noise on next.js (`import`, `React`, `push`);
  misspelling suggestions include false corrections (usize→size,
  iframe→frame); PascalCase React component flagged as case violation.
- git_hotspots empty-window output is correct but gives no "window predates
  HEAD" hint; self-repo hotspot list not sorted by the displayed field.

## Suggested fix order

1. Serving: make `lci mcp` attach to an existing server for the root, or
   block tool dispatch until its own index is ready; fix shutdown discovery
   of auto-spawned servers; reap stale socks. (unblocks everything else)
2. LCI_ERROR_REPORT env override.
3. layers: count real modules, derive pattern confidence or delete the
   pattern list (replace-and-remove).
4. Score floors/saturation (R3) + zero-signal "insufficient data" path;
   C error-idiom classifier (ereport/elog) and PG resource idioms.
5. Unify cohesion/coupling definitions across modes; git_analyze count
   reconciliation + wrong-range investigation; determinism of clusters.

Raw outputs: scratchpad `out_*.txt` / `age_*.txt` / `dn_*.txt` per sweep
session (transient).
