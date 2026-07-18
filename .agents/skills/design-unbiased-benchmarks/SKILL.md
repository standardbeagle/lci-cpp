---
name: design-unbiased-benchmarks
description: Design preregistered, neutral, reproducible software and model benchmarks. Use when creating or changing an A/B evaluation, benchmark harness, scoring oracle, prompt-based comparison, model/tool-output study, experiment manifest, control, or analysis plan before collecting results.
---

# Design Unbiased Benchmarks

Design the experiment before observing treatment outcomes. Optimize for falsifiability, not for confirming the proposed change.

## Workflow

1. Read [methodology.md](references/methodology.md) completely.
2. State the decision the benchmark will inform and the smallest claim it can support.
3. Define the experimental unit, population, arms, models or systems, metrics, controls, repetitions, exclusions, and stopping rule.
4. Build equivalent arms. Change only the treatment under test. Record every unavoidable difference.
5. Define an independent oracle and prove that it accepts a known-good case and rejects a plausible wrong case.
6. Make prompts neutral. Do not name the expected winner, expose oracle material, or describe one arm with favorable language.
7. Preregister predictions, analysis rules, variance estimator, practical-effect threshold, and control response before running the grid.
8. Write the manifest using [manifest-schema.md](references/manifest-schema.md). Validate it with `scripts/validate_manifest.py`.
9. Commit or otherwise timestamp the manifest before collecting results. Record the immutable revision.
10. Run all planned cells. Preserve raw outputs, failures, provenance, and exclusions. Never silently replace failed cells.
11. Analyze according to the preregistration. Label every post-hoc analysis as exploratory.
12. Report nulls, prediction misses, control behavior, uncertainty, practical importance, and claim boundaries with equal prominence.

## Stop conditions

Do not run the benchmark when any condition holds:

- Treatment arms differ in answer content, answer wording, available facts, or task difficulty beyond the declared treatment.
- The prompt or fixture reveals the expected answer, preferred arm, oracle path, or benchmark purpose in a way that can steer behavior.
- The oracle shares the implementation or failure mode it evaluates and lacks a discrimination test.
- A control cannot plausibly produce the expected null or positive signal.
- Provider failures, timeouts, malformed outputs, or missing cells would be scored as wrong answers.
- The analysis can choose metrics, thresholds, exclusions, pooling, or stopping rules after seeing outcomes.

Fix the design or narrow the claim. Do not waive a validity defect because the experiment is cheap.

## Context isolation

Recommend a fresh-context pass when the benchmark is complex or consequential. Treat subagents only as optional context-management tools. Their agreement is not evidence. Independence comes from raw artifacts, withheld conclusions, recomputation, and explicit procedures.

## Deliverables

Produce:

- A validated preregistration manifest
- Frozen fixtures and prompt templates
- Independent oracle and discrimination tests
- Raw per-cell results with provenance
- Machine-readable analysis
- Human report separating confirmatory and exploratory findings

Before interpreting results, use `$audit-benchmark-evidence` against the raw artifacts.
