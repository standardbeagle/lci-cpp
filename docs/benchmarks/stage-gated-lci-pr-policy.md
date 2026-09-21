# Stage-gated LCI optimisation PR policy (stages 1–3)

**Status: informational today; becomes a merge gate only where powered.**
This document defines how the three agent-level benchmark stages — **stage 1
exploration**, **stage 2 claim validation**, **stage 3 edits** — jointly gate a
pull request that optimises LCI's agent-facing behaviour. It is written to be
**non-gameable**: a PR cannot manufacture a pass by editing the yardstick, and no
threshold is enforced before the variance exists to support it.

Companion artifacts:
- Stage-1 report: `benchmarks/repo-qa/ANALYSIS-exploration-stage1.md`
- Stage-3 report: `benchmarks/repo-qa/edits/REPORT.md`
- Pinned configs: `benchmarks/repo-qa/exploration/run-configs/stage1-baseline.json`,
  `benchmarks/repo-qa/edits/run-config.json`
- Methodology: `.agents/skills/design-unbiased-benchmarks`,
  `.agents/skills/audit-benchmark-evidence`

---

## 1. What the stages measure

| Stage | Question | Instrument | Headline |
|---|---|---|---|
| **1 exploration** | Can the agent *find* the right code? | 30-task bank, LCI-vs-lexical arms | cited-evidence precision/recall/F1 vs adjudicated key |
| **2 claim validation** | Does LCI's *answer* hold up? | `exploration/scoring/claim_validation.py`, `score_claim_validation.py` | structured-claim verdict vs independent oracle |
| **3 edits** | Can the agent *change* the code correctly? | 24-task bank, both arms patching | **all-three-gates** pass: behaviour ∧ convention ∧ blast |

The stages are **ordered by cost**: exploration (read-only, cheap) → claim
validation (structured, mid) → edits (produce + execute + adjudicate a patch,
paid and expensive). A change must clear the cheap, precise stages before it is
worth pricing on the expensive one.

---

## 2. The gating rule

A PR that changes LCI's agent-facing behaviour (search/navigation, `get_context`
hydration, tool descriptions/schemas, def/refs/callers, side-effect/purity
labelling) is handled as follows.

**Hard, already-enforced gates (unchanged by this policy):**
- integration **goldens** — the sole correctness oracle for LCI itself
  (`tests/integration/goldens/`, `lci_integration_suite`);
- ctest unit/integration suites and the perf gate (`bench_gate.py`);
- `pytest benchmarks/repo-qa/tests` (contract tests incl. `test_tool_surface.py`).

**Benchmark-stage gates:**

1. **Informational (now).** Run stages 1 and 3 over the pinned configs on the
   PR's branch and on base; **report the paired deltas** in the PR. No merge is
   blocked on the delta value. A PR **must not claim** an LCI improvement that is
   backed only by an unexecuted or single-sample grid.
2. **Gate, stage 1, once powered.** When stage 1 has a `granted` approval,
   `≥3` independent seeds and a passing completeness, a *regression* in the
   treatment arm's cited-evidence F1 (beyond the measured spread) becomes
   merge-blocking.
3. **Gate, stage 3, once powered.** Same precondition: a *drop* in the
   all-three-gates pass rate, or a rise in `harness_failure`/`oracle_failure`
   buckets (which signal instrument rot, not agent error), becomes merge-blocking.
   **`patch_rejected` is the only bucket that is evidence the agent was wrong** —
   the gate must not fire on `missing_cell`/`harness_failure`/`oracle_failure`, or
   it will block PRs for broken tests.
4. **Stage 2 folds in when baselined.** Once a stage-2 baseline ledger exists, a
   PR may not add LCI *claims* that the claim-validation oracle rejects.

**No numeric threshold is fixed in this document.** The first powered stage 1/3
baseline will, by PR, *measure* the natural spread and *then* propose a concrete
bar (with its confidence interval) in a separate review. Fixing a number before
variance exists is the primary gameability hazard and is explicitly prohibited.

---

## 3. Non-gameability guarantees

The gate is only meaningful if a PR cannot pass it by degrading the yardstick.
Each hazard and its structural guard:

