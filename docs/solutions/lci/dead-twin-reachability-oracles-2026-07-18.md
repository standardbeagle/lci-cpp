---
written_at: 2026-07-18T18:07:10Z
source_event: task:01KXMSSERFSVG8V7V989WYVB91
module: lci
category: test-failures
confidence: high
form: constraint
sources:
  - workflow:01KXV5MMC9MMR99R3BMA4D4ZG4#review-attempt-1
  - git:b2f5fd76c1c7e5e72ae150d96a59856e95d87d92
tags: [rewind, oracle-independence, path-resolution, discrimination-test]
status: steering
recurrence: 2
---

# Dead-twin reachability oracles

## Lesson

A reachability validator must resolve references from each live source's directory, remain independent of production rewriting matchers, and prove every supported reference form with a known-bad fixture.

## What didn't work

Green unit tests did not cover Python package-relative imports or relative specifiers containing intermediate directories. The first review also found that the oracle reused the production absolute-reference matcher, allowing shared blind spots. Review rewound the task until independent parsing and negative fixtures covered both cases.

## Why it recurs

String matching against a generated module name ignores that import semantics depend on the importing file's directory and language. Reusing production matchers makes the checker agree with the implementation even when both omit the same syntax.

## Apply when

Validating that generated, renamed, moved, deprecated, or adversarial twin modules are unreachable from live sources.

## Prevention

Resolve each relative reference against its live source directory using an independently implemented parser. Enumerate absolute, package-relative, intermediate-directory, aliased, and renamed-symbol forms. For every supported form, include a hermetic negative fixture that makes the forbidden target reachable and must fail validation.
