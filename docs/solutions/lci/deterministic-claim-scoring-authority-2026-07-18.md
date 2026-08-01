---
written_at: 2026-07-18T19:58:00Z
source_event: task:01KXMSV16JWDMEYEHK7E8YMWQH
module: lci
category: best-practices
confidence: high
form: procedure
sources:
  - task:01KXMSV16JWDMEYEHK7E8YMWQH#acceptance-criteria
  - git:e7b9d11
  - git:2b9c1ce
  - git:3b454bb
  - git:1330bca
tags: [deterministic-scoring, evidence, retries, compatibility, provenance]
status: steering
recurrence: 1
---

# Keep scoring truth, retry history, and compatibility orthogonal

## Lesson

A deterministic claim scorer needs three separate authority rules: a result succeeds only when its verdict is correct **and** its evidence satisfies the declared grounding policy; the latest completed answer controls metrics while every retry remains in forensic history; and experimental run settings remain distinct from post-hoc scoring settings during compatibility checks.

## What didn't work

Early revisions treated evidence and retry selection too loosely. A correct guess could appear successful without authoritative support, a later timeout or provider error could erase an earlier completed answer, and an explicit scoring-policy override could replace recorded run settings and make experimentally different arms appear compatible. Review also exposed ambiguous coordinates where one citation overlapped both live and forbidden anchors.

## Why it recurs

Scoring pipelines often collapse different concerns into one convenient field or latest-record rule. That conflates semantic correctness with evidentiary support, metric authority with append-log order, and collection provenance with analysis policy. The resulting summaries can look deterministic while silently changing the experiment being measured.

## Apply when

Scoring structured benchmark answers from append-only run logs, especially when answers cite adjudicated source coordinates, runs can be retried, or analysts can override scoring thresholds after collection.

## Prevention

1. Define success conjunctively: schema-valid answer, correct verdict, and grounding thresholds must all pass. Report verdict correctness and grounding separately so correct-but-unsupported guesses remain observable but score zero.
2. Match citations against every anchor at a coordinate. If a citation overlaps both an authoritative-live anchor and any dead, misleading, wrong-layer, or otherwise forbidden anchor, fail it closed rather than allowing the live match to launder the ambiguity.
3. Give completed answers authority over later infrastructure failures for metrics. Among completed retries, let the latest completed answer win. Preserve every attempt, in arrival order, in a separate forensic history.
4. Store `run_settings` and `scoring_settings` independently. Compatibility must compare both: a post-hoc policy override may change analysis, but must never hide different model/runtime conditions between arms.
5. Use an explicit test matrix rather than a single happy path: every verdict; correct verdict with good, absent, stale, duplicate, dead, and overlapping evidence; each forbidden anchor class; answered-to-answered and answered-to-failure retries; incomplete pairs; and one incompatibility test per provenance dimension.
