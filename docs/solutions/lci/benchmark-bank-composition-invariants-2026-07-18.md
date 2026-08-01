---
written_at: 2026-07-18T19:17:54Z
source_event: task:01KXMSV0VFYJT17BEN2PSH8WSX
module: lci
category: test-failures
confidence: high
form: procedure
sources:
  - workflow:01KXV9Q75QF406EDKAH9Q7NGJY#review-attempt-1
  - git:fff8c621a222bd0b81fe4be4bee1e2d4ad59b414
  - git:121fb2282cbd4ee168a3a3194b997a9816b02ad4
tags: [rewind, benchmark-composition, missing-criteria, hermetic-fixtures]
status: steering
recurrence: 1
---

# Benchmark-bank composition invariants

## Lesson

Encode every declared bank-composition dimension as an unconditional validator invariant, prove each invariant with an otherwise-valid hermetic negative fixture, and publish complete count maps on success.

## What didn't work

The initial implementation checked corpus and category coverage but omitted verdict coverage, so a bank could pass without an unsupported case and emit an incomplete `per_verdict` map. Earlier composition fixtures also triggered incidental annotation and metadata errors, obscuring whether the intended invariant caused the failure. Review rewound the task until all verdicts were required and fixtures isolated one composition failure each.

## Why it recurs

A real balanced bank makes happy-path assertions pass even when the validator does not enforce the property. Hand-built negative fixtures often become invalid in several unrelated ways as schemas gain cross-record constraints.

## Apply when

Creating or extending a benchmark bank with size bounds, corpus or class coverage, balance limits, paired annotations, or published distribution summaries.

## Prevention

List the supported keys for every composition dimension and compare observed key sets exactly. Enforce bounds and majority limits regardless of other failures. Generate negative fixtures from one valid base, preserving matching annotations and corpus metadata, mutate one dimension at a time, and assert the exact single diagnostic. Assert successful output includes every supported key, including zero-risk minority classes.
