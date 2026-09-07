# Response-format A/B — status

**No result yet. The previously published scorecard is withdrawn.**

## Why the earlier scorecard is withdrawn

The table that stood here reported 92-100% hallucination and 50-67% correctness.
Both figures were grader artifacts, not model behavior.

Answers were matched as phrases rather than facts. The first fix credited an
answer that WRAPPED the expected phrase in extra words; it still failed the
opposite case, and the first real strong-tier cell hit exactly that: glm-5.2
answered `30 seconds` where the bank expected `timeout is 30 seconds`, and the
same answer was scored incorrect AND counted as an unsupported claim.

Expected answers are now atoms — a value with its unit, or an identifier — with
any synonyms declared in the bank as `accepted_forms`. A terse answer, a
re-expressed one, and a wrapped one all score correct; a changed value or a
changed unit does not. The bank validator rejects an expected answer absent from
the rendered facts, so the key cannot drift back into prose.

The run also named `opencode/deepseek-v4-flash-free` as the weak model. That id
is not in `opencode models`, so the weak rows could not have come from the model
the manifest claims.

Because `grading_schema` is part of the cell identity, every record behind the
old table is invalid by construction and the grid must be rerun under
`atomic-value-v3`.

## Smoke evidence (2026-09-07)

Four real cells: both arms of the `file-lines` task on both models, in the
tool-denied corpus-free workspace. Records under
`.work/response-shape-smoke/` (gitignored). The smoke ran with `timeout_seconds`
lowered to 180 for the session, so its records carry a different `manifest_digest`
than the committed manifest (600) and will not be reused by a real grid run; that
is the frozen-timeout rule working as designed, not a ledger defect.

| Model | Arm | Status | Correct | Answer recall | Evidence recall | Hallucinated | Wall (s) |
|---|---|---|---|---|---|---|---|
| `opencode-go/deepseek-v4-flash` | `shape_17` | answered | yes | 1.0 | 1.0 | no | 11.4 |
| `opencode-go/deepseek-v4-flash` | `shape_42` | answered | yes | 1.0 | 1.0 | no | 92.7 |
| `opencode-go/glm-5.2` | `shape_17` | answered | yes | 1.0 | 1.0 | no | 11.6 |
| `opencode-go/glm-5.2` | `shape_42` | answered | yes | 1.0 | 1.0 | no | 11.9 |

Three of the four answered `30 seconds` — the exact terse form the withdrawn
grader called both an omission and a hallucination. All four now grade correct
with no unsupported claim.

Four cells decide nothing about the treatment. They prove the harness runs
end-to-end against both live models and scores the result.

The strong model is reachable: the earlier report that `opencode-go/glm-5.2`
was down was a transient outage, and a run 20 minutes before this one timed out
on all four cells including the weak model that had answered the same day.
Do not name a provider outage a blocker without reproducing it across a gap.

## Analysis

The analyzer pairs each task under both renderings within the same repetition
and applies `manifest.practical_rule` mechanically, per model and per capability
tier, never pooled. A spread of exactly zero, or a baseline pinned at floor or
ceiling, reports `unmeasured` rather than crediting a free lift.

## Known limits

- **Hallucination is an upper bound.** A claim counts as supported only when a
  frozen fact is contained in it, so a supported fact re-expressed as different
  prose still counts against the arm. Every report prints this caveat, and the
  metric must not be read as a rate. It is the one metric whose non-inferiority
  bound the design doc makes decision-relevant.
- No grid has been run under the corrected schema. Do not change any production
  response shape on the strength of anything in this file.
