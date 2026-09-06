# E2.4 — Tool-description grid: selection, confusion, and a registry that mostly failed

Task `01KXSHA3QKT5RDT4F08RA08H1N`. Predictions pre-registered in
`toolcalling/predictions.json`, commit `db9f200` at 2026-09-06T20:43:08Z; the
first grid ledger opened at 20:43:30Z, and the analyzer refuses to read any
ledger that predates the registry commit.

Machine-readable results: `toolcalling/analysis/selection-analysis.json`.

## Boundary

This measures **which tool a model calls first** for a stated goal, against a
**mock MCP** whose handlers return deterministic stubs and record the call. It
does **not** show that the selected tool then produces a correct answer (epic 1),
and it does **not** show that an LCI-equipped agent beats grep end to end
(discovery epic). Nothing here is a claim about LCI's output.

## Headline

**No rewrite beat the shipped descriptions. Every rewrite was directionally
worse, and the mechanism is visible: the rewrites lost ground to the agent's own
built-in tools, not to a neighbouring LCI tool.**

Correct-first-call rate on the confusable tier, mean of 3 reps (per-rep n in
brackets — denominators differ because provider failures are excluded):

| variant | weak | strong |
|---|---|---|
| **A — live verbatim (baseline)** | **0.791** [13,12,13] | **0.711** [15,15,15] |
| B — task-framed | 0.615 [13,13,13] | 0.618 [14,13,12] |
| C — example-augmented | 0.575 [13,13,14] | 0.592 [15,15,14] |
| D — disambiguated | 0.695 [13,12,14] | 0.659 [15,14,12] |

Every gap is negative and each sits inside or near the baseline's own run-to-run
spread (weak 0.308, strong 0.067), so the honest verdict per the pre-registered
decision rule is: **B, C and D show no lift; C is a measured null; the negative
direction is consistent across all three but only the strong tier's B and C
deltas (-0.093, -0.119) exceed that tier's spread.**

### The mechanism: rewrites cost ground to `native:*`, not to neighbours

`native:*` first calls — the agent opening with its built-in glob/read/bash
instead of any offered tool — as a share of the graded denominator:

| variant | weak | strong |
|---|---|---|
| A | 1/41 (2%) | 10/48 (21%) |
| B | 4/42 (10%) | 14/42 (33%) |
| C | 6/43 (14%) | 16/47 (34%) |
| D | 5/42 (12%) | 14/44 (32%) |

Neighbour pressure — a tool being wrongly reached for on the tasks that name it
confusable — did **not** rise to match. Under D on the strong tier it is zero.
The lost selections went to the agent's own toolbox. A rewrite that reads as a
tidier catalogue entry apparently competes worse against `glob` and `read` than
the shipped text does, and the shipped text's concrete, jargon-heavy phrasing
("resolved call-graph query, not a text scan") is doing more work than its
tidiness suggests.

## Per-task detail (correct/graded, pooled over both models and 3 reps)

| task | correct tool | A | B | C | D | dominant wrong column |
|---|---|---|---|---|---|---|
| is-the-corpus-ready | index_stats | 6/6 | 6/6 | 6/6 | 6/6 | — |
| index-internals-breakdown | debug_info | 6/6 | 6/6 | 6/6 | 6/6 | — |
| locate-by-path-fragment | find_files | 6/6 | 6/6 | 6/6 | 6/6 | — |
| pre-commit-change-quality | git_analysis | 6/6 | 6/6 | 6/6 | 6/6 | — |
| what-can-this-server-do | info | 6/6 | 6/6 | 6/6 | 6/6 | — |
| repo-shape-first-look | code_insight | 6/6 | 6/6 | 6/6 | 5/5 | — |
| **control-corpus-readiness** | index_stats | 6/6 | 6/6 | 6/6 | 5/6 | debug_info (D, 1 run) |
| collect-author-markers | semantic_annotations | 6/6 | 5/5 | 0/6 | 5/5 | search (C) |
| package-a-handoff-manifest | context | 6/6 | 3/5 | 5/6 | 4/5 | info (B), native (C,D) |
| where-does-this-text-appear | search | 3/4 | 0/1 | 0/2 | 3/3 | native:glob/grep |
| what-does-this-routine-touch | side_effects | 4/5 | 1/6 | 1/5 | 2/6 | native:read/glob/bash |
| confirmed-call-sites-for-one-routine | callers | 3/6 | 1/6 | 3/6 | 0/5 | list_symbols, native |
| hydrate-identifiers-from-a-prior-lookup | get_context | 2/4 | 2/3 | 3/6 | 3/5 | search, context, find_files |
| full-metadata-from-a-bare-name | inspect_symbol | 2/6 | 0/5 | 0/6 | 1/5 | **list_symbols** |
| browse-one-file-walkthrough | browse_file | 0/4 | 0/5 | 1/5 | 1/5 | **native:read/glob** |
| machine-readable-declaration-list | list_symbols | 0/6 | 0/6 | 0/6 | 0/6 | **native:glob** |

