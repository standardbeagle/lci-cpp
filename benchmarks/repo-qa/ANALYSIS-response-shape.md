# Response-format A/B — status

**No result yet. The previously published scorecard is withdrawn.**

## Why the earlier scorecard is withdrawn

The table that stood here reported 92-100% hallucination and 50-67% correctness.
Both figures were grader artifacts, not model behavior.

Answers were matched by exact string equality after case/whitespace
normalization. A real recorded cell answered `The configured timeout is 30
seconds` where the bank expected `timeout is 30 seconds`; the extra words made
the strings unequal, so the same answer was scored incorrect AND counted as an
unsupported claim. Every correctly-answering cell that added any context was
penalized twice.

The run also named `opencode/deepseek-v4-flash-free` as the weak model. That id
is not in `opencode models`, so the weak rows could not have come from the model
the manifest claims.

Both defects are fixed (`atomic-evidence-v2-containment`, verified model ids).
Because `grading_schema` is part of the cell identity, every record behind the
old table is invalid by construction and the grid must be rerun.

## Smoke evidence (2026-09-06)

Two real cells, one per arm, `file-lines` task, `opencode-go/deepseek-v4-flash`,
in the tool-denied corpus-free workspace. Streams committed under
`response-shape/recordings/`.

| Arm | Status | Correct | Evidence recall | Hallucinated |
|---|---|---|---|---|
| `shape_17` (compact) | answered | yes | 1.0 | no |
| `shape_42` (labeled) | answered | yes | 1.0 | yes (see below) |

Two cells decide nothing about the treatment. They prove the harness runs
end-to-end against a live provider and scores the result.

## Blockers and known limits

- **The strong model is unreachable.** `opencode-go/glm-5.2` is listed by
  `opencode models` but three independent attempts hung to timeout with empty
  stdout and empty stderr, including a bare `opencode run` outside the harness.
  This is provider-side. The grid cannot run its strong tier until it clears.
- **Restructured prose still reads as a hallucination.** Containment credits a
  fact wrapped in extra words; it does not credit the same fact re-expressed.
  The `shape_42` claim `The timeout source is config/runtime.toml at line 12`
  is fully supported by the bank's facts yet was flagged. Loosening further
  (token overlap) risks a blanket accept, so the hallucination metric is
  currently an UPPER bound and must not be read as a rate until this is
  resolved. It is the one metric whose non-inferiority bound the design doc
  makes decision-relevant.
- No grid has been run under the corrected schema. Do not change any production
  response shape on the strength of anything in this file.
