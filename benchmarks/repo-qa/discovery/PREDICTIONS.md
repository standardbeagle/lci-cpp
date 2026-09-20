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
against D2's measured data and the prior tiers). **Where those two disagree is
the deliverable.** Two families disagree; see the table.

## What the sweep measures

The arms are **disjoint, not nested**:

| Arm | Tools |
|---|---|
| treatment | LCI MCP semantic tools + `Read` — **no Grep, no Glob** |
| baseline | `Grep`, `Glob`, `Read` — no LCI |

The treatment cannot fall back to grep. That is what gives
`control_literal_string_search` real teeth: it probes a task shape where the
baseline holds the natively-correct tool and the treatment does not.

The treatment's reference set comes from `mcp__lci__get_context` (relationships
mode), **not** a dedicated `references` tool — no such tool is on the live
`lci mcp` `tools/list` surface. `mcp__lci__callers` *is* on the surface and is a
registered arm tool. An earlier draft listed a phantom `mcp__lci__references`; it
was removed pre-run (see `registry_findings.phantom_reference_tool`), and
`tests/test_predictions_registry.py` now pins every `mcp__lci__*` arm name to the
committed tool-surface manifest so the registry cannot drift from the product.

Population is **functions and methods only** (all 48 cells are `kind=func`;
methods appear as funcs with a receiver). Types and `init`/`main` are excluded.

## Calibration anchors

1. **Prior tiers measured dead accuracy parity** — base 0.96 vs LCI 0.96 — on a
   non-cohort-selected population. The epic's thesis is that cohort selection is
   what those tiers lacked. That thesis is itself unproven, so no confidence here
   exceeds 0.65 on an LCI win.
2. **LCI's only reproducible prior win was completion rate** — DNF 10.8% → 6.5%
   (4.3pp). The one family with a real prior behind it — and even that one is
   underpowered here.

## The denominator rule

> **A family's difficulty statistic must divide by the same thing its answer set
> counts.**

This is the correction that defines schema_version 2, and it is a finding about
the harness rather than a wording fix.

D2's `noise_ratio` divides grep hits by **true references**. A callers task is
graded against **call sites**. Version 1 of this registry argued every callers
mechanism on `noise_ratio` — the wrong denominator — and the two diverge sharply:

| Symbol | `noise_ratio` (per reference) | call-site noise (per call site) |
|---|---|---|
| `LastMessage` | **0.94** — sits in D2's `noise_control` | **45.0** (45 hits, 1 call site) |
| `nextFunc` | 2.0 | 8.0 — exactly `noise_high`'s entry bar |
| `BindFunc` | 3.42 | 8.12 |

So version 1's "easy" callers control contained a 45x cell, and its
`callers_high_fan_in` parity call was argued from BindBody at 1.20x — **the most
favourable of that cell's six symbols**. Both the cells and the mechanism were
wrong.

The families are now derived on `call_site_noise = grep_hit_count / fan_in`,
using **D2's own published thresholds** (`control_max_noise=2.0`,
`high_min_noise=8.0`) rather than new ones — so the re-derivation cannot become a
tuning knob. **No threshold in this registry was chosen to produce a convenient n.**

Correcting the denominator **roughly doubled** the sample for the epic's central
hypothesis: n=6 (on the wrong axis) → **n=13**.

This is **not a defect in D2**: `noise_ratio` is correct for the reference-set
task it was built for, and D2 committed the raw `grep_hit_count` and `fan_in`
fields that made this re-derivation possible without touching its file.
*Follow-up for the epic: D2's cohort file would be more usable if it published
`call_site_noise` alongside `noise_ratio`.*

The same class of error hit the definition-lookup control: version 1 claimed grep
"lands on the definition within one or two hits", true for only **3 of its 26
cells** (max 47, median 7). Definition-lookup difficulty is **`def_count`**, not
hit count — with `def_count=1` exactly one hit is definition-shaped however many
grep prints. Fixing the axis raised that control from n=26 to **n=31**.