Six tasks are at ceiling under every variant: their tools are not a wording
problem and no rewrite can help them. Three tasks are at or near the floor under
every variant — `list_symbols` (0/24), `browse_file` (2/19), `inspect_symbol`
(3/22) — and those are where the value is, but the thief is `native:glob`/
`native:read` and `list_symbols`, not the neighbour the descriptions argue with.

### The control reports its null

`control-corpus-readiness` is single-tool-obvious by construction. It stayed at
1.00 under A, B and C on both models, and the analyzer classifies D's single
`debug_info` run as `no_effect` against the baseline's spread. The instrument
does not respond to wording where there is no ambiguity, which is what makes the
negative deltas above readable at all.

### The one "lift" the analyzer found, and why it is not a recommendation

`browse-one-file-walkthrough` on the strong model: D scored 1/3 reps where A
scored 0/3. The baseline is pinned at the floor, so its spread is exactly 0.0
and any non-zero treatment clears it. That is a degenerate case of the spread
rule, not evidence — one correct call in one rep. It is reported here rather
than quietly dropped, and it is **not** in the recommendations. A floor-pinned
baseline needs a minimum-n gate the current rule does not have.

## Where the predictions failed

13 misses, 9 hits. The registry's central bet was wrong.

**Miss 1 (the big one) — D was predicted best on 8 tasks; it was best on none.**
Predicted: an explicit `Unlike <neighbor>, ...` contrast resolves the pairs the
bank was built around. Measured: D is 0.10-0.05 *below* A on both tiers, and
every one of those 8 per-task predictions resolves to `a (no_effect)`.
Hypothesis: the contrast sentence lengthens the description and spends its most
salient position naming a *different* tool. A model skimming a 16-tool list sees
the neighbour's name inside the entry and the entry loses distinctiveness.
`collect-author-markers` under C (0/6, all stolen by `search`) is the same shape
from the other direction: text that mentions another tool's job invites it.

**Miss 2 — B was predicted best on 3 tasks; it was best on none**, and is the
weakest arm on the strong tier bar C. Hypothesis: "Use when you need ..." framing
converts a capability claim into a hedge. The shipped text asserts what the tool
*is*; the task-framed text describes a situation, which competes badly against a
built-in tool that needs no situation.

**Miss 3 — g2, weak-tier-gains-most: falsified, and inverted.** Predicted the
weak tier would gain most from the best rewrite, mirroring the output epic.
Measured: both tiers *lose*, and the weak tier loses more (-0.096 vs -0.052).
Hypothesis: the weak model is more sensitive to description wording in general,
so it is also more damaged by a rewrite that dilutes the shipped text. Sensitivity
is not the same as benefit; the output epic's result does not transfer.

**Miss 4 — g1 held, but vacuously.** The E2.3 smoke's `search`-for-`callers`
mis-selection did not reproduce under ANY variant, including baseline A: the
`search` column on the callers task is empty everywhere. D "fixed" a confusion
that was not present in 24 graded runs. The callers task still fails half the
time — to `list_symbols` and to native tools, not to `search`. Treat the smoke
finding as a single-run artifact, not a description defect.

**Predictions that held:** g3 (control null), g4 (native-first is
wording-insensitive — held in the strong sense: it did not fall, it *rose*),
g5 (C is a null — examples teach argument shape, not selection), g6
(hallucinated first calls, 0 across all 8 arms), and the four per-task nulls
(`is-the-corpus-ready`, `pre-commit-change-quality`, `what-can-this-server-do`,
the control).

## Recommended description changes, ranked

Ranking is (selection gain x call frequency). **Production call frequency is not
measured by this bench** — the proxy used is the tool's neighbour degree in the
bank plus its share of graded runs, and that substitution is a weakness of the
ranking, not a hidden assumption.

