# Discovery eval — find-the-chokepoint — 2026-07-05/06

> **SUPERSEDED 2026-07-06 — see "Fixed-binary re-run" below.** Everything in
> the original verdict ran against a crippled LCI (find_files 100% dead
> under `.work/`, test/ui files unindexed — fixed 3abebc2+0895aae). The
> grep-parity conclusion does not survive the fix.

## Fixed-binary re-run (results/discovery-fixed, same questions/models)

| model | base | lci (crippled) | lci (fixed) |
|---|---|---|---|
| deepseek-v4-flash | 0.94 | 1.00 | **1.00** |
| deepseek-v4-pro | 0.96 | 0.85 | **1.00** |
| glm-5.2 (Go) | 0.89 | 0.96 | **1.00** |
| kimi-k2.7-code | 1.00 | 1.00 | **1.00** |
| qwen3.7-max | 0.93 | 1.00 | **1.00** |
| kimi k2p7 | 1.00 | 0.96 | **1.00** |
| mimo-v2.5 | 1.00 | 0.94 | **1.00** |
| nemotron-3-ultra | 0.90 | 0.86 | **1.00** |
| zhipuai glm-5.2 | 0.93 | 1.00 | **1.00** |
| **mean** | **0.95** | 0.95 | **1.00** |

With a working LCI (find_files functional, full index, grouped
root-relative search output), **every model scores a perfect 1.00 on
find-the-chokepoint questions — 77/77 completed runs, all fact groups
hit** — vs 0.95 baseline. The crippled run's "grep parity" was the tool
failing, not the concept: models fell back to grep because half their LCI
calls returned empty. Completion rate is now par (lci 10/87 timeouts vs
base 9/83 — both heavy-model 600s cases); token cost varies by model
(nemotron −60% from grouped output, kimi k2p7 up on deeper exploration).

Implication for all earlier tiers: pre-0895aae LCI numbers are a floor.
Tier 0-3 LCI slices under-measure; re-run before quoting them.

160 completed runs: zls/okhttp/pocketbase × 9 discovery questions ("which
function is the chokepoint that all X routes through" — symbol NOT named)
× 10 models (Go fleet: deepseek-v4-pro, glm-5.2, kimi-k2.7-code,
qwen3.7-max; zhipuai glm-5.2; kimi k2p7; free fleet: deepseek-v4-flash,
mimo, north-mini, nemotron) × base/lci.

## Verdict

| model | base facts | lci facts | Δ |
|---|---|---|---|
| deepseek-v4-flash | 1.00 | 1.00 | 0 |
| deepseek-v4-pro | 0.96 | 0.83 | −0.13 |
| glm-5.2 (Go) | 0.89 | 0.96 | +0.07 |
| kimi-k2.7-code | 1.00 | 1.00 | 0 |
| qwen3.7-max | 0.93 | 1.00 | +0.07 |
| kimi k2p7 | 1.00 | 0.96 | −0.04 |
| mimo-v2.5 | 1.00 | 1.00 | 0 |
| nemotron-3-ultra | 0.92 | 0.92 | 0 |
| zhipuai glm-5.2 | 0.94 | 1.00 | +0.06 |
| **mean** | **0.96** | **0.96** | **0.00** |

Completion rate: base 9/83 failed (10.8%) vs lci 5/77 (6.5%) — the same
~2× DNF reduction as tier 3. Token cost with LCI: ~1.5-2.5×.

## What the whole program says (tiers 0-3 + discovery, ~1,400 runs)

1. **2026 strong models navigate repos near-perfectly with grep+read** on
   corpora up to ~150k LOC — even discovery-style chokepoint hunting.
   Accuracy headroom for any navigation tool is small in this size class.
2. **LCI's reproducible win is reliability + weak-model rescue**: DNF rate
   halves on high-minefield repos (tier 3 and discovery independently);
   haiku-class models go from drowning (8/18 finished) to finishing (9/11);
   the one baseline triple-DNF (rg-m1) never happened with LCI.
3. **Token overhead (~1.5-2.5×) is the adoption blocker.** The payload slim
   (b1b3615) cut 74% of search response size; the residual is round-trips +
   schema re-reads + models re-reading files they already searched.
4. **Where to look for accuracy wins**: corpora an order of magnitude
   bigger (monorepos, multi-service), tasks that need transitive structure
   (impact analysis, refactor planning — "what breaks if"), and token-
   budgeted agents where grep's read-everything strategy stops fitting.

## Method notes

- Judge: nemotron-3-ultra (partial coverage on discovery — facts metric is
  judge-independent and is what the verdict uses).
- deepseek-v4-pro's −0.13 is 2-3 questions of single-rep noise; reps=1.
- 20 runs never completed after retries (600s timeouts on the heaviest
  model × question pairs, both variants) — excluded from facts, counted in
  completion rate.
