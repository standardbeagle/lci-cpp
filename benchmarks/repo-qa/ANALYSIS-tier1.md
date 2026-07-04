# Tier 1 + trace + micro analysis — 2026-07-04

## Tier 1 (gemini-2.5-flash, qwen3-coder; results/tier1, 108 runs)

| model | variant | facts | judge | tok_total | cost$ | wall_s |
|---|---|---|---|---|---|---|
| gemini25f | base | 0.61 | 3.60 | 49k | .0086 | 8.9 |
| gemini25f | lci | 0.61 | 3.39 | 70k | .0116 | 14.7 |
| gemini25f | lci-slim | 0.55 | 3.40 | 65k | .0114 | 10.8 |
| qwen3coder | base | 0.65 | 3.87 | 78k | .0035 | 20.0 |
| qwen3coder | lci | 0.61 | 3.79 | 191k | .0042 | 27.2 |
| qwen3coder | lci-slim | 0.59 | 3.71 | 155k | .0037 | 27.8 |

Unlike claude/gpt-5-mini (post-slim tier 0), these two models got **no lift**
from LCI. Traces explain most of it (below): validation-error thrash and
double-paying (lci_search then re-`read`ing the same files — qwen made 58
reads on top of 43 searches).

## Trace findings (results/traces: 36 runs, 4 models x lci, chi)

- **Adoption is not the problem**: 33/36 runs used lci tools; zero bash
  fallback after an lci call in 3 of 4 models.
- **gpt-5-mini: 20/22 lci_search calls failed validation** — it fills every
  optional param, sending `patterns: ""`, and the schema's `minLength: 1`
  rejected the whole call. FIXED (5f81d98): empty optional strings validate.
- **Models guess `include` values** (`signature`, `ids`, `all`, `path,lines`,
  `*.go`) because the tools/list description said just "Include options".
  FIXED (5f81d98): descriptions now enumerate valid values and redirect
  file-filtering to `filter`.
- Refinement re-calls (same tool, new args) run ~1.1-1.7 per question — each
  is a full extra turn. Post-fix rerun should cut the error-driven share.
- 6 zero-signal `lci_find_files` calls (<80 chars out) — models pass
  fuzzy/partial names it can't match; response gives no "did you mean".

## Micro benchmark (results/micro: 6 tool-oriented prompts x 4 models, lci)

Single-tool prompts isolate output interpretation from navigation strategy:

| model | facts | tool_match | judge |
|---|---|---|---|
| gpt5mini | 0.83 | 1.00 | 4.21 |
| haiku45 | 0.83 | 1.00 | 3.75 |
| gemini25f | 0.67 | 1.00 | 3.79 |
| qwen3coder | 0.67 | 1.00 | 3.83 |

Every model calls the requested tool (tool_match 1.0) — the capability gap is
in **reading the output** (counting results, extracting file:line, reporting
error payloads faithfully), and it tracks the tier-1 task-level gap.

## Deterministic tool eval (tooleval.py, tool-cases/chi.json)

20/20 pass, all calls sub-ms. Covers shape assertions, max-cap behavior,
unknown-param rejection, empty-enrichment omission, chained
search→object_id→get_context, and clean error paths.

## Open actions

- [ ] Re-run gpt5mini/qwen lci traces post-fix; expect validation errors → ~0
  (running: results/traces-postfix).
- [ ] Root-relative paths in all tool outputs (search, find_files,
  list_symbols, get_context, browse_file all emit absolute paths — ~60-110
  chars of repeated prefix per result).
- [ ] `find_files` "no match" response could suggest close names.
- [ ] Latest-model matrix added (Sonnet 5, GPT-5.5, Fable 5, GLM-5.2,
  Kimi K2.7-code, Qwen 3.7 Max, DeepSeek V4 Pro, Gemini 3.5 Flash — all
  probed working). Judge upgraded to sonnet-5.
- [ ] Tier 2 (medium repos: guzzle, ripgrep) with the refreshed matrix.