| # | change | expected selection gain | net-positive? |
|---|---|---|---|
| 1 | **Ship nothing. Keep variant A verbatim.** | +0.10 weak / +0.05 strong versus the best rewrite | **Yes** — it is the measured ceiling of the four arms |
| 2 | Do not adopt D's `Unlike <neighbor>` sentence anywhere | avoids -0.096 weak / -0.052 strong | Yes |
| 3 | Do not adopt B's `Use when you need` framing | avoids -0.176 weak / -0.093 strong | Yes |
| 4 | Do not adopt C's example invocations for selection purposes | measured null, and the largest native-first rise | Yes (they may still help argument construction; untested here) |
| 5 | `list_symbols`, `browse_file`, `inspect_symbol`: the real target, but **not** via a wording A/B | 0/24, 2/19, 3/22 under all four arms | Not net-positive as a wording change — needs a different intervention |

Item 5 is the finding worth acting on. Three tools are never selected, and the
competitor is the agent's built-in `glob`/`read`, not another LCI tool. No
wording arm in this grid moved them. The follow-up that would test the actual
hypothesis is a description that argues against the *built-in* tool rather than
against an LCI neighbour ("returns declarations with signatures and line spans,
which `glob` and `read` cannot give you without opening the file"), which is a
fifth variant this grid did not contain.

## What ran

- 16 tasks x 4 variants x 2 models = **128 cells per rep, 3 reps, 384 cells**.
  Reps are separate ledgers with `started_at` sidecars; no rep was reduced.
- Models, both verified present in `opencode models` (1.18.26) and both free:
  `opencode-go/deepseek-v4-flash` (weak), `opencode-go/glm-5.2` (strong).
  **Spend: $0.** No paid provider was used.
- Non-selection outcomes after one retry sweep with `--retry-provider-failures`:
  `empty_answer` 23, `no_tool_call` 11, `provider_timeout` 1 (35/384 = 9.1%),
  all excluded from every denominator and reported by name. `hallucinated_tool`: 0.
- **Excluded ledger**: `.work/toolcalling/records.jsonl` (the 9 E2.3 smoke
  records). They were written before the mock served real input schemas
  (`01M1W40XF2E0HKT05SS51A5PJ2`) and carry no time evidence, so they are neither
  comparable to this grid nor admissible under the ordering gate.

## Instrument caveats carried into every number above

- **This is selection, not answering.** Every tool call lands on a mock MCP that
  returns a deterministic stub and records the call. A correct selection here is
  no evidence that the real LCI tool then produces a correct answer (epic 1), and
  no evidence that an LCI-equipped agent beats grep end to end (discovery epic).
- **Only the first call is graded.** A run that reached the right tool third has
  mis-selected. `correct_ever` and `first_correct_position` are kept in the
  records for anyone who wants the softer metric; they are not in the matrix.
- **`native:*` columns are real selections.** When the agent opens with its
  built-in bash or read instead of any offered tool, that is the selection it
  made, and it keeps its own confusion column inside the denominator.
- **Non-selection outcomes are excluded from every denominator**
  (`provider_error`, `provider_timeout`, `provider_quota`, `empty_answer`,
  `exit_N`, `no_tool_call`, `hallucinated_tool`) and reported by name. A provider
  outage is not a wrong selection. Denominators therefore differ per cell, so
  every rate is quoted with its n.
- **`args_plausible` is a contract judgement**, validated against the committed
  `comprehension/surface/tool-surface.json` input schema. The mock now serves
  those same schemas, but the grader deliberately does not read them from the
  mock: an oracle that shares its contract with the thing under test cannot fail.
- **Spread is not a confidence interval.** With three reps the spread is the
  crudest possible noise estimate. It is used as a floor a delta must clear, not
  as a significance test; nothing here carries a p-value.

## Reproducing

```
# 1. the predictions, committed before any cell ran
git show db9f200:benchmarks/repo-qa/toolcalling/predictions.json

# 2. one rep of the grid (128 cells: 16 tasks x 4 variants x 2 models)
python3 benchmarks/repo-qa/scripts/selection_ab.py run --live \
  --records benchmarks/repo-qa/.work/toolcalling/rep1/shard0.jsonl \
  --retry-provider-failures

# 3. the analysis (deterministic: run twice, cmp)
python3 benchmarks/repo-qa/scripts/analyze_selection.py \
  --ledgers benchmarks/repo-qa/.work/toolcalling/rep*/shard*.jsonl \
  --predictions-commit-time 2026-09-06T20:43:08Z \
  --out benchmarks/repo-qa/toolcalling/analysis/selection-analysis.json
```

Ledgers live under `benchmarks/repo-qa/.work/toolcalling/` and are gitignored;
the committed artifact is the aggregate JSON, which holds no traces.
