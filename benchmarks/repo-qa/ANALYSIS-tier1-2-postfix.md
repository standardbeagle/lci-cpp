# Tier-1 re-run after the tier0-comb fixes (817ea43)

Continues the ladder from `ANALYSIS-tier0-postfix.md` (f81ac6e). Binary under
test: `build/src/lci` at **817ea43** — the four tier0-comb fixes: leading-`**/`
globs match at root, search validation errors carry `allowed_params`,
`path=` alias on find_files/browse_file, `get_context` emits a `callers`
count. Results in `results/tier1-postfix`, facts scored with
`judge.py --skip-llm` (judge-independent). Comb tooling: `scripts/comb.py`.

## Provider reality (2026-07-06 早)

Old tier-1 models (gemini25f, qwen3coder via openrouter/copilot) were not
re-probed — the tier0-postfix finding stands that the copilot/openrouter legs
are dead on this plan. Probed fleet: **godsv4pro, gokimi27** (opencode-go) and
**dsv4free, mimo** (zen free tier) all answered smoke runs; **northmini**
produced nothing in 90 s and was dropped pre-run.

Mid-run (~05:00) the opencode zen + go providers entered a hard throttle
(PONG probe itself timed out). Consequences, all recorded as
`tools=0, wall=600` provider stalls — the model never produced a single
event:

- **mimo-lci**: 8/18 chi runs timed out → mimo kept only as a base-side row.
- **gokimi27 sinatra-base**: 3 timeouts; sinatra-lci never started for the
  go models before harvest.
- Run was harvested at 161/288 per the "don't wait hours for throttled
  stragglers" directive. **Complete base+lci grids exist for chi** (18 runs
  per cell, reps=2) for godsv4pro, gokimi27, dsv4free — the headline uses
  those. Sinatra rows are partial (base-only) and excluded from deltas.
  Tier 2 was not attempted against the throttled fleet.

## Headline (chi, complete cells only, facts = deterministic)

| model | variant | n | facts | tokens (avg) | wall s | tool calls | lci calls |
|---|---|---:|---:|---:|---:|---:|---:|
| godsv4pro | base | 18 | 0.900 | 51,518 | 47.4 | 3.4 | 0 |
| godsv4pro | **lci** | 18 | 0.887 | 120,298 | **44.4** | 7.3 | 4.2 |
| gokimi27 | base | 18 | 0.880 | 93,406 | 45.9 | 5.8 | 0 |
| gokimi27 | **lci** | 18 | **0.913** | 155,726 | 54.5 | 7.9 | 4.0 |
| dsv4free | base | 18 | 0.848 | 34,324 | 47.3 | 1.5 | 0 |
| dsv4free | **lci** | 18 | 0.850 | 120,360 | **35.5** | 6.7 | 3.1 |
| mimo | base | 18 | 0.898 | 50,240 | 54.5 | 3.3 | 0 |
| mimo | lci | 1+8TO | — | — | — | — | dropped (throttle) |

Reference deltas:

- **vs tier0-postfix2, same models, chi-only, reps=1**: godsv4pro-lci 0.900→0.887
  (noise), gokimi27-lci 0.881→**0.913** (+0.032, and now **above its own base**
  0.880 — in tier0 the lci variant trailed base by 0.06). Wall and tokens are
  within run-to-run variance.
- **vs old `results/tier1`** (gemini25f/qwen3coder, crippled binary): no model
  overlap — structural comparison only. Old tier1-lci *lost* facts vs base
  (qwen3coder 0.65→0.61, gemini25f flat 0.61) with find_files 100 % dead.
  Post-fix, lci is at parity or better on every runnable model.
- **vs old `results/tier2`** (same crippled-binary caveat): its 0.90-0.92 facts
  band for good models matches the post-fix band here; the difference is that
  lci no longer pays an accuracy tax (old tier2: deepseekv4-lci −0.02,
  qwen3coder-lci −0.03).

The recurring shape holds: facts at parity-to-positive, wall time down for
models that replace broad reads with targeted search (dsv4free −25 %,
godsv4pro −6 %), tokens up (tool schemas + result payloads ride every turn).

## Did the four fixes get exercised? (55 traced lci runs, chi)

