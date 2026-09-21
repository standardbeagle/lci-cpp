# S2.5 — Stage-2 claim-validation paired baseline: configuration, execution accounting, initial report

Run config: `benchmarks/repo-qa/exploration/run-configs/stage2-baseline.json`
(`claim_validation_run_config_v1`, config_id `stage2-baseline`).
Machine-readable artifacts: `benchmarks/repo-qa/results/exploration-stage2/`
(`run-records.jsonl` → `scores.json` `claim_validation_score_set_v1` →
`aggregate.json` `claim_validation_aggregate_v1`).
Subject binary: `lci` 0.10.1, built from checkout commit
`71f0c5892e6041126e6ccfc93cf77fba7c111fc9`.
**Provider spend: $0.00 — the paid grid was never launched (see "Why nothing was executed").**

Stage 2 asks a different question than stage 1: not *can the agent find the
code* but *does LCI's answer hold up*. Both arms read the same pinned corpora and
adjudicate the same 30 structured claims (`verdict ∈ {true, false, unsupported}` +
cited evidence) against an independent, schema-versioned oracle
(`exploration/scoring/claim_validation.py`) that needs **no LLM judge**.

This report establishes **observations only**. It pins the stage-2 configuration,
accounts for every cell, and reports **no LCI-vs-baseline effect**, because no cell
was executed.

## Headline

The stage-2 instrument is **fully pinned, fully instrumented, and fully
materialised**, but **zero of its 60 planned cells were executed**: a paid Claude
CLI claim grid requires a user-approved model and spending/time budget recorded
before launch, and this configuration was pinned by an unattended implementer with
no approver and no provider credentials on the host.

The honest output is a **complete cell accounting** — every planned
`(task, arm, rep)` cell is present in `run-records.jsonl` with the machine-readable
status `not_executed` and a reason — plus a scoring pipeline that consumes those
records **without laundering a failure into a zero-scored answer**. **No
completion, verdict, grounding, citation, token, or wall-clock delta between the
arms exists yet.**

## Measured facts

Reproducible on this checkout right now, with no provider call:

- **Task bank is claim-complete.** `validate_exploration_tasks.py` exits 0 over the
  same 30 tasks: `{next.js: 11, pocketbase: 9, scikit-learn: 10}`; author verdicts
  `{true: 10, false: 10, unsupported: 10}`; categories
  `{true, wrong-layer, misleading-doc, dead-code, false-premise, unsupported}` × 5.
  Every task carries the claim-mode fields (`claim`, `request`, `author.verdict`,
  `author.category`, `author.anchor_classification`).
- **Committed task-bank digest** is `sha256:5cc00f41…c47ff3f6`
  (`score_claim_validation._bank_digest`); the ledger's
  `aggregate.compatibility.task_bank_digest` matches it exactly.
- **The paired grid is 60 cells:** 30 claims × 2 arms × `repetitions.per_cell = 1`,
  each run_key suffixed `::mode-claim-validation::schema-claim_validation_answer_v1`.
- **The scoring pipeline is reproducible from the committed ledger.**
  `score_claim_validation.py` over `run-records.jsonl` re-derives `scores.json` and
  `aggregate.json` **byte-identically** (verified by `diff`; re-run by CI — see
  "Reproduction").
- **Instrument wiring is proven end-to-end, no provider.** A hermetic one-claim /
  two-arm smoke (`tests/test_claim_baseline_ledger.py::TwoArmClaimSmokeTest`) runs
  the real `runner.run.run_task_both_arms` in claim mode with a deterministic fake
  agent and scores **both arms `success = true`** for a grounded correct verdict —
  the runner gate and the scorer agree on the same answer schema. The guarded **live**
  `claim-smoke` **SKIPS** without `--live` / credentials, demonstrating the launch
  gate refuses an unapproved paid run rather than spending or failing-to-answer.

## Configuration (pinned before any launch)

Recorded in `stage2-baseline.json`; the acceptance criteria require these recorded
for sign-off before launching paid runs:

