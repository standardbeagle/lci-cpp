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

## Repo tiers — the minefield axis

Corpora are the `real_projects/` submodules. The difficulty axis is not LOC:
repos are profiled by **architectural minefield** metrics — call-graph
chokepoint concentration (top symbol reach share, betweenness brokers), max
cyclomatic complexity, call cycles, layer violations, module coupling vs
cohesion, Louvain modularity, and side-effect impurity. LCI computes these
itself (`scripts/profile_repos.py` → `repo-profiles.json`); the composite
0-100 `minefield` index and tier live in `config.kdl`. LCI's purpose is
identifying and navigating exactly these minefields, so the central
hypothesis is: **LCI's advantage over grep grows with the minefield index**
(zls 38.6, pocketbase 36.9, okhttp 34.3 at the top; guzzle 15.6 at the
bottom). The report slices results by minefield tier. Tier ladder grows from
2 small repos + 2 cheap models (tier 0, harness/LCI tuning) to the
high-minefield set + frontier models (tier 3).

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

Annotation overlay comparison:

```bash
scripts/bench.py run --repos zls,okhttp,pocketbase --models zhipuglm52 \
    --variants lci,lci-ann --difficulties annotation \
    --out results/annotation-compare
```

`lci-ann` copies repo-specific manifests from `annotations/<repo>/` into the
workspace's `.lci/annotations/` directory before MCP indexing. Plain `lci`
uses the same source tree without those manifests.

Validate annotation manifests against the live MCP tool:

```bash
scripts/validate_annotations.py
```

The validator queries every label and category in `annotations/<repo>/` through
`semantic_annotations` and checks the returned symbol, path, category, labels,
and tags.

## Stage banks (unbiased, approval-gated)

Beyond the tier ladder above, this directory holds a three-stage,
preregistration-style measurement suite built with the
`design-unbiased-benchmarks` / `audit-benchmark-evidence` methodology. Each stage
pins a `run-config.json`, isolates one variable (the agent's navigation surface),
scores against an LCI-independent oracle, and is **informational until approved
and powered** — no cell is silently re-run and none is scored when it never ran.

| Stage | Where | What it measures | Status |
|---|---|---|---|
| **1 exploration** | `exploration/` | cited-evidence precision/recall/F1 (find the right code) | pinned, smoke-instrumented, **0 executed** (approval not granted) — `ANALYSIS-exploration-stage1.md` |
| **2 claim validation** | `exploration/scoring/claim_validation.py`, `scripts/score_claim_validation.py` | does LCI's *answer* hold vs an independent oracle | instrument present, **PENDING** baseline (no committed ledger/report) |
| **3 edits** | `edits/` | all-three-gates pass (behaviour ∧ convention ∧ blast) on a *produced patch* | pinned, two-arm smoke **PASS**, **0 executed** (approval not granted) — `edits/REPORT.md` |

Stage-3 pinned config + artifacts + the two-arm pre-flight smoke:

```bash
# structural bank validation (no provider)
/usr/bin/python3 scripts/validate_edit_tasks.py
# one-task/two-arm pipeline smoke (must print SMOKE PASS; deterministic, no provider/toolchain)
/usr/bin/python3 scripts/edit_pipeline_smoke.py --out-dir edits/results/smoke
# re-derive baseline scores.json + aggregate.json from the committed ledger (deterministic)
/usr/bin/python3 scripts/edit_baseline_ledger.py
# the paid grid itself (guarded; refuses to launch without approval + creds + --live)
python3 scripts/exploration_runner.py edit-smoke   # SKIPs unless opted in + capable
python3 scripts/exploration_runner.py edit-run --model <approved> --timeout <cap> \
    --corpus-root .work/exploration \
    --records edits/results/baseline/run-records.jsonl --work-root .work/edits-run
```

How the three stages jointly gate an LCI optimisation PR (non-gameable policy,
no hard threshold before variance supports it) is defined in
`../../docs/benchmarks/stage-gated-lci-pr-policy.md`. Forged corpora, raw
transcripts and credentials are never committed (`.work/` is gitignored).

## Caveats

- The installed `~/.local/bin/lci` (0.7.0 release) predates the MCP newline
  framing fix and does not answer MCP requests — `config.kdl` points at the
  build-tree binary.
- Copilot models report cost 0; compare tokens, not dollars, across providers.
- The judge model (sonnet-4.6) also appears as a candidate in tiers ≥2;
  treat its own judged scores with self-preference caution.
