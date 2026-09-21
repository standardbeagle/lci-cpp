# S6 — Stage-1 exploration paired baseline: configuration, execution accounting, initial report

Run config: `benchmarks/repo-qa/exploration/run-configs/stage1-baseline.json`
(`exploration_run_config_v1`, config_id `stage1-baseline`).
Machine-readable artifacts: `benchmarks/repo-qa/results/exploration-stage1/`
(`run-records.jsonl` → `scores.json` `exploration_score_set_v1` → `aggregate.json`
`exploration_aggregate_v1`).
Subject binary: `lci` 0.10.1, built from checkout commit `78b8f6fc3078f8f3f30182476e7bb2e5cbe268f1`.
**Provider spend: $0.00 — the paid grid was never launched (see "Why nothing was executed").**

This report establishes **observations only**. It pins the stage-1 configuration and
accounts for every cell of the grid. It does **not** report an LCI-vs-baseline effect,
because no cell was executed.

## Headline

The stage-1 experiment is **fully pinned, fully instrumented, and fully materialised**,
but **zero of its 60 planned cells were executed**, because a paid Claude CLI grid
requires a user-approved model and spending/time budget recorded before launch, and
this stage-1 configuration was pinned by an unattended implementer with no approver
and no provider credentials on the host.

The honest output of S6 is therefore a **complete cell accounting** — every planned
`(task, arm, rep)` cell is present in `run-records.jsonl` with the machine-readable
failure status `not_executed` and a reason — and a scoring pipeline that consumes
those records without laundering a failure into a zero-scored answer. **No completion,
evidence, token, or wall-clock delta between the arms exists yet.**

## Measured facts

These are reproducible on this checkout right now, with no provider call:

- **Task bank is well-formed.** `validate_exploration_tasks.py` exits 0:
  `30 task(s) {next.js: 11, pocketbase: 9, scikit-learn: 10}`,
  categories `{true: 5, wrong-layer: 5, misleading-doc: 5, dead-code: 5, false-premise: 5, unsupported: 5}` (6 × 5 = 30),
  author verdicts `{true: 10, false: 10, unsupported: 10}`.
- **All three corpora are present and integrity-clean.** For each of
  pocketbase / scikit-learn / next.js at seed 7, `corpus.locate_forged_corpus` +
  `forge.tree_hash` reproduce the `reference_tree_hash` committed in `corpora.json`
  (match = True), `manifest.status = ready`, `forge_version = 1` (matching the bank
  pins). The clean-checkout materialisation path is exercised by the runner unit suite.
- **The paired grid is 60 cells:** 30 tasks × 2 arms × `repetitions.per_cell = 1`.
- **Scoring is reproducible from the committed ledger.** `score_exploration.py` over
  `run-records.jsonl` re-derives `scores.json` and `aggregate.json` deterministically
  (re-run by CI; see "Reproduction").

## Configuration (pinned before any launch)

Recorded in `stage1-baseline.json`; the AC requires these before launching paid runs:

| Field | Value |
|---|---|
| Model (arm-shared) | `claude-sonnet-4-5` (harness default; **approval not granted**) |
| Repetitions / cell | 1 (variance-powered replication deferred — out of scope) |
| Spending budget | not set (no paid run authorized) |
| Time budget / cell | 600 s |
| Harness schema | runner mode `exploration`; answer `exploration_answer_v1`; prompt `claim-request-v1` |
| Scorer schema | score `exploration_score_v1`; set `exploration_score_set_v1`; aggregate `exploration_aggregate_v1` |
| Task-bank schema | `exploration_task_v1` (30 tasks) |
| Forge schema | registry `exploration_corpora_v1`; emitted `forge_version` 2; bank-pinned `1` |
| Source commits | pocketbase `d438c6a9…f15d`; scikit-learn `0885712e…b709`; next.js `97532172…065b` |
| Seeds | `[7]` |
| LCI binary commit | `78b8f6fc3078f8f3f30182476e7bb2e5cbe268f1` (v0.10.1) |
| Treatment allowlist | `Read` + 8 `mcp__lci__*` (browse_file, callers, find_files, get_context, inspect_symbol, list_symbols, references, search) |
| Baseline allowlist | `Glob`, `Grep`, `Read` (no LCI server attached) |
| Denied both arms | `Bash`, `Edit`, `Write` |

