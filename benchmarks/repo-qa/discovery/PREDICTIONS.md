# Pre-registered predictions — discovery sweep

`predictions.json` is the machine-readable registry; this file is its rationale.
D5 diffs measured outcomes against the JSON mechanically. **The JSON is the
record — this document explains it, it does not amend it.**

Registered **2026-07-17, before D4's runner executed anything.** A prediction
written after seeing results is not a prediction.

## Why this exists

The user's ask: *"also useful is finding where predictions are failing."* That
only works if the predictions are on record first. Without pre-registration a
surprising result gets silently reinterpreted as "expected", and the most
valuable signal in the epic is destroyed.

So each family records **two** things: `epic_intuition` (what the epic's plan
predicted) and `prediction` (what this registry commits to after calibrating
against D2's measured cohort data and the prior tiers' results). **Where those
two disagree is the deliverable.** Three families disagree; see below.

## What the sweep measures

The arms are **disjoint, not nested**:

| Arm | Tools |
|---|---|
| treatment | LCI MCP semantic tools + `Read` — **no Grep, no Glob** |
| baseline | `Grep`, `Glob`, `Read` — no LCI |

The treatment cannot fall back to grep. That is what gives
`control_literal_string_search` real teeth: it probes a task shape where the
baseline holds the natively-correct tool and the treatment does not.

Population is **functions and methods only** (all 48 cells are `kind=func`;
methods appear as funcs with a receiver). Types and `init`/`main` are excluded.
No prediction here presumes type symbols are in the sample.

## Calibration anchors

Two facts constrain every number below:

1. **Prior tiers measured dead accuracy parity** — base 0.96 vs LCI 0.96 — on a
   non-cohort-selected population. The epic's whole thesis is that cohort
   selection is what those tiers lacked. That thesis is itself unproven, so no
   confidence here exceeds 0.65 on an LCI win. Predicting blindly optimistic
   accuracy gains would assume the conclusion.
2. **LCI's only reproducible prior win was completion rate** — DNF 10.8% → 6.5%
   (4.3pp). That is the one family with a real prior behind it, and even that
   one is underpowered here (see below).

## Three findings from D2's sample that changed the predictions

### 1. The two "LCI wins" axes are disjoint — `fan_in_high ∩ noise_high = 0`

Every high-fan-in symbol in this corpus is **low-noise** (noise_ratio
1.02–3.42); every high-noise symbol has **low fan-in** (0–4). The epic's phrase
"exhaustive callers of a *common-named* symbol" conflates *many callers* with
*noisy name*. **This corpus contains no symbol that is both**, so no cell tests
the conjunction, and the sweep cannot test it at all.

Consequence: `callers_high_fan_in` is calibrated to **parity** (grep's hits
there are nearly all true — BindBody: 25 true refs at noise 1.07, readable
straight off grep output), against the epic's `lci_wins` intuition. The
collision-noise hypothesis lives in `callers_high_noise` alone.

### 2. `send` has fan_in 0 — an empty answer set

`noise_high` includes `send` (`tools/mailer/sendmail.go`) with **zero callers**.
It is the sharpest baseline trap in the pool (grep returns ~47 hits for a symbol
with no callers) — and a scoring hazard: **F1 is undefined on an empty oracle
set.** *Binding on D4: define empty-answer-set grading before running.* A
correct "there are none" must score as a win, not a division-by-zero or a silent
skip.

### 3. `impl_many` is n=2 **and** confounded

Its two cells are `Close` and `Write`. `Close` is also the **noisiest symbol in
the entire pool** (38.43). So even at adequate n, these cells could not separate
an implementations effect from a noise effect.

## The families

| # | Family | Cohort | n | Intuition | **Prediction** | Threshold | Conf | Power |
|---|---|---|---|---|---|---|---|---|
| 1 | callers_high_fan_in | fan_in_high | 6 | lci_wins | **parity** ⚠️ | \|Δ mean_f1\| ≤ 0.10 | 0.55 | underpowered |
| 2 | callers_high_noise | noise_high | 6 | lci_wins | **lci_wins** | Δ mean_precision ≥ 0.20 **+ unanimous 6/6** | 0.65 | underpowered |
| 3 | shadowed_definition | shadowing_multi | 17 | lci_wins | **lci_wins** | Δ mean_f1 ≥ 0.15 **+ sign p<0.05** | 0.60 | underpowered |
| 4 | transitive_callers_depth2 | fan_in_high | 6 | lci_wins | **lci_wins** | Δ mean_recall ≥ 0.20 **+ unanimous 6/6** | 0.60 | underpowered |
| 5 | referenced_outside_tests | refs_test_dominant | 10 | lci_wins | **parity** ⚠️ | \|Δ mean_f1\| ≤ 0.10 | 0.55 | underpowered |
| 6 | interface_implementations | impl_many | 2 | lci_wins | *directional only* | **none — untestable** | — | **untestable** |
| 7 | control_unique_name_definition | unique ∩ control | 26 | parity | **parity** | \|Δ mean_f1\| ≤ 0.05 | 0.80 | **powered** |
| 8 | control_literal_string_search | noise_control | 26 | parity/grep | **grep_wins** | Δ mean_f1 ≥ 0.10 (baseline−treatment) | 0.65 | **powered** |
| 9 | control_low_fan_in_callers | fan_in_low ∩ control | 16 | parity | **parity** | \|Δ mean_f1\| ≤ 0.05 | 0.75 | underpowered |
| 10 | token_budgeted_completion | full pool | 48 | lci_wins | **lci_wins** | Δ dnf_rate_pct ≥ 15.0pp **+ McNemar p<0.05** | 0.55 | underpowered |

