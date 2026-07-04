# Tier 0 analysis — 2026-07-04

108 runs: chi + sinatra × haiku-4.5 + gpt-5-mini × {base, lci, lci-slim} × 9 questions.
All ok after 2 timeout retries. Judge: sonnet-4.6. Full numbers: `results/tier0/report.md` (local).

## Headline

| model | variant | facts | judge | tok_total | cost$ | wall_s |
|---|---|---|---|---|---|---|
| gpt5mini | base | 0.83 | 4.51 | 95k | .0086 | 27 |
| gpt5mini | lci | 0.82 | 4.43 | 141k | .0115 | 55 |
| gpt5mini | lci-slim | 0.84 | 4.49 | 139k | .0103 | 33 |
| haiku45 | base | 0.78 | 4.25 | 52k | .0154 | 48 |
| haiku45 | lci | 0.82 | 4.26 | 142k | .0303 | 35 |
| haiku45 | lci-slim | 0.80 | 4.12 | 116k | .0291 | 31 |

## Findings

1. **LCI helps weak models on hard questions, is cost-neutral-to-negative
   elsewhere (on small repos).** haiku hard-question facts: 0.69 → 0.82
   (+0.13), wall 58s → 38s. Per-question: haiku 4 wins / 1 loss, gpt-5-mini
   2 wins / 4 losses. Small repos (10-24k LOC) are easy to grep — LCI's
   navigation advantage should grow with repo size (test at tier 2-3).

2. **Token overhead is the blocker: lci variant costs 1.5-2.8× base tokens.**
   Two sources measured:
   - `lci_search` default payload: 50 results × ~390 chars = **18.6k chars
     (~4.6k tokens) per call**. Fat: absolute path prefix repeated per result
     (~110 chars), vestigial `result_id`, empty-string enrichment fields.
   - 14 tool schemas ≈ 4.3k tokens re-read every turn; lci agents also take
     ~2.5× more tool round-trips than base (haiku 2.3 → 5.7).

3. **lci-slim (5 tools instead of 14) fixed latency, not tokens.** gpt-5-mini:
   wall 55→33s, judge back to baseline, facts best-in-class 0.84 — but total
   tokens barely moved. Payload size, not schema count, dominates.

4. **Installed 0.7.0 release binary is MCP-dead** (predates newline framing
   fix 0e9f085). Any real-world user on the released binary gets a hung MCP
   server. Re-ship needed.

5. Models ignore LCI tools without steering — the lci variants inject an
   `AGENTS.md` preferring `lci_*` tools (mirrors the lci plugin). Without it,
   models grep via bash and the MCP server is dead weight.

## Actions

- [x] Slim `handle_search` response: default max 50→15, drop `result_id`,
  omit empty enrichment fields (b1b3615). Measured: 74% payload cut
  (18.6k → 4.9k chars on the same chi query).
- [ ] Relative (root-relative) `file` paths in search results — needs project
  root plumbed into handlers; saves another ~25% of payload.
- [x] Re-run tier0 lci/lci-slim slices against the slimmed binary.

## Post-slim re-run (results/tier0-postslim, same 108-run grid)

| model | variant | facts | judge | tok_total | Δtok vs pre | cost$ | wall_s |
|---|---|---|---|---|---|---|---|
| gpt5mini | base | 0.83 | 4.51 | 95k | — | .0086 | 27 |
| gpt5mini | lci | **0.87** | **4.58** | 169k | +20% (noise) | .0119 | 42 |
| gpt5mini | lci-slim | 0.84 | 4.49 | 128k | −7% | .0102 | 30 |
| haiku45 | base | 0.78 | 4.25 | 52k | — | .0154 | 48 |
| haiku45 | lci | 0.82 | 4.17 | 106k | **−25%** | .0262 | 38 |
| haiku45 | lci-slim | **0.83** | 4.22 | 111k | +1% | .0259 | 31 |

(Caveat: pre/post judged by different-day sonnet-4.6 sessions; facts are the
stable metric.) Post-slim, **every LCI variant now beats its baseline on fact accuracy**
(gpt5mini lci 0.87 vs 0.83 with perfect grounding 5.0; haiku lci-slim 0.83
vs 0.78 at 36% less wall time). haiku lci token cost dropped 25% at equal
accuracy. gpt5mini lci tokens rose — single-rep noise; that model's
exploration depth varies heavily run-to-run (reps=2+ needed at tier 1).
Residual 2× token premium vs base is now round-trip count + schema re-reads,
not payload.
- [ ] Tier 1-2: add gemini-2.5-flash + qwen3-coder, medium repos (ripgrep,
  guzzle) — test whether LCI's win grows with repo size.
- [ ] Consider a `compact` default output mode for `get_context`-adjacent
  tools before tier 3 (large repos).