| Field | Value |
|---|---|
| Model (arm-shared) | `claude-sonnet-4-5` (harness default; **approval not granted**) |
| Repetitions / cell | 1 (variance-powered replication deferred — out of scope) |
| Spending budget | not set (no paid run authorized) |
| Time budget / cell · total | 600 s · 36 000 s (60 cells) |
| Harness schema | runner mode `claim-validation`; answer `claim_validation_answer_v1` |
| Scorer schemas | score `claim_validation_score_v1`; set `claim_validation_score_set_v1`; aggregate `claim_validation_aggregate_v1` |
| Oracle evidence rule | `evidence_min_valid = 1`, `evidence_min_recall = 0.0` (no LLM judge) |
| Task-bank schema | `exploration_task_v1` (30 tasks); schema-file `sha256:cbde51eb…ac1e444c` |
| Forge registry | `exploration_corpora_v1`; `sha256:0cdc5a36…a2fa4a0a`; bank-pinned `forge_version` 1 |
| Source commits | pocketbase `d438c6a9…f15d`; scikit-learn `0885712e…b709`; next.js `97532172…065b` |
| Seeds | `[7]` |
| LCI binary commit | `71f0c589…c111fc9` (v0.10.1) |
| Treatment allowlist | `Read` + 8 `mcp__lci__*` (browse_file, callers, find_files, get_context, inspect_symbol, list_symbols, references, search) |
| Baseline allowlist | `Glob`, `Grep`, `Read` (no LCI server attached) |
| Denied both arms | `Bash`, `Edit`, `Write` |

The single manipulated variable is the **tool surface**; model, system prompt,
timeout, evidence rule, and the clean checkout are byte-identical across arms, and
`gate.enforce` re-checks every emitted call against the effective allowlist. All
sign-off dimensions (model, reps, time/spend ceilings, task/manifest/schema digests,
source commits, seeds, LCI commit, both allowlists) are recorded in the config's
`approval.signoff_fields_recorded`.

## The paired comparison — measured status: not run

`aggregate.json` for the pinned configuration:

| Arm | cells | answered | verdict_acc | grounded_acc | citation P / R / F1 | false-premise | unsupported | tool calls | tokens | wall-clock |
|---|---|---|---|---|---|---|---|---|---|---|
| treatment | 30 | 0 | 0.0 | 0.0 | none / none / none | 0.0 | 0.0 | 0 | null / null | null |
| baseline  | 30 | 0 | 0.0 | 0.0 | none / none / none | 0.0 | 0.0 | 0 | null / null | null |

Pairing: `paired_count = 0`, `unpaired_count = 60`. **`deltas` is empty** because the
scorer refuses to emit an LCI-minus-baseline delta when no arm completed — there is
no verdict-accuracy, grounded-accuracy, citation, false-premise, unsupported, tool-call,
token, or wall-clock difference to report. Every `0.0` is the scorer's
**"no completed attempts" floor**, and every `none`/`null` is deliberate: the
`not_executed` path returns **NULL** evidence so an un-run cell can never masquerade
as a scored empty answer.

### Per-category verdict breakdown (complete; measured)

The `aggregate` records a full expected-verdict × predicted-verdict confusion per
category. With no answers, **every** cell lands in the `invalid` (no-prediction)
column of its expected-verdict row. This is the honest accounting that a per-category
rate would otherwise hide — 60 cells are *known missing*, not *known wrong*:

| category | count | expected verdict | predicted (all) |
|---|---|---|---|
| true | 5 | true | 5 × `invalid` |
| dead-code | 5 | true | 5 × `invalid` |
| wrong-layer | 5 | false | 5 × `invalid` |
| false-premise | 5 | false | 5 × `invalid` |
| misleading-doc | 5 | unsupported | 5 × `invalid` |
| unsupported | 5 | unsupported | 5 × `invalid` |

(`dead-code` / `wrong-layer` share the `true` / `false` oracle verdicts of their
sibling categories but are tracked separately so a future run can show *which*
failure mode the verdict accuracy hides.)

## Efficiency metrics — measured status: not run

Tool calls, input/output tokens and wall-clock per cell are all `0` / `null` in the
ledger because no agent was invoked; the `aggregate.arms[*].tool_calls` distribution
is empty and `attempts` history carries one `not_executed` row per run_key. No
cost or latency comparison exists.

## Failure / missing-cell accounting (complete — no omissions)

Per the acceptance criteria, a cell that was not executed is **recorded**, not
silently dropped. All 60 cells carry status `not_executed`, reason
`not_executed:paid_run_approval_not_granted`:

