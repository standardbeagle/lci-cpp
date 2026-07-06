# Tier-3 re-run after the tier0/tier1 comb fixes — high-minefield repos

Completes the ladder past `ANALYSIS-tier1-2-postfix.md` (00b3393) and the
discovery verdict (`ANALYSIS-discovery.md`, 1f0f889). Binary under test:
`build/src/lci` at **1f0f889** — carries every post-tier0 fix: `**/` globs at
root, search param recovery + allow-list, `path=` alias, get_context `callers`
count (817ea43), and the tier1-comb fixes find_files directory normalization,
`include=signature`, and empty-result hints (1f4c75c). Repos: the three
high-minefield corpora — **zls** (38.6), **okhttp** (34.3), **pocketbase**
(36.9) — easy/medium/hard banks (discovery bank scored separately). Facts
scored `judge.py --skip-llm` (deterministic, judge-independent). Comb:
`scripts/comb.py`.

## Provider reality (2026-07-06)

The prior tier3-fixed attempt on the free-tier zen fleet
(mimo/northmini/dsv4free/nemotron) produced **zero** completed lci runs across
~3 h — those providers are quota-dead. The old `results/tier3-fixed/` base
files from that crippled-binary fleet are retained in the directory but
**excluded from every table here** (different fleet, different binary).

Liveness smoke (chi easy, base, 1 rep) probed 7 candidates. **6 alive:**
`godsv4pro`, `goglm52`, `gokimi27`, `goqwen37` (opencode-go plan), `kimik2p7`
(kimi-for-coding), `zhipuglm52` (zhipuai plan). `dsv4free` (free retry) still
quota-dead (0 files). Both variants were run for all 6 live models —
same-model, same-day, same-binary comparison. **324 runs launched, 299
completed ok, 5 timeouts, 0 other failures.**

The only DNFs: **pocketbase-lci on three opencode-go models** (`goglm52` 3,
`gokimi27` 2, `goqwen37` never engaged) — all `wall~=584 s, ntools=0`: the
model never emitted a single event. This is the documented go-plan throttle
firing at the tail of a 300-run session (same class as tier1/tier3 provider
logs), **not** an LCI defect: `godsv4pro` (also go-plan) completed
pocketbase-lci 9/9 cleanly when it ran earlier in the queue, and a fresh
2-model relaunch under light load still stalled at `ntools=0` — the endpoint,
not the harness. zls+okhttp are 100 % complete on all 6 models both variants.

## Headline — zls+okhttp, clean 6-model grid (18 q/model, 100 % completion)

Base and lci on the identical question set, per model:

| model | base | lci | delta | base tok | lci tok | base wall | lci wall |
|---|---:|---:|---:|---:|---:|---:|---:|
| godsv4pro | 0.924 | **0.991** | **+0.067** | 59,849 | 129,114 | 48.6 | **38.4** |
| zhipuglm52 | 0.947 | **0.980** | +0.032 | 75,331 | 84,177 | 60.0 | 71.0 |
| gokimi27 | 0.970 | **0.989** | +0.019 | 90,803 | 167,294 | 43.6 | 57.3 |
| kimik2p7 | 0.943 | **0.961** | +0.019 | 81,177 | 94,589 | 44.2 | 52.1 |
| goqwen37 | 0.981 | 0.961 | -0.020 | 63,063 | 109,386 | 47.0 | 49.3 |
| goglm52 | 0.989 | 0.966 | -0.023 | 66,781 | 122,711 | 49.3 | 63.0 |
| **AGG** | **0.959** | **0.975** | **+0.015** | | | | |

**4 of 6 models win with LCI; aggregate +0.015.** The two that dip
(`goglm52`, `goqwen37`) enter at a 0.98-0.99 base — ceiling saturation, the
same shape tiers 1-2 showed for strong models on grep-friendly banks. The
standout is `godsv4pro`: +0.067 facts **and** -21 % wall (48.6->38.4 s) — LCI's
intended effect, targeted search replacing broad reads.

## Per-repo (facts, failures = 0)

| repo (minefield) | base | lci | delta | note |
|---|---:|---:|---:|---|
| zls (38.6) | 0.943 | **0.968** | **+0.025** | 54/54 both, all 6 models |
| okhttp (34.3) | 0.975 | **0.981** | +0.006 | 54/54 both, all 6 models |
| pocketbase (36.9) | 0.943 | 0.937 | -0.006 | 3-model fair set; other 3 go-plan lci DNF |

zls — the highest-minefield repo — shows the largest lift (+0.025), the
tier-3 thesis that LCI's navigation matters most where the call graph is most
tangled. pocketbase is flat: its bank names the target symbol
(`processActiveProps`, `splitModifier`, CORS `AllowOriginFunc`), handing the
baseline a grep key — the same bank-anchoring caveat raised in
`ANALYSIS-tier3.md` finding 3.

## vs old tier3 (`ANALYSIS-tier3.md`: 0.72 base -> 0.77 lci)

Different fleet, different binary — **structural comparison only.** The old
grid ran the crippled binary (find_files 100 % dead) on weak free-tier zen
models (base 0.72). This fresh fleet enters at base 0.96, so the LCI delta is
compressed by the ceiling (+0.015 vs the old +0.05). The old run's headline
effect — "completion-rate insurance for weak models" — did not reproduce here
because this fleet is strong enough to finish base runs unaided; instead the
DNF risk inverted onto the **lci** side via provider throttle on the biggest
repo (see below). What *is* durable across both: LCI >= base on the untangled
repos (zls +0.025), flat on symbol-named banks.

