# Tier 3 analysis — high-minefield repos — 2026-07-05

349 runs: zls (38.6), okhttp (34.3), pocketbase (36.9) × base/lci ×
minefield-anchored banks. Model fleet shifted mid-tier by provider events
(below): final usable grid = 5 zen models (big-pickle, deepseek-v4-flash,
nemotron-3-ultra, mimo-v2.5, north-mini-code) complete on both variants,
haiku-4.5 partial, gpt-4.1/sonnet-4.6 all-timeout (excluded), gpt-5-mini
base-only remnant. Judge: nemotron-3-ultra (see config note; facts metric is
judge-independent).

## Headline

Minefield tier × variant (all runs, failures count as facts=0):
base 0.72 → lci **0.77 (+0.05)** — the largest aggregate LCI lift of any
tier (tiers 0-2 low/mid minefield: ±0.01). Decomposed:

1. **Completion-rate insurance, not point-accuracy, is LCI's main effect on
   treacherous repos.** haiku-4.5 without LCI finished 8/18 runs (10×
   600s-timeouts); with LCI 9/11. Same pattern as tier 2's rg-m1 triple-DNF.
   Weak/mid models drown in high-minefield code without navigation; LCI
   keeps them swimming.
2. On completed runs, strong zen models move little: big-pickle 0.94→0.95,
   dsv4free 0.90→0.92, nemotron 0.93→0.95, mimo 0.95→0.94, north-mini
   0.91→0.91 — at ~1.6-2× tokens.
3. **Bank-design lesson:** minefield questions that *name* the chokepoint
   symbol (resolveType, CallServerInterceptor…) hand the baseline its grep
   key — 0.90+ base facts everywhere. Next bank iteration needs
   discovery-style questions ("which function routes all X", "where would a
   change to Y break Z") where the symbol must be *found*, not looked up.
   That is the actual minefield-identification task LCI exists for.

## Provider log (operational reality of multi-LLM evals)

- openrouter: credits exhausted mid-tier-3 → 100 provider_error runs purged;
  deepseek-v4-pro/glm-5.2/kimi-k2.7/gpt-5-mini(openrouter) dropped.
- copilot monthly: gpt-4.1 + sonnet-4.6 mass-timeout (600s) under load —
  every run dead; also killed sonnet-4.6 as judge after 700+ good judgments.
- kimi-for-coding: key invalid (needs interactive re-auth).
- z.ai: hard 429s.
- opencode Go (zen): free-tier models have per-model quotas
  (deepseek-v4-flash judged 100 grading calls then quota'd out);
  big-pickle answers trivial prompts in seconds but blows a 240s budget on
  real grading prompts; nemotron-3-ultra judged the backlog at ~10s/call.

## Cross-tier summary (the ladder so far)

| tier | repos (minefield) | LCI effect |
|---|---|---|
| 0 pre-slim | chi+sinatra | accuracy flat, 2.5× tokens — found 18.6k search payload |
| 0 post-slim | chi+sinatra | all LCI variants beat base facts; haiku hard +0.13 |
| 1-2 | low/mid repos, 7 models | flat (±0.01); strong models saturate via grep |
| 3 | high-minefield | +0.05 aggregate; DNF-rate insurance; banks too anchored |

Product changes shipped from this loop: search payload −74% (b1b3615),
empty-optional-param validation + enumerated tool descriptions (5f81d98),
CMake schema staleness fix, minefield profiler (6b9d300).
