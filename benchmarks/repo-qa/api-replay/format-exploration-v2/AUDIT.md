# Benchmark evidence audit: format exploration v2

Outcome: fail  
Confidence: high  
Audit context: authoring context

## Decision

The primary format-effect evidence fails the oracle and control gates. The frozen
extractor marks a plainly correct `path at line N` answer wrong, creating the only
apparent DeepSeek difference and the failed cycle-null control. Exact-rate claims
based on that difference are unsupported.

The narrower decision not to advance a candidate is robust: the frozen analysis
advances none, and the post-hoc semantic sensitivity makes every arm 8/8 while still
advancing none. This is not positive evidence for JSON; it is failure to meet the
predeclared advancement bar.

## Hard gates

| Gate | Status | Severity | Evidence |
|---|---|---|---|
| G1 preregistration | pass | none | revisions `824690c`, `57d93f4`, `9789ea7` |
| G2 isolation | pass | none | exact one-pointer preflight |
| G3 neutrality | pass | none | opaque arms, Latin square |
| G4 leakage | pass | none | stateless direct requests |
| G5 oracle | fail | major | correct `path at line N` answer rejected |
| G6 grid | pass | none | 64/64 unique answered cells |
| G7 failures | pass | none | zero failures/retries/exclusions |
| G8 controls | fail | major | DeepSeek frozen cycle-null −0.25 |
| G9 repetition/effect | pass | none | eight blocks; McNemar/Holm applied |
| G10 stratification | pass | none | per-model, no pooling |
| G11 reproducibility | pass | none | raw-to-analysis regeneration |
| G12 scope | pass | none | no advancement or production claim |

## Recomputed result

Frozen: GLM 8/8 on all four arms; DeepSeek JSON 7/8 and every candidate 8/8.
All raw and Holm-adjusted p-values are 1.0. No candidate advances. Sensitivity:
DeepSeek JSON becomes 8/8, all six deltas become zero, both cycle controls pass, and
no candidate advances.

## Required correction

Before a broader tool study, freeze an oracle that recognizes equivalent file/line
phrasings and add discrimination cases for `path:line`, `path at line N`, omissions,
definition/callsite confusion, and extra locations. Do not retroactively replace the
v2 primary scores.

## Residual uncertainty

Only one search task and two models were tested. Format effects on other LCI tools,
larger outputs, errors, and multi-step agents remain unknown. The audit occurred in
the authoring context and relies on published raw artifacts and recomputation.
