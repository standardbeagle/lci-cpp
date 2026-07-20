# Complex-response evaluator meta-test v1

The evaluator is **not yet reliable as a two-judge gate**. DeepSeek passed all
preregistered thresholds; GLM failed the accuracy, completeness, and repetition
stability gates. Neither judge falsely accepted a response it labeled `correct`,
but GLM used `ambiguous` for several responses containing claims it explicitly
identified as unsupported.

| Judge | Exact | Accuracy | False accepts | False rejects | Unresolved | Repeat disagreements |
|---|---:|---:|---:|---:|---:|---:|
| DeepSeek | 29/32 | 90.625% | 0 | 3 | 0 | 1 |
| GLM | 26/32 | 81.25% | 0 | 0 | 6 | 3 |

The run found two instrument defects. First, the prompt did not clearly say that
the candidate was an intermediate post-tool continuation, causing grounded
responses that requested necessary additional evidence to be penalized for not
finishing the whole objective. Second, the structural validator allowed an
`ambiguous` verdict alongside definite `unsupported_claims`, even though the
rubric says invented claims must be rejected.

These findings are useful calibration failures. The raw attempts are preserved.
Prompt and validation changes motivated by these results are post-hoc; a rerun on
the same fixtures can demonstrate regression behavior only and cannot replace this
result. A fresh held-out fixture bank is required for confirmatory validation of
the repaired evaluator.
