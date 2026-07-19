# Hardcoded tool-response-shape A/B design

## Decision and claim

This experiment decides whether to prototype a labeled response envelope for LCI tools. It tests whether models produce better downstream answers when the same hardcoded tool facts use a stable labeled shape instead of the current compact response shape.

The experiment can support only this claim: on the frozen canned tasks and tested models, one response shape improves downstream use of supplied evidence. It cannot show that production emits the proposed shape correctly, that agents select tools differently, or that live repository work improves. Do not change a production response until the complete preregistered grid passes its controls and practical-effect rule.

## Treatment isolation

Use opaque arm IDs in model-visible and raw execution data. Reveal their mapping only in the analysis metadata.

- Arm A contains the current compact or raw hardcoded response.
- Arm B contains the same facts under four stable labels: `status`, `key_result`, `evidence`, and `next_action`.
- Hold prompts, model IDs and versions, temperature and provider settings, tool names, descriptions, input schemas, output schemas, selected tool, task order policy, time budget, underlying facts, expected answers, and grader constant.
- Disable all tools and outside repository access during answer generation. The supplied canned response is the only evidence.
- Match facts machine-checkably. Store a canonical fact list beside each pair and require both arm renderings to round-trip to that list with an independent fixture parser.
- Keep the arm texts approximately token-matched. Preregister a per-pair limit of the larger of 10 tokens or 10% of Arm A's token count. Padding may use neutral syntax only; it must not add instructions, summaries, certainty, salience, or facts.
- Record punctuation, ordering, and unavoidable token differences. If Arm B resolves ambiguity, adds a conclusion, recommends a specific action absent from Arm A, or cannot meet equivalence, exclude the pair before execution rather than label it a format result.

`next_action` must restate an action already present in the canonical facts. Use an empty value when no action exists. It must not tell the model how to answer.

## Bank and experimental grid

Freeze 6–10 deterministic tasks drawn by a declared rule from the existing comprehension surface. Cover direct lookup, multi-item extraction, negative evidence, file-and-line evidence, and a no-op/null case. Do not select tasks after comparing arm outputs. Each task provides one question, one selected tool, one canonical fact list, two hardcoded renderings, and an independent expected answer.

The experimental unit is `(task, arm, model, repetition)`. Use the same two full provider/model IDs and capability tiers as `comprehension_ab.py` for the first quick grid unless the preregistration names replacements. Counterbalance arm order within each task/model/repetition and run at least two repetitions. Freeze the model list, repetition count, timeout, task bank, arm mapping, tokenizer, thresholds, and analysis revision before the first provider call. Run every planned cell; do not stop when a preferred result appears.

Include two controls:

- A null task whose answer is already explicit and whose two shapes should score equally.
- A positive discrimination fixture in unit tests where a known wrong answer omits evidence or invents a fact; the oracle must reject it while accepting the known-good answer.

A surprising null-control effect or failed discrimination test invalidates interpretation until corrected and rerun from a new preregistration revision.

## Metrics and decision rule

Score model answers against frozen, author-only oracles that share no response renderer.

- Primary: downstream correctness, reported as exact completion rate and atomic-answer precision/recall.
- Secondary: evidence use, the recall and precision of required file/line or fact identifiers.
- Secondary: hallucination rate, the fraction of answers containing any unsupported atomic claim and the unsupported-claim count.
- Secondary: completion rate, the fraction of planned cells that return a parseable, gradeable final answer.
- Diagnostic only: input/output tokens, latency, malformed answers, and provider failures. Tool selection is fixed and is not an outcome.

Analyze paired task-level Arm B minus Arm A effects per model and capability tier. Report cell counts, paired means, dispersion, unusable cells, and each task's result; do not pool model tiers for the decision. Before execution, set a practical threshold for correctness improvement and a non-inferiority bound for hallucination. Adopt neither shape when the effect is within repeated-run noise, controls fail, completion drops beyond its bound, hallucination exceeds its bound, or results disagree materially by task without a prespecified explanation. Report null and adverse results with the same prominence as gains.

## Architecture and data flow

