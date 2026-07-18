# Benchmark methodology

## 1. Decision and claim

Write the decision first. Name who will act, what could change, and the practical threshold that would justify change. State the narrowest supported claim. A canned comprehension test can show that a model can read a representation; it cannot show that production emits it correctly or that agents complete live tasks more often.

List plausible outcomes that would reverse, preserve, or defer the decision. A benchmark designed so every outcome supports the proposed change is advocacy, not evaluation.

## 2. Hypotheses and preregistration

Record before result collection:

- Primary hypothesis and directional prediction
- Per-stratum predictions when effects may differ by capability, workload, language, or environment
- Primary and secondary metrics
- Practical-effect threshold and statistical/noise rule
- Repetition count and stopping rule
- Inclusion, exclusion, retry, and missing-data rules
- Planned aggregation and prohibited pooling
- Expected control behavior
- Analysis code revision and fixture digests

Commit or timestamp the preregistration separately from results. Preserve prediction misses as first-class findings. Mark all analyses invented after observation as exploratory.

## 3. Experimental units and sampling

Define the unit receiving a treatment: prompt, task, repository, request, session, or participant. Randomize or counterbalance order when history, cache, fatigue, or provider load can matter. Prevent one unit from leaking state into another.

Select tasks by a rule independent of expected treatment response. Include representative difficulty and known failure modes. Do not select only examples on which a proposed format looks useful. Record the target population and limits to generalization.

## 4. Treatment-arm equivalence

Change one causal factor. Hold task, facts, answer wording, prompt, tools, time budget, model, temperature, context, and grading constant unless one is the declared treatment.

For format tests, compare derivable-equivalent information. Annotation may surface facts the production system already resolves. If it adds facts or pre-resolves ambiguity that production cannot produce, label the arm `idealized`; treat the result as capability research, not a formatting win.

Create a machine-checkable equivalence assertion where possible. Record unavoidable differences and explain how each could bias the result.

## 5. Neutral prompts and blinding

Use arm-neutral labels and identical instructions. Avoid words such as improved, baseline, richer, clearer, compact, or noisy. Do not mention the expected winner or hypothesis. Do not place oracle answers, grading tokens, solution paths, patch content, or hidden identifiers in model-visible context.

Shuffle arm order and opaque identifiers when order can lead. Keep graders blind to arm identity when judgment is subjective. If automated grading is used, freeze it before results.

## 6. Independent oracles

Use a source of truth independent of the system under test. Avoid shared parsers, regexes, helpers, indexes, or transformations that can reproduce the same bug.

Prove discrimination:

1. The oracle accepts a known-good answer.
2. The oracle rejects an untransformed, broken, or plausibly wrong answer.
3. Ordinary tool failures, missing inputs, and ambiguous matches fail closed with reason codes.

For extraction tasks, score precision, recall, and false positives. Exact-match alone can hide hallucinated additions; recall alone can reward indiscriminate output.

## 7. Controls

Include controls selected before observation:

- Null control: treatment should not matter.
- Positive control: a known distinction should be detected when practical.
- Leakage or contamination check when models can access external context.

Investigate a surprising control before interpreting treatment effects. A large null-control effect can reveal non-equivalent wording, grader sensitivity, order effects, or contamination. Exclude invalid controls from production recommendations and report the reason.

## 8. Execution and failure handling

Run the complete declared grid. Persist one immutable result per logical cell with task, arm, stratum, repetition, input digest, model/system version, timestamps, status, raw output, score, and failure reason.

Reject duplicate logical cells and stale resume data. Classify quota errors, timeouts, malformed output, empty provider streams, harness errors, and missing cells as unscored failures unless the preregistration says otherwise. Never convert infrastructure failure into a zero-quality answer.

Keep tools disabled when testing comprehension of supplied evidence. Use isolated workspaces to prevent corpus lookup or solution leakage.

## 9. Variance and analysis

Use repeated runs. Estimate noise within comparable cells or treatment arms; do not let the treatment effect inflate its own parity threshold. Report per-arm sample size, mean, dispersion, effect size, and unusable runs.

Apply the preregistered practical-effect rule. Treat differences within noise as parity. Stratify by the dimensions named in advance; do not pool weak and strong systems, distinct workloads, or incompatible tasks merely to produce a headline.

Rank recommendations by expected decision value, such as measured gain multiplied by production frequency, cost, or affected users. Keep idealized arms separate from deployable changes.

## 10. Reporting

Publish raw data, machine-readable analysis, methodology, and a concise report. Give nulls, misses, exclusions, malformed cells, invalid controls, and variance caveats the same visibility as wins.

For every finding, state:

- What changed and by how much
- Uncertainty and repetitions
- Whether the arm was production-faithful
- What population and strata were tested
- What the result establishes
- What it does not establish
- The next decision or experiment