⚠️ = **contradicts the epic's stated intuition.** These are not hedges: each is
falsified by any \|Δ\| > 0.10.

### The controls (criterion: ≥ 2 predicting parity/grep_wins — 3 registered)

- **#7 unique_name_definition** — *instrument null check.* If the treatment wins
  here, the instrument is biased toward LCI on definition lookups generally, and
  #3's result cannot be attributed to shadowing. Powered (n=26).
- **#8 literal_string_search** — *directional null check.* Proves the instrument
  can report an **LCI loss** at all. If no family can report `grep_wins`, the
  sweep is rigged and every LCI win it reports is uninterpretable. Powered (n=26).
- **#9 low_fan_in_callers** — *mechanism null check.* Same task shape and prompt
  as #1/#2 on the easy cell. **If the treatment wins here as well as on #2, LCI's
  advantage is a property of the callers task shape, not of collision noise** —
  which falsifies the epic's stated mechanism even while its outcome prediction
  appeared to succeed. #2 must never be read on its own.

### Oracle independence — binding on D4

**#8's oracle MUST NOT be `grep(1)`.** The baseline arm's entire mechanism *is*
grep; grading a grep arm with a grep oracle means the arm is graded by its own
matching mechanism — the shared-blind-spot failure
`.claude/rules/bench-harness-oracle-independence.md` forbids — and would bias
#8 toward `grep_wins` **by construction**, destroying the one control that
proves the instrument can report an LCI loss. Use an independently authored
literal scanner sharing no code object with the arm's tooling, and give it a
discrimination test: a known occurrence it must **find**, and a near-miss
(one byte / case differing) it must **not** report.

All other families use **gopls**, which shares no mechanism with either arm
(neither arm may run gopls).

## Statistical power — the honest part

The epic carries a **binding power warning**, verified directly against D2's
committed sample. It governs what may be claimed:

- **n ≤ 2 → untestable.** At n=2 the smallest attainable two-sided sign-test p is
  `2×(0.5²) = 0.50`. **No outcome — including a perfect 2/0 sweep — can reach
  significance.** #6 therefore carries **no threshold at all** and a **null
  confidence**: attaching a number to a claim the sample cannot test would dress
  an untestable statement up as a measurement. It is retained rather than
  dropped so the gap is on the record.
- **n = 6 → unanimous-only.** Only a 6/0 split reaches p=0.031 (`2×0.5⁶`). Below
  n=6 nothing can ever reach significance at any effect size. #2 and #4
  therefore require **unanimity** as part of the threshold. At 5/1 or worse the
  cell cannot separate a real effect from run-to-run variance — aggravated by
  model nondeterminism in the agent arm.
- **Power is per-metric, not a global cell count.** #10 has the *largest* n in
  the registry (48, the whole pool) and is still **underpowered**: DNF is a
  rare-event proportion, and detecting the prior's 4.3pp gap needs **n≈900 per
  arm** (expected counts at n=48 are ~5 vs ~3). Its 15.0pp threshold is **the
  detection floor of this sample, not a prediction of the prior's magnitude.** A
  gap between 4pp and 15pp is real-but-undetectable here and must be reported as
  such — neither a win nor a null.
- **Parity predictions cannot be *confirmed* at small n.** A met parity threshold
  on #1/#5/#9 is *"no separation detected, underpowered to detect one"* — never
  *"parity established"*.

### Non-independence

- #1 and #4 run over the **same 6 cells** (depth-1 vs depth-2).
- #3 and #2 overlap on **6 cells** (`shadowing_multi ∩ noise_high`).

D5 **must not** treat agreeing outcomes on these pairs as independent
corroboration.

### What D5 may not say

Per the epic's warning: if #2 (n=6) or #6 (n=2) trend LCI-positive, the honest
conclusion is **"directionally consistent, underpowered, needs a bigger or
different corpus"** — *not* a beachhead. Reporting a win off n=2 would be exactly
the weak-evidence disease this epic was built to avoid, and **worse than the
0.96-vs-0.96 null it replaces**.

If the high-noise class is where the beachhead lives, the next lever is **a
corpus with more such symbols** — not more repetitions over the same 6 cells.
This distribution is not D2's error: it is a deterministic draw over the real
candidate population, and the fact that PocketBase simply does not contain many
high-noise or many-implementor functions **is itself a finding**, surfaced
before a paid sweep rather than after.

## Amendment policy

`predictions.json` **must not be edited after D4 runs.** D5 records deltas
against it. If a family's prompt is later found broken, set `voided: true` with a
`void_reason` in a **new** commit and report the void explicitly. **Never**
retune a prediction, threshold, or confidence after seeing an outcome — that
would convert the record into a rationalisation and destroy the only thing this
slice produces.
