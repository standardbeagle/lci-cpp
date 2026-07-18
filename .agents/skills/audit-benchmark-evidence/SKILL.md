---
name: audit-benchmark-evidence
description: Audit benchmark validity, reproducibility, and evidence claims from preregistration through reporting. Use when reviewing an A/B test, model evaluation, benchmark harness, scoring rubric, experiment report, raw result grid, control behavior, statistical analysis, or recommendation derived from benchmark data.
---

# Audit Benchmark Evidence

Audit from raw artifacts. Do not begin with the author's conclusion, suspected defect, or desired decision.

## Workflow

1. Read [rubric.md](references/rubric.md) completely.
2. Inventory the preregistration, immutable revision, fixtures, prompts, oracle, raw results, analysis code, generated outputs, and report. Mark missing artifacts.
3. Reconstruct the timeline. Verify that predictions, metrics, exclusions, stopping rules, and analysis rules predate outcome collection.
4. Compare arms fact by fact. Identify every difference beyond the declared treatment and its likely bias direction.
5. Inspect model-visible inputs for leading language, answer leakage, oracle paths, solution identifiers, favorable arm labels, and unequal affordances.
6. Verify oracle independence, good-case acceptance, wrong-case rejection, and fail-closed behavior.
7. Recompute logical-cell uniqueness, grid completeness, status classifications, exclusions, per-stratum metrics, within-cell variance, effect decisions, prediction misses, controls, and recommendations.
8. Regenerate machine and human reports byte-for-byte when deterministic output is claimed.
9. Score every rubric gate as `pass`, `fail`, `inconclusive`, or `not_applicable`, with artifact evidence.
10. Derive the overall outcome using the rubric. Do not average away a hard validity failure.
11. Write the audit using [report-template.md](references/report-template.md). Validate structured gate data with `scripts/score_audit.py`.

## Independence and context

Seek evidence independence, not reviewer personification. A fresh context or subagent can reduce contamination, so recommend one for complex or consequential audits when available. Treat it only as context management. Agreement, confidence, identity, or reviewer count never substitutes for artifact evidence or recomputation.

If operating in the authoring context, explicitly disclose that fact. Withhold conclusions from any fresh-context pass and provide raw artifacts rather than suspected bugs or intended answers.

## Fail closed

Return `inconclusive` when required evidence is unavailable. Return `fail` when a defect can change the measured outcome or headline claim. Never infer a pass from missing data, green tests alone, a polished report, or another reviewer's approval.

## Required output

Lead with the outcome and the highest-severity evidence. Include:

- Overall outcome and confidence
- Hard-gate results
- Recomputed effect and control results
- Blockers and their likely bias direction
- Claim-by-claim support level
- Required corrections
- Residual uncertainty and context-isolation disclosure