| fix | evidence this run | tier0-postfix2 |
|---|---|---|
| A. `**/` globs | **7 calls, 6 returned content** — incl. `**/chain.go` → 120ch HIT, the exact tier0 miss (`find_files **/chain.go → 56ch empty → fell back to read`). The 1 miss was the new path-scope bug, not the glob. | 3 calls, root-level forms all empty |
| B. search error recovery | 5 genuine search errors, **3 carried an allowed-list, 5/5 recovered in-run** (next search succeeded). tier0's `paths` thrash (3 retries, never recovered, worst run of the tier) did not recur. | 4 unrecoverable `paths` errors, 0 named the allow-list |
| C. `path=` alias | **6 uses** on find_files/browse_file, 0 unknown-param errors (tier0 direct probe: `browse_file path=` errored). `browse_file path=mux.go` → 7,146ch outline. But 5/6 find_files uses returned empty — see finding 1. | 0 uses possible (param rejected) |
| D. get_context callers | **23 calls** (2.5× tier0's 9), median 482ch payload, every one carrying the new `callers` field (verified by live probe: `name=ServeHTTP → callers:107`). | 9 calls, no callers in default payload |

Search scoping adoption roughly doubled alongside: `path=` **70** uses
(tier0: 35), `filter=` **26** (16), `include=` **48** (36× plain
`object_ids`). bash-explore fallback after an lci call: **2/55** runs
(tier0: 1/36) — LCI keeps answering.

## Comb findings, ranked

### 1. HIGH (new) — find_files silently returns 0 for `.` / `/` / absolute / wrong-dir scopes
**8 of 8 near-empty find_files calls this run are one root cause.** Models
naturally scope "the whole repo" as `path="."`, `directory="/"`, or the
absolute workspace path; find_files treats all of these as no-match and
returns a bare `{"results":[],"total_matches":0}` with **no hint**:

```
gokimi27 chi-m3:  {"pattern":"*.go","path":".","max":30}          -> 49ch empty
dsv4free chi-h1:  {"pattern":"*.go","directory":"/"}              -> 49ch empty
dsv4free chi-h2:  {"pattern":"**/*.go","directory":"/home/.../chi-lci"} -> 52ch empty
dsv4free chi-h1:  {"pattern":"tree.go","path":"chi"}              -> 52ch empty (repo name as dir)
```

Direct probe confirms: `pattern=*.go` alone → 74 matches; the same pattern
with `directory="."` → **0**. `search` already handles `path="."` correctly
and emits a hint + dirs histogram on bad paths — find_files should normalize
`.`/`/`/workspace-absolute prefixes to repo-root (i.e. no filter) and answer
a nonexistent directory with the same hint style search uses. This is the
direct successor to the tier0 `**/` bug: the most natural spelling of
"everywhere" still silently misses, one layer up.

### 2. MED — `include=` add-on vocabulary doesn't match model intuition
The new top param-error class (5 errors, replacing `paths`). Models pass
`include=signature,ids` (inspect_symbol vocabulary), `include=*.go`
(filter semantics), or stuff the regex into `include` and omit `pattern`
entirely:

```
godsv4pro chi-m3: include='signature,ids' -> "include='signature' is not a recognized
                  search add-on. Allowed: object_ids, breadcrumbs, refs, safety, deps."
gokimi27 chi-m3:  include='*.go'          -> same allow-list error
godsv4pro chi-m3: {'include':'func.*Mount'} (no pattern) -> "pattern cannot be empty"
```

The fix-B error text made all five recoverable (5/5 next-search success,
zero grep fallback) — so this is now UX cost (~1 wasted round-trip each),
not a correctness hole. Cheapest wins: accept `signature` as an include
add-on (models plainly want signatures inline; inspect_symbol already emits
them), and alias `ids`→`object_ids`.

### 3. LOW-MED — `search output=files` drops the empty-result hint
Plain search with 0 matches returns the helpful `hint` + `similar_symbols`;
`output=files` mode returns a bare `{"files":[],"showing":0,...}` (59ch):

```
gokimi27 chi-h2: {"pattern":"404|405|NotFound|...","path":"chi","output":"files"} -> 59ch
```

(Also note: that call's real problem was `path:"chi"` — finding 1 — and the
missing hint hid it.) Emit the same hint object in files-mode empties.

### 4. LOW — read-after-search re-reads
174 `read` calls across 55 runs; **22/55 runs re-read the same file** (paged
or repeated). Expected behavior (search locates, model quotes), unchanged
from tier0 (20/36). Not a tool defect; a larger default context window on
`browse_file`/`get_context` sections is the only lever and cuts against
payload discipline — leave.

### 5. Noise retired
tier0's finding 5 (success payloads tripping the harness `"error"` substring
heuristic) produced **0 false positives this run** — comb.py now
distinguishes genuine `success:false`/`validation_errors` payloads, and all
5 flagged searches were genuine.

## Ranked next tool improvements

1. **find_files: normalize `.` / `/` / absolute-workspace scopes to repo root,
   and hint on nonexistent directories** (finding 1). Every observed
   find_files failure this run — 8/8 — is this one behavior. Mirror search's
   empty-result hint.
2. **search include=: accept `signature` (emit signatures inline) and alias
   `ids`→`object_ids`** (finding 2). Kills the current top param-error class
   at the vocabulary level instead of the error-message level.
3. **search output=files: carry the standard empty-result hint** (finding 3).
   One-line payload change; keeps bad-scope calls diagnosable.

## Verdict

All four 817ea43 fixes are live in real model traffic and behave as designed:
`**/` globs went 0→6/7 productive, the search-param thrash class is gone
(5/5 in-run recovery), `path=` transfers across tools without erroring, and
get_context adoption jumped 2.5× now that it answers chokepoint questions in
one call. gokimi27-lci flipped from −0.06 under base (tier0) to +0.03 over
base. The next rung's blocker is provider capacity, not the harness; the next
tool win is the find_files path-scope normalization, which owns 100 % of this
run's remaining empty navigation calls.
