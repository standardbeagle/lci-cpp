# Audit report template

```markdown
# Benchmark evidence audit

Outcome: pass | pass_with_changes | fail | inconclusive
Confidence: high | medium | low
Audit context: fresh context | authoring context

## Decision

State whether the benchmark supports its headline decision and why.

## Hard gates

| Gate | Status | Severity | Artifact evidence |
|---|---|---|---|
| G1 preregistration | pass/fail/inconclusive/n/a | critical/major/minor/none | path, digest, command, or recomputation |

## Recomputed results

Give cell counts, unusable runs, per-stratum effects, within-cell variance, controls, prediction misses, and recommendation rankings.

## Blockers

For each blocker, state the defect, affected claim, likely bias direction, evidence, and required correction.

## Claim map

| Claim | Support | Boundary |
|---|---|---|
| ... | direct/indirect/unsupported | What the evidence does not establish |

## Residual uncertainty

State small samples, malformed cells, unavailable artifacts, model nondeterminism, and generalization limits.
```

Use structured gate data with the scorer:

```json
{
  "schema": "benchmark.audit.v1",
  "gates": [
    {"id": "G1", "hard": true, "status": "pass", "severity": "none", "evidence": ["git show ..."]}
  ]
}
```

The scorer derives only the outcome implied by gate statuses. It does not judge whether evidence is truthful or sufficient.