The single manipulated variable is the **tool surface**; model, system prompt,
timeout, and the clean checkout are byte-identical across arms, and the runner's
`gate.enforce` re-checks every emitted call against the effective allowlist.

## The paired comparison — measured status: not run

`aggregate.json` for the pinned configuration:

| Arm | cells | answered | success_rate | evidence P / R / F1 | tool calls (mean) | input / output tokens | wall-clock (s, mean) |
|---|---|---|---|---|---|---|---|
| treatment | 30 | 0 | 0.0 | none / none / none | 0 | null / null | 0.0 |
| baseline  | 30 | 0 | 0.0 | none / none / none | 0 | null / null | 0.0 |

Pairing: `paired_count = 0`, `unpaired_count = 60`. **`deltas` is empty** because the
scorer refuses to emit an LCI-minus-baseline delta when no arm completed — so there is
no precision, recall, F1, tool-call, token, or wall-clock difference to report. Every
`null` is deliberate: the scorer's `not_executed`/failure path returns NULL evidence so
an un-run cell can never masquerade as a scored empty answer.

## Failure / missing-cell accounting (complete — no omissions)

Per the AC, a cell that was not executed is recorded, not silently dropped. All 60
cells carry status `not_executed`, reason `not_executed:paid_run_approval_not_granted`:

- 30 × `treatment::…::seed-7` — `not_executed`
- 30 × `baseline::…::seed-7`  — `not_executed`

The full per-cell list is the `pairing.unpaired` array in `aggregate.json` and every
line of `run-records.jsonl`. There are **0 executed cells and 0 omitted cells.**

## Interpretation

- **What S6 did establish:** the stage-1 instrument is real and complete — bank
  validated, corpora verified byte-identical to their committed references, both arms
  defined and isolated, and a deterministic score/aggregate pipeline that turns a
  `not_executed` ledger into a correctly-empty comparison instead of a fabricated one.
- **What it did not establish:** any claim about LCI helping an agent explore these
  corpora. That requires the paid grid to actually run, which needs the governance
  precondition S6 could not satisfy: a named human approving the model and the
  spending/time budget **before** launch (design-unbiased-benchmarks stop conditions),
  plus live provider credentials. Absent those, launching (or, worse, launching and
  scoring the auth failures as answers) would be the invalid move, not the safe one.
- The `success_rate = 0.0` here is **not** a measured zero; it is the scorer's
  "no completed attempts" floor. It carries no information about the arms.

## Limitations and claim boundary

- Single corpus seed per task (seed 7), `reps = 1`: no variance estimate. Threshold
  selection, significance, and ≥3-rep replication are explicitly **out of scope** (a
  later task owns them).
- This report supports the claim "the stage-1 grid is pinned and instrumented" and
  nothing about LCI-vs-baseline effect.
- Minefield/difficulty axes are not used here; the only axis is the committed task
  bank's own categories.

## To execute the grid (deferred, gated on approval)

1. A named user records an approved `model` and spending/time budget into
   `stage1-baseline.json` and flips `approval.status` to `granted`.
2. With `~/.claude` authenticated (or `ANTHROPIC_API_KEY` set) and the forged
   corpora present, run
   `python3 benchmarks/repo-qa/scripts/exploration_runner.py run --model <approved> --timeout <cap>`
   to append live records to a `run-records.jsonl`, then re-run the scorer.
3. The informational CI below will then re-validate the (now answered) ledger and
   regenerate the tables; it never gates a merge on the resulting delta.

## Reproduction

```sh
# re-derive scores + aggregate from the committed ledger (no provider, deterministic)
python3 benchmarks/repo-qa/scripts/score_exploration.py \
  --tasks-dir benchmarks/repo-qa/exploration/tasks \
  --records  benchmarks/repo-qa/results/exploration-stage1/run-records.jsonl \
  --out-dir  /tmp/stage1-check
diff /tmp/stage1-check/aggregate.json benchmarks/repo-qa/results/exploration-stage1/aggregate.json
# task-bank integrity gate (structural layer; live layer when corpora present)
python3 benchmarks/repo-qa/scripts/validate_exploration_tasks.py
```