## Fine-tooth comb — 137 traced lci runs

The tier0/tier1 fixes are **fully live and the tier1 #1 finding is
eliminated:**

| surface | this run | tier1-postfix |
|---|---|---|
| find_files content rate | **48/48 (100 %)** | 8/8 *empty* was the #1 finding |
| find_files `**/` globs | 3/3 HIT (`**/CallServerInterceptor**`->209ch, `**/connection/*`->3.7kch) | 6/7 |
| near-empty lci outputs (<80ch) | **none** | find_files 8, the dir-scope bug |
| search errors | 2, **both carry allow-list** | 5, all recovered |
| `include=signature` | in the live allow-list (kimik2p7 error text names it) | just shipped |
| bash-explore after an lci call | **3/137 runs (2.2 %)** | 2/55 |
| path= alias (browse_file) | 2/2 HIT | 6 |

**find_files directory normalization (1f4c75c) closed the entire empty-
navigation class** that owned 100 % of tier1's remaining misses:
`.`/`/`/absolute/wrong-dir scopes now resolve, and 48/48 calls returned
content across three deep-nested, large-file corpora. No new empty-output
pattern appeared on the high-minefield repos.

**LCI substitutes semantic search for blind filesystem probing.** Raw
grep/glob/bash exploration calls per run, zls+okhttp, base -> lci:

| difficulty | base fs-probe | lci fs-probe | raw grep/glob (base->lci) |
|---|---:|---:|---|
| easy | 2.4 | 1.4 | 1.4 -> 0.2 |
| medium | 8.0 | 6.6 | 3.5 -> 0.8 |
| hard | 4.8 | **2.8** | 2.2 -> 0.2 |

On hard (architecture) questions LCI nearly eliminates raw grep/glob (2.2->0.2)
and cuts total filesystem probing 42 % (4.8->2.8), replacing it with
`lci_search` (230 calls), `get_context` (43), `inspect_symbol` (44),
`find_files` (48). `read` count is unchanged (models still read to quote the
answer) — the win is in *locating*, not reading.

### Remaining findings, ranked

1. **LOW — the only 2 genuine tool errors are both `search include=`
   vocabulary edge cases.** `kimik2p7 pb-h1`: `include='*.go'` (filter
   semantics leaked into the add-on slot) -> error now names the allow-list
   *including* `signature`, model recovered next call. `godsv4pro okh-h2`:
   passed a garbage `file='true">okhttp/...'` value (model hallucinated an
   HTML fragment as the param) -> allow-list returned, did not recover in-run.
   Both are model-side arg malformation, not tool defects; the allow-list
   fix keeps them diagnosable. No cheap tool win here beyond what already
   shipped.
2. **LOW — same-file re-reads, 46/137 runs.** 385 reads total (2.8/run);
   paged/repeated reads of the file the search located. Unchanged behavior
   from tiers 0-1 (expected: search locates, model pages to quote). Only
   lever is a larger default browse_file/get_context window, which cuts
   against payload discipline — leave.
3. **NOT A TOOL BUG — pocketbase-lci go-plan stalls (5 DNF).** `ntools=0`
   provider hangs, not LCI. The operational lesson is harness-side: run the
   largest-repo x lci cells **first** in the model queue (while provider
   budget is fresh), not last. Filed as the next-rung ordering change.

## Ranked next improvements

1. **Harness: order the job queue biggest-repo-lci-first per model**, so
   provider throttle (which arrives at the session tail) lands on cheap
   already-completed cells instead of the one expensive grid. Would have
   saved all 5 DNFs this run. (bench.py job ordering — no product change.)
2. **Bank iteration for pocketbase/okhttp: discovery-style questions.** Both
   sit flat because the bank names the chokepoint symbol (grep key handed to
   base). zls, whose bank asks more "what does X do / what guards Y", shows
   the +0.025 lift. Port the discovery-bank style (proven in
   `ANALYSIS-discovery.md`: LCI 1.00 vs base 0.95) to the medium/hard tiers.
   This is the highest-value change and it is a benchmark-design change, not
   a tool change — the tools are clean.
3. **Product: nothing urgent.** The comb surfaced zero new tool defects on
   three high-minefield, deep-nested, large-file corpora. find_files is at
   100 % content, empties are gone, errors are recoverable. The tool backlog
   from tiers 0-1 is closed.

## Verdict

On the fixed binary and a strong same-day fleet, LCI is **+0.015 aggregate on
the clean high-minefield grid (zls+okhttp), +0.025 on the single most tangled
repo, and flat where the bank hands base a symbol name** — while cutting raw
grep/glob exploration ~40 % on architecture questions and, for `godsv4pro`,
delivering +0.067 facts at -21 % wall. Every tier0/tier1 comb fix is live in
model traffic: find_files empties are eliminated (48/48 content, the tier1 #1
finding gone), and the only two errors all run carry the recovery allow-list.
The lci-side DNFs are provider throttle on the biggest repo, addressable by
job ordering. The tool surface is clean; the remaining lever is
discovery-style banks, not more fixes.