## Other findings from D2's sample

### The two "LCI wins" axes are disjoint — `fan_in_high ∩ noise_high = 0`

Every high-fan-in symbol is low reference-noise (1.02–3.42); every high-noise
symbol has fan_in 0–4. The epic's phrase "exhaustive callers of a *common-named*
symbol" conflates *many callers* with *noisy name*. **No cell is both**, so the
sweep cannot test the conjunction. The noise question is carried by
`callers_call_site_high`, the enumeration question by `transitive_callers_depth2`.

### Five cells have `fan_in=0` — empty answer sets

`send`, `callFieldInterceptors`, `finalizeActivePropsProcessing`, `Benchmark`,
`shouldEscape`. They are **excluded from every call-site family by the stated
`fan_in >= 1` rule** (call-site noise is undefined at zero — the helper raises
rather than defaulting), so the exclusion is principled, not a cherry-pick. They
remain in the pool for the DNF family.

*Binding on D4: define empty-answer-set grading before running.* F1 is undefined
on an empty oracle set, and a correct "there are none" must score as a win, not a
division-by-zero or a silent skip. These are also the sharpest baseline traps in
the pool — `send` returns ~47 grep hits for a symbol with **zero** callers.

### `impl_many` is n=2 **and** confounded

Its cells are `Close` (67.2x) and `Write` (14.0x) — both also `call_site_high`
members. Even at adequate n they could not separate an implementations effect
from a noise effect.

## The families

| # | Family | Axis | n | Intuition | **Prediction** | Threshold | Conf | Power |
|---|---|---|---|---|---|---|---|---|
| 1 | callers_call_site_high | call-site noise ≥8 | 13 | lci_wins | **lci_wins** | Δ precision ≥ 0.20 **+ sign p<0.05** | 0.65 | underpowered |
| 2 | shadowed_definition | def_count 2–31 | 17 | lci_wins | **lci_wins** | Δ f1 ≥ 0.15 **+ sign p<0.05** | 0.60 | underpowered |
| 3 | transitive_callers_depth2 | fan_in 9–28 | 6 | lci_wins | **lci_wins** | Δ recall ≥ 0.20 **+ unanimous 6/6** | 0.60 | underpowered |
| 4 | referenced_outside_tests | refs | 10 | lci_wins | **parity** ⚠️ | \|Δ f1\| ≤ 0.10 | 0.55 | underpowered |
| 5 | interface_implementations | impl_many | 2 | lci_wins | *directional only* | **none — untestable** | — | **untestable** |
| 6 | control_unique_name_definition | def_count=1 | 31 | parity | **parity** | \|Δ f1\| ≤ 0.05 | 0.80 | **powered** |
| 7 | control_literal_string_search | — | 26 | parity/grep | **grep_wins** ⚠️ | Δ f1 ≥ 0.10 (baseline−treatment) | 0.65 | **powered** |
| 8 | control_callers_call_site_control | call-site noise ≤2 | 9 | parity | **parity** | \|Δ precision\| ≤ 0.05 | 0.75 | underpowered |
| 9 | token_budgeted_completion | full pool | 48 | lci_wins | **lci_wins** | Δ DNF ≥ 15.0pp **+ McNemar p<0.05** | 0.55 | underpowered |

⚠️ = contradicts the epic's stated intuition. Not hedges — each is falsified by a
concrete margin.

**Retired, not silently dropped:** `callers_high_fan_in` (depth-1 over
`fan_in_high`, n=6) was registered in v1 and is not carried forward. On the
correct axis its cells span **1.20–8.12x** — control-grade to high-grade by D2's
own thresholds — so it confounds enumeration load with collision noise and can
isolate neither. Its questions are carried by #1 and #3.

### The controls (criterion: ≥ 2 predicting parity/grep_wins — 3 registered)

- **#6 unique_name_definition** — *instrument null check.* If the treatment wins
  here, the instrument is biased on definition lookups generally and #2 cannot be
  attributed to shadowing. **Powered (n=31).**
