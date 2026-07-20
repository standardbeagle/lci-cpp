# Staged semantic oracle v2

Decision: prevent deterministic wording misses from being reported as model failures.
The smallest supported claim is that source-location answers are scored consistently
across the frozen phrase regression bank; this does not establish correctness for
arbitrary coding answers.

The protocol is:

1. Apply named deterministic location rules and retain extracted evidence.
2. Accept exact semantic matches immediately.
3. Queue **every** non-exact answered result for a blinded LLM adjudicator. Transport
   failures remain unscored and are never sent as answer-quality failures.
4. Require strict structured claims consistent with the adjudicator verdict.
5. Never let an LLM verdict overwrite a primary score directly. A confirmed heuristic
   false negative becomes a versioned regression candidate with a proposed gap.
6. Promote a deterministic rule only after it accepts the candidate and known-good
   variants while rejecting omissions, wrong locations, extras, and negated claims.

The first record is explicitly retrospective: it repairs the known v2 `at line N`
oracle defect and must not be presented as preregistered evidence. Future benchmark
manifests must pin this oracle revision before result collection.
