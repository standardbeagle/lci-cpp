# Tier 2 analysis — 2026-07-04

504 runs: chi, sinatra, guzzle, ripgrep × 7 models (haiku-4.5, gpt-5-mini,
gemini-3.5-flash, qwen3-coder, deepseek-v4-pro, kimi-k2.7-code, glm-5.2) ×
base/lci × 9 questions. 503 ok; 1 documented baseline failure (haiku-4.5
base timed out 3× at ~570s on rg-m1 — ripgrep ignore-precedence — without
LCI). Judge: sonnet-4.6 (one glm52 result judge-timed-out; facts scored).

## Headline

| minefield tier | variant | facts | judge | tok_total | wall_s |
|---|---|---|---|---|---|
| low (sinatra, guzzle, ripgrep) | base | 0.87 | 4.39 | 87k | 39.5 |
| low | lci | 0.88 | 4.35 | 164k | 45.8 |
| mid (chi) | base | 0.85 | 4.53 | 87k | 40.2 |
| mid | lci | 0.84 | 4.46 | 133k | 38.2 |

Per model: deepseek-v4-pro best value (facts 0.92 base at $0.0045/run);
haiku-4.5 gains most from LCI (+0.04 facts, only model whose baseline
DNF'd a question); gemini-3.5-flash is token-expensive (47-68k input/run,
$0.11-0.15) for mid accuracy; qwen3-coder is the outlier: weakest facts
(0.71/0.68) and worst LCI token blowup (2.8×, re-reads whole files after
searching).

## Interpretation

On low/mid-minefield repos, strong models saturate (0.90+ facts baseline) —
grep + read suffices, LCI adds ~1.7× tokens for ±0.01 accuracy. Combined
with post-slim tier 0 (LCI wins on haiku/gpt5mini on chi = the *highest*
minefield repo in tiers 0-2), the pattern is consistent with the central
hypothesis: **LCI pays off where architecture is treacherous, and for
models that can't brute-force**. The decisive test is tier 3: zls (38.6),
pocketbase (36.9), okhttp (34.3) with minefield-targeted question banks
(questions anchored on profiled risk-9 chokepoints, parameter-dependent
dispatch, cross-layer reach).

## Profiler caveats (filed)

- `code_insight` unified CYCLES and LAYER VIOLATIONS counts appear
  display-capped (5 and 8 across every repo) — the minefield index
  saturates on those two terms.
- zls module coupling/cohesion read 0.0 (module analyzer vs zig's
  src/-rooted layout).
- `analysis` param is silently ignored outside mode=detailed (fail-fast
  gap; mode itself does fail fast).