Follow the existing canned-output harness rather than changing the live MCP server:

1. A frozen manifest names models, repetitions, thresholds, opaque arm mapping, fixture digests, tokenizer, and analysis revision.
2. A fixture loader validates each task and canonical fact list, verifies both renderings, and rejects fact or token-budget drift.
3. A fake MCP/provider adapter returns the selected hardcoded arm response. Unit tests never use credentials, corpora, or a live `lci` process.
4. The runner creates the same empty Git workspace used by `comprehension_ab.py`, disables tools and permissions, builds an arm-neutral prompt, and executes each cell.
5. The recorder writes one immutable JSON object per logical cell with identity, input and prompt digests, raw response, raw model answer, provenance, status, timing, token counts, score, and failure reason.
6. The analyzer reads only complete records, checks the frozen grid, reveals the arm mapping, computes paired metrics, and emits machine-readable analysis plus a short report.

The fake adapter proves harness behavior; an optional guarded provider run measures model behavior. `tooleval.py` remains a direct MCP conformance check and does not grade this treatment.

## Failure and resume isolation

Derive the resume key from task, opaque arm, full model ID plus collision-resistant digest, repetition, manifest digest, prompt digest, fixture digest, grading-schema version, and analysis revision. Write results atomically. Reject corrupt records and duplicate logical cells. Reuse a record only when every identity field matches and its answer and score are complete.

Classify timeouts, quota errors, provider errors, malformed streams, empty answers, malformed final answers, harness errors, and missing cells separately. Infrastructure failures remain unscored; never convert them to incorrect answers. Retry only statuses declared retryable before execution, preserve every attempt, and never overwrite an answered cell with a later failure. Use a fresh empty workspace and stateless fake session per cell so one arm cannot leak files, messages, caches, or tool state into another.

## Test plan

Hermetic tests must prove:

- the bank contains 6–10 tasks, both opaque arms, required task strata, and the null control;
- prompts, model settings, tool metadata, selected tool, canonical facts, and expected answers are identical across a pair;
- independent parsers recover the same canonical facts from both renderings;
- token differences meet the frozen tolerance;
- the fake MCP/provider returns the requested hardcoded response and cannot access a real corpus or credentials;
- the oracle accepts a known-good answer and rejects omissions, unsupported claims, wrong evidence, and malformed output;
- correctness, evidence, hallucination, and completion metrics match hand-calculated fixtures;
- arm order is deterministic and counterbalanced;
- cell keys resist model-ID collisions, atomic writes survive resume, stale digests are rejected, and duplicate cells fail closed;
- timeout, quota, provider, malformed-stream, empty, and malformed-answer outcomes remain unscored;
- a completed answer survives a later retry failure;
- dry-run prints the exact planned grid without calling a provider.

Run the focused unit tests first, then the full `benchmarks/repo-qa/tests` suite. Run the provider grid only after the manifest, fixtures, tests, and design are committed. Audit raw artifacts before interpreting results.

## Likely implementation files

- `benchmarks/repo-qa/scripts/response_shape_ab.py` — grid runner, isolated execution, resume, and records
- `benchmarks/repo-qa/scripts/analyze_response_shape_ab.py` — frozen paired analysis and report data
- `benchmarks/repo-qa/response-shape/manifest.json` — preregistered models, repetitions, thresholds, controls, and arm mapping
- `benchmarks/repo-qa/response-shape/tasks.json` — canonical facts, hardcoded renderings, expected answers, and strata
- `benchmarks/repo-qa/tests/test_response_shape_ab.py` — deterministic fake-provider/MCP, oracle, metric, failure, and resume tests
- `benchmarks/repo-qa/tests/test_response_shape_analysis.py` — grid completeness and paired-analysis tests
- `benchmarks/repo-qa/ANALYSIS-response-shape.md` — generated human report after the run

Reuse small, treatment-neutral helpers from `benchmarks/repo-qa/scripts/comprehension_ab.py` where practical. Do not modify production response emitters or `benchmarks/repo-qa/scripts/tooleval.py` for this experiment.
