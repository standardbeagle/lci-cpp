# Stage-3 edits baseline — pinned, instrumented, not executed; integrated stage 1–3 report

This is the stage-3 (edit-mode) companion to the stage-1 exploration report
(`../ANALYSIS-exploration-stage1.md`) and the anchor for the stage-gated LCI
optimisation PR policy (`../../../docs/benchmarks/stage-gated-lci-pr-policy.md`).

Run config: `benchmarks/repo-qa/edits/run-config.json` (`edit_run_config_v1`,
config_id `stage3-edits-baseline`).
Machine-readable artifacts: `benchmarks/repo-qa/edits/results/baseline/`
(`run-records.jsonl` (empty) → `scores.json` (`edit_score_v1`, `[]`) →
`aggregate.json` (`edit_aggregate_v1`)).
Subject binary: `lci` 0.10.1, built from checkout commit
`e8489a108d615963a28fe9dddfd8236024456cee`.
**Provider spend: $0.00 — the paid grid was never launched (see "Why nothing was executed").**

**Scope of this report:** it *pins* the stage-3 configuration, *proves the
instrument end-to-end* with a two-arm smoke, *accounts for every planned cell*,
and *integrates* the three measurement stages into one PR-gating story. It
**reports no LCI-vs-baseline edit effect**, because no paid cell ran. Sections
below are labelled **FACT** (reproducible on this checkout now) or
**INTERPRETATION** (what the facts do and do not support).

---

## Headline (FACT)

The stage-3 grid is **fully pinned and fully instrumented**, and **0 of its 48
planned cells were executed**. The committed `aggregate.json` reports:

| | treatment | baseline |
|---|---|---|
| planned cells (task×arm×seed-7) | 24 | 24 |
| observed cells | 0 | 0 |
| all-three-gates pass (`all_pass`) | 0 | 0 |
| **all-three-gates pass rate** (over planned) | **0.0** | **0.0** |
| `headline.complete` | **false** | **false** |

The rate is **0.0 because the scorer's denominator is the PLANNED grid**, not the
observed one: an un-run cell counts *against* its arm, so a missing grid cannot
inflate a pass rate by leaving the divisor. `headline.complete = false` is the
machine-readable flag that these rates are **not trustworthy as a comparison** —
they carry no information about either arm.

---

## What a cell must pass — the all-three-gates conjunction (FACT)

The headline is a **conjunction of three independently-evaluated gates** on the
agent's *captured patch* (never the agent's self-report), judged by an oracle
that shares no mechanism with LCI:

| Gate | Conjunction of | Evaluator |
|---|---|---|
| **behaviour** | `existing_suite ∧ discrimination` | `edits/oracles/oracle_gate.py` (`oracle_gate_v1`) |
| **convention** | `agent_patch ∧ bank_health` | `edits/conformance/conformance_gate.py` (`conformance_gate_v2`) |
| **blast** | `changed_path ∧ api_impact` | `edits/oracles/oracle_gate.py` (`oracle_gate_v1`) |

An **absent gate is a FAIL** (`GATE_ABSENT`), never an unknown or a pass — so a
cell that never reached a verdict is charged, not excused. The gates run *after*
the patch is captured, and the agent never sees oracle material (its prompt is
the goal prompt only; its tree is the pristine corpus).

---

## Full matrix (FACT — zero-filled)

The scorer always materialises the eight-cell `behaviour × convention × blast`
matrix (bit order behaviour, convention, blast) over each arm, so a missing cell
cannot hide by shrinking the table. With nothing observed, every entry is 0:

| matrix key | treatment | baseline |
|---|---|---|
| `TTT` (all three pass) | 0 | 0 |
| `TTF` | 0 | 0 |
| `TFT` | 0 | 0 |
| `TFF` | 0 | 0 |
| `FTT` | 0 | 0 |
| `FTF` | 0 | 0 |
| `FFT` | 0 | 0 |
| `FFF` | 0 | 0 |
| **sum** | **24 planned, 0 observed** | **24 planned, 0 observed** |

Per-gate rates (all over the planned denominator): behaviour 0/0 → 0.0,
convention 0/0 → 0.0, blast 0/0 → 0.0 for both arms. The `judged` count is 0 for
every gate, distinguishing "failed" from "never evaluated" at a glance.

---

## Paired arm deltas (FACT — empty by design)

`aggregate.pairing`: `paired_count = 0`, `unpaired_count = 0`.
`aggregate.deltas`: **`{}` (empty)**.

The scorer **refuses to emit an LCI-minus-baseline delta when no arm completed**
(`_deltas` returns `{}` if either arm has no paired observation). There is
therefore **no** all-three-gates-rate delta, per-gate delta, tool-call delta,
token delta, or wall-clock delta to report. This emptiness is deliberate: a
delta computed over an un-run grid would be a fabrication.

---

## Efficiency / process metrics (FACT — null, not zero)

