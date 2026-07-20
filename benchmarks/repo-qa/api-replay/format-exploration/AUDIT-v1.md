# Benchmark evidence audit: format exploration v1

Outcome: inconclusive  
Confidence: high  
Audit context: authoring context

## Decision

V1 supports no comparison among JSON, readable XML, path records, or tagged blocks.
All 64 scheduled calls ended with HTTP 403 before a model completion. The treatment
isolation and scoring machinery remain mechanically valid, but there is no usable
effect or control evidence.

The failure is attributable to the direct transport omitting captured non-secret
OpenCode client/request headers. It does not favor an arm: every arm and both model
strata failed identically. It prevents a decision rather than supplying evidence of
parity or harm.

## Hard gates

| Gate | Status | Severity | Artifact evidence |
|---|---|---|---|
| G1 preregistration | pass | none | `5aa1d8f` predates attempts; `f8f0f95` unlocked execution |
| G2 treatment isolation | pass | none | `preflight.json`; exact one-pointer diffs |
| G3 neutrality | pass | none | opaque arms; frozen Latin-square schedule |
| G4 leakage | pass | none | stateless direct continuations; oracle absent |
| G5 oracle | pass | none | good/wrong/omission/invention tests |
| G6 grid/provenance | inconclusive | major | 64/64 attempts preserved, 0 usable |
| G7 failure handling | pass | none | 403s remain unscored provider failures |
| G8 controls | inconclusive | major | manipulation passes; cycle null unavailable |
| G9 repetition/effect | inconclusive | major | no answered repetitions |
| G10 stratification | inconclusive | major | both strata are 0/32 usable |
| G11 reproducibility | pass | none | deterministic analysis regeneration |
| G12 claim scope | pass | none | no format or production claim made |

## Recomputed results

- Planned logical cells: 64
- Immutable first attempts: 64
- HTTP status: 403 in 64/64
- Answered/quality-scored cells: 0
- Retries: 0, consistent with the frozen non-retryable classification
- Manipulation control: passed
- Cycle-null control: unavailable
- Advanced formats: none; this is an incomplete decision, not evidence favoring JSON

## Required correction

Preserve v1 unchanged. Register a new v2 study whose fixtures and transport include
an explicit allowlist of the safe headers recorded from the genuine OpenCode calls.
Revalidate exact body isolation, freeze new fixture/adapter digests, and collect a new
grid. Do not relabel v2 attempts as v1 retries.

## Residual uncertainty

No model behavior was observed, so the direction and size of every format effect are
unknown. This audit was performed in the authoring context; its conclusions rely on
recomputation from the published raw attempts rather than reviewer independence.
