---
written_at: 2026-07-18T16:43:37Z
source_event: task:01KXEEH7RD3D03VN6EP8ZZEP0F
module: lci
category: test-failures
confidence: high
form: constraint
sources:
  - workflow:01KXH30SW9CRPTKQ1CPPV89SRJ#review-attempt-1
  - git:fdbf4d6
tags: [rewind, oracle-independence, discrimination-test, missing-criteria]
status: steering
recurrence: 1
---

# Validator oracle independence

## Lesson

A validator must not share the transformer's matching mechanism, and every mutation class needs a discrimination test proving the validator rejects its known-bad form.

## What didn't work

The TypeScript import rewriter and checker shared a relative-specifier regex. Both missed bare `next/dist/...` references, so directory renames could break a corpus while validation still marked it ready. The first review attempt rewound the task.

## Why it recurs

Happy-path validation and shared helpers make a checker look consistent while giving transformer and oracle the same blind spots. Bare or aliased references also evade logic designed around relative paths.

## Apply when

Building a mutator, rewriter, migration, corpus forge, or repair tool whose output is accepted by a self-authored validator.

## Prevention

Use an independently implemented checker or an external oracle. Enumerate relative, bare, aliased, and build-system reference forms. For each mutation class, test that the transformed form passes and a deliberately restored stale form fails.
