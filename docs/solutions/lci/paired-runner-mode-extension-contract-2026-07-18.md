---
written_at: 2026-07-18T19:35:00Z
source_event: task:01KXMSV10XZ39YYJ6M4H9KSA9V
module: lci
category: best-practices
confidence: high
form: procedure
sources:
  - task:01KXMSV10XZ39YYJ6M4H9KSA9V#acceptance-criteria
  - workflow:01KXVAM6882DE3JCY0BYQN623M#attempt-01KXVB3T8M8WPC0HAQ74FS3G60
  - git:1dd76d2
  - git:a671473
tags: [paired-runner, experimental-parity, isolation, resume]
status: steering
recurrence: 1
---

# Extending a paired runner without adding experimental variables

## Lesson

A new benchmark mode needs four independent contracts: whole-request parity, mode compatibility, sealed-data isolation, and terminal capture of malformed answers.

## What didn't work

The first claim-validation implementation kept arm-specific tool instructions. Although the model, prompt, timeout, checkout, and output schema matched, those instructions introduced a second treatment variable. Independent review rejected the run as confounded.

## Why it recurs

Paired runners often assemble requests through arm-specific helpers. Reusing those helpers in a stricter mode silently carries historical differences into the experiment. Output parsing, record identity, and path enforcement can likewise become coupled when a mode is added in place.

## Apply when

Adding a mode, schema, or treatment to a reusable A/B runner while retaining older run formats and resumable records.

## Prevention

1. Construct both requests, compare every serialized field, and exclude only the preregistered treatment field (here, `allowed_tools`). Use one shared, tool-neutral instruction contract in the strict mode; preserve legacy instructions only in the legacy mode.
2. Namespace resume keys and digests by mode and output-schema version so old records remain compatible but cannot satisfy a new logical cell.
3. Keep author metadata sealed and reject sensitive paths at every agent-controlled boundary, including tool calls and structured citations—not merely when loading the task.
4. Preserve the raw final answer and transcript when schema parsing fails. Mark malformed output as a terminal model outcome distinct from provider, timeout, configuration, and isolation failures.
5. Test each contract separately: field-by-field arm parity, legacy-mode compatibility, sealed path/write rejection, malformed terminal resume, and mode/schema digest separation.
