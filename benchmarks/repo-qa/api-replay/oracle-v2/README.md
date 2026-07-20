# All-tools semantic oracle v2

This directory repairs the scoring design of the completed all-tools transport
pilot. It does **not** revise that pilot's frozen primary result or authorize a
model/encoding selection.

## Evaluation protocol

1. Each of the 28 tool/scenario tasks has a curated, typed claim schema bound to
   the SHA-256 digest of the genuine captured tool output.
2. Schema generation fails unless every task has at least one known-good and
   known-bad answer and the evaluator separates all of them.
3. Deterministic evaluation records every required claim, failed claim, and
   forbidden-pattern match. A miss is provisional and queues the complete
   question, candidate answer, typed claims, and genuine source tool output.
4. Two independently identified judges review every queued miss. Each judge may
   retry up to three times only when its response is structurally invalid; all
   raw attempts remain in the checkpointed file.
5. Only matching, non-ambiguous valid verdicts resolve a case. Disagreement,
   ambiguity, missing judgments, and provider failures remain unresolved.
6. Judge consensus never overwrites a score. Consensus-correct misses become
   proposed regression cases; a maintainer must add a discriminating deterministic
   test, regenerate the bank, and rerun the full analysis.

The historical analysis is exploratory validation against the already collected
448-cell grid. Its output is not confirmatory evidence because the repaired
oracle was developed after those responses were observed.

## Reproduction

```sh
python3 benchmarks/repo-qa/scripts/build_tool_claim_schemas.py \
  --matrix benchmarks/repo-qa/api-replay/all-tools/tool-output-matrix.json \
  --surface benchmarks/repo-qa/comprehension/surface/tool-surface.json \
  --out benchmarks/repo-qa/api-replay/oracle-v2/claim-schemas.json --check

python3 benchmarks/repo-qa/scripts/analyze_all_tools_replay.py \
  --cells benchmarks/repo-qa/api-replay/all-tools/results \
  --matrix benchmarks/repo-qa/api-replay/all-tools/tool-output-matrix.json \
  --claims benchmarks/repo-qa/api-replay/oracle-v2/claim-schemas.json \
  --out benchmarks/repo-qa/api-replay/oracle-v2/historical-analysis.json
```

Run `llm_tool_result_adjudicator.py` twice with different `--judge-id` values
and preferably different model families. Combine them with
`aggregate_tool_adjudications.py`. Judge outputs and consensus are intentionally
not pre-populated here: the instrument must pass review before spending API calls.
