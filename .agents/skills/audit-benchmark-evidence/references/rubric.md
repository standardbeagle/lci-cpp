# Benchmark evidence audit rubric

## Outcomes

- `pass`: Every applicable hard gate passes. Results reproduce. Claims stay within the evidence.
- `pass_with_changes`: Every hard gate passes, but bounded reporting, provenance, or low-risk reproducibility defects remain. Corrections cannot plausibly change headline conclusions.
- `fail`: A hard gate fails, or another defect can plausibly change a measured outcome, winner, or decision.
- `inconclusive`: The design may be valid, but missing or unusable evidence prevents a decision.

Do not compute an average score. One failed hard gate overrides many passing checks.

## Status and severity

Score each gate `pass`, `fail`, `inconclusive`, or `not_applicable`.

- `critical`: Contamination, fabrication, leakage, or loss of raw evidence invalidates the experiment.
- `major`: The defect can change an effect, winner, control interpretation, or recommendation.
- `minor`: The defect affects clarity, traceability, or bounded reproducibility but not the supported conclusion.

## Hard validity gates

### G1. Preregistration and temporal integrity

Require immutable evidence that predictions, primary metrics, thresholds, repetitions, exclusions, stopping rules, stratification, and analysis rules predate result collection. Label post-hoc work exploratory. Fail if choices were outcome-dependent or if temporal order cannot be established for a confirmatory claim.

### G2. Treatment isolation and arm equivalence

Require the same task, facts, answer wording, context, tools, time, and grading except for the declared treatment. For format tests, compare derivable-equivalent information. Classify extra facts or pre-resolved ambiguity as idealized capability changes. Fail when an undeclared difference can explain the effect.

### G3. Prompt neutrality and blinding

Require arm-neutral instructions and identifiers. Reject favorable adjectives, expected-winner hints, asymmetric examples, unequal tool permissions, or order that is neither randomized nor justified. Fail when steering can plausibly affect behavior.

### G4. Leakage and contamination

Require no oracle answer, solution identifier, corpus access, hidden path, patch content, grader token, or cross-cell state in model-visible context. Verify isolated workspaces and tool restrictions when relevant. Any plausible answer leakage is critical unless bounded evidence proves it unreachable.

### G5. Oracle independence and discrimination

Require a truth source independent of the system under test and its matching mechanism. Require a known-good pass and a plausible wrong or broken case that fails. Require explicit reason codes for missing, ambiguous, or failed oracle execution. Fail when the oracle can share the tested bug or has never proved discrimination.

### G6. Grid integrity and provenance

Require the complete preregistered grid, unique logical cells, immutable input digests, raw output, system/model version, repetition, status, score, and failure reason. Reject stale resume data and silent cell replacement. Missing evidence yields `inconclusive`; duplicate or biased replacement yields `fail`.

### G7. Failure and exclusion handling

Require timeouts, quota errors, malformed streams, empty provider output, harness errors, and missing cells to remain distinct from genuine wrong answers. Apply frozen exclusions consistently and report all unusable cells. Fail when infrastructure failures become zero scores or retries depend on observed quality.

### G8. Controls and instrument validity

Require preregistered null controls and, when feasible, positive controls. Investigate surprising controls before interpreting treatments. Check that control arms are equivalent. Fail when an unexplained control effect undermines the instrument or when an invalid control supplies production evidence.

### G9. Repetition, variance, and effect rule

Require repeated measurements and per-arm sample sizes. Recompute variance within comparable cells or arms; exclude between-treatment effects from the noise estimator. Apply frozen practical and statistical thresholds. Fail when single runs decide winners, effects inflate their own threshold, or noise-level gaps become claims.

### G10. Stratification and aggregation

Require results by preregistered capability tier, workload, language, or other effect modifier. Reject Simpson's-paradox-prone pooling and aggregation that hides failures. Verify weighting. Fail when pooling creates the headline effect or erases a material stratum.

### G11. Reproducibility and determinism

Require analysis regeneration from raw artifacts. Compare machine outputs and reports byte-for-byte when determinism is claimed. Record environment and revisions. Missing convenience documentation is minor; inability to reproduce headline values is major.

### G12. Claim scope and decision validity

Map each claim to direct evidence. Separate comprehension from live production, idealized capability from deployable formatting, correlation from causation, statistical from practical significance, and tested population from general use. Fail when a recommendation depends on an unsupported boundary crossing.

## Common leading-evaluation patterns

Flag these even when tests pass:

- Calling one arm improved, enriched, clean, or baseline in prompts
- Giving the treatment arm exact oracle wording while the control paraphrases it
- Choosing tasks known to showcase the proposed feature
- Showing graders the hypothesis or arm identity
- Tuning prompts, exclusions, thresholds, or retries after observing failures
- Reporting only the strongest metric, model, task, or repetition
- Treating malformed provider output as model incompetence
- Pooling capability tiers after predicting different sensitivity
- Declaring parity because treatment effects inflate pooled variance
- Promoting idealized fixtures as production-ready format changes
- Hiding nulls, misses, invalid controls, or negative results below wins

## Recommendation rule

Recommend production change only when:

1. Every relevant hard gate passes.
2. The effect exceeds the preregistered noise and practical thresholds.
3. The tested arm is production-faithful, or capability work is named explicitly.
4. The affected population and frequency justify the cost.
5. Residual uncertainty and the next live validation are stated.
