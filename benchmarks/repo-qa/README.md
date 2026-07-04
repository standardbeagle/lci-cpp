# Repo-QA Benchmark

Measures whether LCI improves an agent's ability to answer questions about a
codebase. Compares **baseline opencode** (bash/grep/read only) against
**opencode + LCI MCP** across models, repos, and question difficulties.

## Rubric

Each run records:

| Metric | Source | Notes |
|---|---|---|
| tokens (in/out/cache/total) | opencode `step_finish` events | primary cost signal |
| cost ($) | opencode event stream | $0 for copilot subscription models |
| wall time (s) | runner | includes MCP startup + indexing for lci variant |
| tool calls (total, `lci_*`) | `tool_use` events | verifies the lci variant actually used LCI |
| fact accuracy (0-1) | deterministic regex fact-groups per question | `must_mention` in question banks |
| LLM judge (1-5) | judge model, four dimensions | correctness, completeness, grounding, conciseness |

## Question difficulty

- **easy** — locate/describe a single symbol or file
- **medium** — relationships: callers, mechanism spanning 2-3 files
- **hard** — architecture or subtle cross-cutting behavior (request tracing, edge-case semantics)

Question banks live in `questions/<repo>.json`; every gold answer was verified
against the corpus code, and `must_mention` fact groups are regex alternatives
(any match credits the group).

## Repo tiers

Corpora are the `real_projects/` submodules, classified in `config.kdl` by
size (LOC) and complexity (module structure). Tier ladder grows from
2 small repos + 2 cheap models (tier 0, harness/LCI tuning) to 7 repos +
6 models + 3 reps (tier 3).

## Variants

- `base` — workspace copy of the corpus, `opencode.json` with all MCP disabled
- `lci` — same plus the LCI MCP server (build-tree binary) and an `AGENTS.md`
  steering the agent to prefer `lci_*` tools (mirrors the lci plugin setup)

Prompts are identical across variants; only tooling differs.

## Running

```bash
# 1. execute runs (idempotent; re-run to resume)
scripts/bench.py run --tier 0 --out results/tier0

# 2. score (fact regexes + LLM judge; --skip-llm for facts only)
scripts/judge.py --dir results/tier0

# 3. aggregate
scripts/report.py --dir results/tier0
```

Ad-hoc slices:

```bash
scripts/bench.py run --repos chi --models haiku45 --variants lci \
    --difficulties easy --reps 1 --out results/smoke
```

## Caveats

- The installed `~/.local/bin/lci` (0.7.0 release) predates the MCP newline
  framing fix and does not answer MCP requests — `config.kdl` points at the
  build-tree binary.
- Copilot models report cost 0; compare tokens, not dollars, across providers.
- The judge model (sonnet-4.6) also appears as a candidate in tiers ≥2;
  treat its own judged scores with self-preference caution.