- **#7 literal_string_search** — *directional null check.* Proves the instrument
  can report an **LCI loss** at all. If no family can report `grep_wins`, the
  sweep is rigged and every LCI win is uninterpretable. **Powered (n=26).**
- **#8 callers_call_site_control** — *mechanism null check.* Same task shape,
  same axis, easy end (≤2 hits/call site vs 8–67). **If the treatment wins here
  too, LCI's advantage is task shape or tool affordance, not collision noise** —
  falsifying the epic's mechanism even while its outcome prediction appears to
  succeed. #1 must never be read alone.

`control_callers_call_site_control` is **not widened** to reach a better n: the
>2.0 cells are exactly the ones whose presence would defeat its purpose. Its n=9
weakness is recorded rather than traded away.

### Oracle independence — binding on D4

**#7's oracle MUST NOT be `grep(1)`.** The baseline arm's entire mechanism *is*
grep; grading a grep arm with a grep oracle means the arm is graded by its own
matching mechanism — the shared-blind-spot failure
`.claude/rules/bench-harness-oracle-independence.md` forbids — biasing #7 toward
`grep_wins` **by construction** and destroying the one control that proves LCI can
lose. Use an independently authored literal scanner sharing no code object with
the arm's tooling, with a discrimination test: a known occurrence it must
**find**, and a near-miss (one byte / case) it must **not** report.

All other families use **gopls**, which shares no mechanism with either arm.

## Statistical power — the honest part

The epic's **binding power warning** governs what may be claimed:

- **n < 6 → untestable.** At n=5 the best attainable two-sided sign-test p is
  `2×0.5⁵ = 0.063`; at n=2, `0.50`. **No outcome can reach significance at any
  effect size** — untestability here is arithmetic, not judgement. #5 therefore
  carries **no threshold** and a **null confidence**: a number on a claim the
  sample cannot test would dress an untestable statement up as a measurement. It
  is retained so the gap is on the record.
- **n = 6 → unanimous-only.** Only 6/0 reaches p=0.031. #3 requires unanimity.
- **Power is per-metric, not a global cell count.** #9 has the *largest* n (48)
  and is still **underpowered**: DNF is a rare-event proportion, and the prior's
  4.3pp gap needs **n≈900 per arm**. Its 15.0pp threshold is **this sample's
  detection floor, not a prediction of that magnitude.** A gap between 4pp and
  15pp is real-but-undetectable and must be reported as such — neither win nor
  null.
- **Parity cannot be *confirmed* at small n.** A met parity threshold on #4/#8 is
  *"no separation detected, underpowered to detect one"* — never *"parity
  established"*.

### Non-independence

Both `impl_many` cells (`Close`, `Write`) are also `callers_call_site_high`
members. D5 must not read #1 and #5 as independent evidence.

### What D5 may not say

If #1 (n=13) or #5 (n=2) trend LCI-positive without clearing their thresholds,
the honest conclusion is **"directionally consistent, underpowered, needs a bigger
or different corpus"** — *not* a beachhead. Reporting a win off n=2 would be
exactly the weak-evidence disease this epic was built to avoid, and **worse than
the 0.96-vs-0.96 null it replaces**.

If the high-noise class is where the beachhead lives, the next lever is **a
corpus with more such symbols** — not more repetitions over the same cells. This
distribution is not D2's error: it is a deterministic draw over the real
candidate population, and the fact that PocketBase does not contain many
high-noise or many-implementor functions **is itself a finding**, surfaced before
a paid sweep rather than after.

## Amendment policy

`predictions.json` **must not be edited after D4 runs.** D5 records deltas
against it. If a family's prompt is later found broken, set `voided: true` with a
`void_reason` in a **new** commit and report the void explicitly. **Never** retune
a prediction, threshold, or confidence after seeing an outcome.

Amendments **before** D4 runs are legitimate and expected — pre-registration is
exactly the moment to fix a design flaw. schema_version 2's correction was made
with **no outcome in existence to tune toward**.