| Hazard | Guard (where enforced) |
|---|---|
| **Editing the oracle to "make it pass"** (Goodhart) | Tasks/oracles/adjudications live in `edits/tasks`, `edits/oracles`, `edits/annotations`; the agent's checkout is the **pristine corpus** and its prompt is the **goal prompt only** — oracle material is structurally withheld. A PR that changes the bank must carry **independent re-adjudication** (two new annotator verdicts) and is reviewed on that diff alone, never bundled with a behaviour change that it would "fix". |
| **One lucky sample clearing a bar** | No gate fires before `≥3` independent seeds (`repetitions` require a re-forged, re-adjudicated bank — see stage-3 REPORT). |
| **Shrinking the denominator / dropping hard cells** | Headline denominator is the **planned** grid fixed by `run-config.json`; missing cells count against (`completeness.passed` must be true before any delta is quotable). |
| **Arm leakage** (baseline gets LCI, or treatment sees the key) | Disjoint tool-allowlists + per-call `gate.enforce`; `Bash` denied both arms; the `test_tool_surface.py` snapshot and the disjointness smoke block silent drift. |
| **Self-referential oracle** (blast uses LCI's own refs) | The headline is a **conjunction**: behaviour ∧ convention (both LCI-independent) must also pass, so a blast-only pass never counts. Documented as a limitation. |
| **Cost/patch gaming** (a no-op patch trivially "in blast radius") | behaviour forces a real red→green functional change; convention forces house style on the agent's own patch; blast caps changed files. A do-nothing patch fails behaviour, not passes blast. |
| **Judge-model drift** | Stage 3 uses **no LLM judge** — deterministic gates. Stage 2's claim oracle is schema-versioned (`edit_score_v1` etc.). |
| **Silent paid re-run / spend games** | `run-config.json` pins approval (`granted_by`, `granted_at`, budget) *before* launch; the runner is **resume-idempotent per cell** (`task_id::arm::seed` + `task_digest`); completed cells are never re-run, only retryable infra classes. A not-run cell is `missing_cell`, never a fabricated answer. |

---

## 4. Provenance invariants (each run must pin, in the config)

`schema`/`config_id`, model id (verified against the live provider),
repetitions and per-cell + total budgets, per-corpus `source_commit` +
`reference_tree_hash` + `seed`, task/gate/score/aggregate **schema versions**,
the LCI binary commit + reported version, and the **per-arm tool allowlists** +
denied set. The aggregate's `compatibility`/`grouped_by` fields must be empty
(no silently-mixed models, forge versions, or bank versions) for a comparison to
be quotable.

---

## 5. Uncommitted-ness (hard rule)

Generated corpora, credentials, and raw oversized transcripts are **never
committed**:
- forged corpora regenerate from `(source_commit, forge_version, seed)` and live
  under gitignored `benchmarks/repo-qa/.work/`;
- provider keys are read from the environment, never stored in any artifact;
- raw agent transcripts are written to an ephemeral `--work-root` and referenced
  by path only; committed ledgers keep token counts and gate verdicts, not
  full transcripts (stage-3 smoke additionally pins out ephemeral timing for
  byte-stability).

Committed evidence is limited to: the pinned configs, the per-cell **ledgers**
(`run-records.jsonl`), the versioned `scores.json`/`aggregate.json`, the human
reports, and the scripts that regenerate them — all of which re-derive
deterministically without a provider call.

---

## 6. Lifecycle summary

```
PR touches agent-facing LCI behaviour
        │
        ├─ goldens + ctest + pytest repo-qa tests   ── HARD (must pass)
        │
        ├─ stage-1 exploration delta (informational) ── report only, until powered
        ├─ stage-3 edits delta       (informational) ── report only, until powered
        │
        └─ when powered (≥3 seeds, approval granted, completeness passed):
              stage-1 F1 regression / stage-3 all-three-gates drop
              on the PATCH-REJECTED bucket ── merge-blocking
              any instrument-rot bucket   ── block, fix the instrument (not the PR)
```

The benchmark stages **corroborate** the correctness story the goldens already
tell; they gate merges only on evidence that is *powered, approved, and
independently adjudicated*. Until then they publish deltas and say nothing more.