- 30 × `treatment::…::seed-7::mode-claim-validation::schema-claim_validation_answer_v1`
- 30 × `baseline::…::seed-7::mode-claim-validation::schema-claim_validation_answer_v1`

The full per-cell list is `pairing.unpaired` in `aggregate.json` and every line of
`run-records.jsonl`. There are **0 executed cells and 0 omitted cells.**

## Interpretation (separated from the measured facts above)

- **What S2.5 did establish:** the stage-2 instrument is real and complete — the
  same claim-ready bank validated, both arms defined and isolated, a deterministic
  verdict/grounding/citation oracle that needs no judge, and a score/aggregate
  pipeline that turns a `not_executed` ledger into a correctly-empty comparison
  instead of a fabricated one. The end-to-end wiring genuinely scores a grounded
  verdict on both arms (hermetic smoke).
- **What it did not establish:** any claim about LCI improving (or hurting) claim
  adjudication on these corpora. That needs the paid grid to run, gated on the
  precondition S2.5 could not satisfy: a named human approving model + spending/time
  budget **before** launch (design-unbiased-benchmarks stop conditions), plus live
  provider credentials. Launching — or scoring the resulting auth failures as answers
  — would be the invalid move, not the safe one.
- The `verdict_accuracy = 0.0` / `grounded_accuracy = 0.0` here are **not** measured
  zeros. They carry no information about the arms.

## Limitations, comparability, and claim boundary (vs stage 1)

- **Directly comparable to stage 1** on: the same 30 tasks, corpora, seed 7,
  `forge_version 1`, both arm allowlists, lci 0.10.1, model, reps=1, 600 s/cell,
  runner + gate + forge + resume-idempotency. **Not comparable to** stage 1 in
  *headline*: stage 1 measures cited-evidence precision/recall/F1 (find the right
  location); stage 2 measures verdict + grounded-verdict accuracy against an
  independent oracle (is the answer correct *given* the location).
- Single seed (7), `reps = 1`: no variance estimate. Threshold selection,
  significance, and ≥3-rep replication are **out of scope** (a later task owns
  them).
- Both stage 1 and stage 2 are **pinned-not-executed at $0 spend**, so neither
  yields an effect size. "Comparability" here means the *instruments* line up, not
  that any number can be diffed.
- This report supports the claim "the stage-2 grid is pinned and instrumented" and
  nothing about LCI-vs-baseline verdict quality.
- Oracle caveat inherited from the design: `false_premise` and `unsupported`
  detection are only meaningful once arms actually answer; with a `not_executed`
  grid the `invalid` confusion bucket is the only populated column.

## To execute the grid (deferred, gated on approval)

1. A named user records an approved `model` and spending/time budget into
   `stage2-baseline.json` and flips `approval.status` to `granted`.
2. With `~/.claude` authenticated (or `ANTHROPIC_API_KEY` set) and the forged
   corpora present, run the guarded smoke then the grid:
   `python3 benchmarks/repo-qa/scripts/exploration_runner.py claim-smoke --live --task <id>`
   then `... claim-run --model <approved> --timeout <cap>`
   to append live records to a `run-records.jsonl`, then re-score.
3. The informational CI below re-derives the committed aggregate from the ledger
   and surfaces the verdict/grounding/citation numbers — it never gates a merge.

## Reproduction

```sh
# re-derive scores + aggregate from the committed ledger (no provider, deterministic)
python3 benchmarks/repo-qa/scripts/score_claim_validation.py \
  --tasks-dir benchmarks/repo-qa/exploration/tasks \
  --records  benchmarks/repo-qa/results/exploration-stage2/run-records.jsonl \
  --out-dir  /tmp/stage2-check
diff /tmp/stage2-check/aggregate.json benchmarks/repo-qa/results/exploration-stage2/aggregate.json

# regenerate the whole ledger + scores + aggregate from config + bank (deterministic)
python3 benchmarks/repo-qa/scripts/claim_baseline_ledger.py

# task-bank integrity gate (structural layer; live layer when corpora present)
python3 benchmarks/repo-qa/scripts/validate_exploration_tasks.py

# hermetic claim-pipeline contract tests (fake agent; no provider)
python3 -m pytest benchmarks/repo-qa/tests/test_claim_validation_scorer.py \
                  benchmarks/repo-qa/tests/test_claim_baseline_ledger.py -q
```
