# Oracle v2 design audit

Verdict: **instrument passes pre-run design checks; no confirmatory result exists**.

The repaired evaluator covers all 28 tool/scenario cases, binds every schema to
the genuine output digest, and fails generation if any curated good/bad pair is
misclassified. It reports typed claim failures rather than reducing answers to a
single regex decision. All deterministic misses are queued with the actual tool
output for exhaustive review.

The adjudication protocol is resumable and preserves all attempts. Formatting
retries are bounded at three and stop after any structurally valid verdict. Two
distinct judge identities are required; only matching non-ambiguous verdicts
resolve a case. Consensus cannot modify scores directly and may only nominate a
new deterministic regression test.

Remaining evidence limits:

- The 448 existing cells predate this oracle, so their v2 analysis is exploratory.
- The 84 historical review cases have not been run through the new two-judge
  protocol; they remain unresolved.
- The 11 empty provider completions remain infrastructure failures, not semantic
  failures.
- A fresh provider-locked, preregistered run is required before comparing formats
  or selecting a winner.

Verification: schema self-check passed and the full repository suite passed all
417 tests on 2026-07-20. Some unrelated forged-corpus checks reported their
documented skip notices.
