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
  omit empty enrichment fields (this commit). Expected: ~70% payload cut.
- [ ] Relative (root-relative) `file` paths in search results — needs project
  root plumbed into handlers; saves another ~25% of payload.
- [ ] Re-run tier0 lci/lci-slim slices against the slimmed binary; compare.
- [ ] Tier 1-2: add gemini-2.5-flash + qwen3-coder, medium repos (ripgrep,
  guzzle) — test whether LCI's win grows with repo size.
- [ ] Consider a `compact` default output mode for `get_context`-adjacent
  tools before tier 3 (large repos).