| Metric | treatment | baseline |
|---|---|---|
| tool calls (mean/spread) | null | null |
| input tokens | null | null |
| output tokens | null | null |
| wall-clock seconds | null | null |

All process distributions are **`null`** because no cell produced a record.
`null` is not `0.0`: the scorer distinguishes "no measurement" from "measured
zero". No efficiency claim (tokens-per-answer, calls-per-patch) is supported.

---

## Uncertainty / variance (FACT — not estimable in the committed bank)

- **Repetitions pinned:** `repetitions.per_cell = 1`.
- **Independent seeds available in the committed bank:** exactly one (`seed 7`).
- **Spread / significance:** **not computable.**

A *repetition* in this scorer is a distinct `(task, arm, seed)` cell. All 24
committed edit tasks are authored and dual-adjudicated against the **single**
seed-7 forged corpus (`corpora.json` `reference_tree_hash["seed-7"]`). To obtain
`≥3` independent replications you must **re-forge and re-adjudicate** seeds 8, 9,
… (new mutated trees + new oracle keys + new annotator verdicts), which no
committed bank provides. Therefore **no variance, no confidence interval, and no
minimum-successes gate exists yet**, and any pass-rate difference would be a
single-sample observation, not a powered estimate.

---

## Failure / missing-cell accounting (FACT — complete, no omissions)

Every planned cell is accounted for. All 48 cells carry the scorer's explicit
`missing_cell` status (bucketed under `failure_reasons.missing_cell = 24` per
arm) and appear in `aggregate.completeness.missing` (24 treatment + 24 baseline
`{task_id, arm, seed}`). There are **0 executed cells, 0 omitted cells, 0
unplanned stray records**, and `completeness.passed = false`.

**Why `missing_cell` and not a fabricated `not_executed` record:** the edit
status taxonomy (`edit_record.ALL_STATUSES`) has no `not_executed` member, and
`edit_scorer._STATUS_FAILURE` would mis-bucket an unknown status as
`harness_failure` — laundering a *governance* decision (run not approved) into a
*tooling-fault* signal. The scorer's purpose-built **`missing_cell`** path is the
correct, honest representation of "planned, not run", and it is what
`aggregate.json` uses.

---

## Two-arm smoke (FACT — passes, no provider)

Two independent pre-flight smokes, both run on this checkout with **$0 spend**:

1. **Hermetic pipeline smoke** — `scripts/edit_pipeline_smoke.py`
   (`results/smoke/`). One task, **both arms**, a deterministic fake agent, a
   trivial argv behaviour command, driven through the **real** runner primitive
   (`run_edit_task_both_arms`), the **real** behaviour/convention/blast gates,
   the **real** scorer, and JSON-schema validation.
   **Result: PASS** — both arms reach `edit_passed` (all three gates green),
   `completeness.passed = true`, headline rate 1.0 over 2 planned cells, and the
   artifacts are **byte-stable across re-runs** (wall-clock pinned out — the fake
   agent's latency is harness jitter, not a measured quantity).
   This proves the grid would be *scored honestly* if it ran.
2. **Guarded live smoke** — `exploration_runner.py edit-smoke` (no `--live`).
   **Result: SKIP, exit 0** — `not opted in (requires --live AND a resolvable
   claude CLI AND the lci binary)`. This is the fail-closed guard that refuses to
   spend credentials on an unapproved/unable launch.

The smoke is a **necessary but not sufficient** precondition for the paid run:
it validates the instrument, not the hypothesis. AC "one-task/two-arm smoke
passes before the paid run" is satisfied by (1); the paid run is gated on (2)
being able to run under an *approved* config.

---

## Why nothing was executed (FACT)

The task's own blocker list requires **"explicit paid-run configuration
signoff"** before launch, and `run-config.json` records `approval.status =
not_granted`. This config was pinned by an **unattended implementer** with:

- **no named approver** present to approve the model and the spending/time budget
  *before* launch (the `design-unbiased-benchmarks` stop condition), and
- **no provider credentials** on this host (`ANTHROPIC_API_KEY` /
  `ANTHROPIC_AUTH_TOKEN` unset).

Under those conditions, launching — or launching and scoring the auth failures as
answers — would be the **invalid** move. `AGENTS.md` forbids fabricating test
data. So the grid is pinned and instrumented and left unexecuted, with spend
recorded at **$0.00**.

---

## Integrated view: how the three stages jointly gate an LCI optimisation PR

| Stage | Instrument | Status | Gate power today |
|---|---|---|---|
| **1 — exploration** (find the right code) | 30-task bank, LCI-vs-lexical arms, cited-evidence precision/recall | Pinned + materialised, **0 executed** (same approval reason) | informational only |
| **2 — claim validation** (does LCI's *answer* hold) | `claim_validation.py` + `score_claim_validation.py` + `claim-run`/`claim-smoke` | **PENDING — no committed baseline ledger/report** | not yet a gate |
| **3 — edits** (does LCI help the agent *change* the code correctly) | 24-task bank, all-three-gates conjunction | Pinned + smoke-proven, **0 executed** (approval not granted) | informational only |

The policy that ties them together lives in
`docs/benchmarks/stage-gated-lci-pr-policy.md`. In one line: **an LCI optimisation
PR is judged by whether it moves stage-3 all-three-gates correctness without
regressing stage-1 exploration evidence and, once stage 2 is baselined, without
introducing unsupported stage-2 claims — and none of the three may be turned into
a hard merge threshold until its variance is powered.**

### Interpretation

- **Established:** the stage-3 instrument is real and complete — 24 well-formed
  tasks, three corpora pinned to commits/tree-hashes/seed, disjoint arms
  isolated by the tool-allowlist gate, three independently-evaluated gates that
  fail closed, a deterministic scorer/aggregate, a green two-arm smoke, and a
  complete 48-cell accounting that turns "not run" into `missing_cell` rather
  than a fake zero or a silent gap.
- **Not established:** whether LCI helps an agent produce correct patches on
  these treacherous corpora. That needs the paid grid to run under an approved
  config with live credentials.
- **Explicitly not claimed:** any *threshold* on the LCI-vs-baseline delta. With
  one seed and one repetition per cell there is **no variance estimate**, so
  pinning a merge threshold now would be un-powered and gameable (see the policy
  doc). The `success_rate`/`rate = 0.0` and `headline.complete = false` are the
  scorer's honest "no completed attempts / incomplete grid" floor, not evidence.

---

## Known limitations and claim boundaries

- **No effect measured.** This report supports "the stage-3 grid is pinned,
  instrumented and smoke-proven" and **nothing** about LCI-vs-baseline edit
  quality, cost, or speed.
- **Single seed, `reps = 1` → no variance.** Thresholds, significance, and ≥3-rep
  replication are out of scope and gated on a re-forged/re-adjudicated bank.
- **Stage 2 not baselined.** The claim-validation instrument exists but has no
  committed stage-2 baseline; it is integrated as **PENDING**, not folded into
  any headline.
- **Blast `api_impact` depends on LCI's own reference resolution** on the mutated
  tree — the *blast* half of a passed cell is partly self-referential to the
  subject; behaviour and convention are not. Treat a blast-only pass with
  suspicion (the conjunction is the guard).
- **Arms are disjoint by construction but the baseline keeps `Edit`/`Write`**
  (both arms must be able to patch); the *only* manipulated variable is the
  navigation surface (LCI MCP + `Read` vs `Glob`/`Grep` + `Read`), with `Bash`
  denied and re-checked per call.
- **Corpora, raw transcripts, and credentials stay uncommitted.** The multi-hundred-
  MB forged trees live under gitignored `.work/`, transcripts are written to an
  ephemeral `--work-root` and referenced by path, and no provider key is stored
  in any artifact.

---

## To execute the grid (deferred, gated on approval)

1. A named user records an approved `model` and spending/time budget into
   `run-config.json` and flips `approval.status` to `granted`.
2. With the Claude CLI authenticated (`~/.claude` or `ANTHROPIC_API_KEY`) and the
   forged corpora present under `.work/exploration/`, run the guarded smoke live
   first (`exploration_runner.py edit-smoke --live`), then the grid:
   ```sh
   python3 benchmarks/repo-qa/scripts/exploration_runner.py edit-run \
     --model <approved> --timeout <cap> \
     --corpus-root benchmarks/repo-qa/.work/exploration \
     --records benchmarks/repo-qa/edits/results/baseline/run-records.jsonl \
     --work-root benchmarks/repo-qa/.work/edits-run
   ```
3. Re-derive the artifacts and re-run the pipeline smoke:
   ```sh
   python3 benchmarks/repo-qa/scripts/edit_pipeline_smoke.py
   python3 benchmarks/repo-qa/scripts/edit_baseline_ledger.py
   ```
   With a live ledger present, `aggregate.json`'s `observed_cells`, rates, matrix,
   `deltas` and process metrics populate; the report's FACT tables are then
   regenerated from it. This pipeline never gates a merge on the resulting delta.

---

## Reproduction (deterministic, no provider)

```sh
# 1. two-arm pipeline smoke (must print SMOKE PASS, exit 0)
/usr/bin/python3 benchmarks/repo-qa/scripts/edit_pipeline_smoke.py --out-dir /tmp/smoke
# 2. re-derive baseline scores + aggregate from the committed (empty) ledger
/usr/bin/python3 benchmarks/repo-qa/scripts/edit_baseline_ledger.py \
  --records benchmarks/repo-qa/edits/results/baseline/run-records.jsonl \
  --out-dir /tmp/baseline
diff /tmp/baseline/aggregate.json benchmarks/repo-qa/edits/results/baseline/aggregate.json
# 3. structural bank validation
/usr/bin/python3 benchmarks/repo-qa/scripts/validate_edit_tasks.py
# 4. contract + scorer suite (928 repo-qa tests incl. 63 edit tests)
/usr/bin/python3 -m pytest benchmarks/repo-qa/tests -q
```
