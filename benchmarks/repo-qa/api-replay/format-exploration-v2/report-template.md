# LCI live search format exploration v2

Status: **not executed**

## Decision

State whether any candidate satisfied the frozen advancement rule. Advancement is only to a broader benchmark, never directly to production.

## Validity gates

- New per-model fixtures and their frozen digests:
- Sanitized safe-header profiles and their frozen digests:
- Successful authenticated pre-grid probes:
- Frozen analysis revision:
- Exact one-pointer and round-trip controls:
- Oracle discrimination:
- Complete 64-cell grid:

Any failed item makes treatment inference unavailable.

## Results by model and arm

Report usable/planned cells, exact rate and exact 95% interval, precision, recall, completion, input bytes, provider tokens, and latency. Infrastructure failures remain unscored.

## Paired contrasts and multiplicity

Report all six candidate-versus-production paired deltas, discordant counts, raw exact McNemar p-values, and Holm-adjusted p-values, including nulls and prediction misses.

## Controls and failures

Report cycle-null behavior, manipulation checks, authentication probes, every unusable or missing cell, and all retries.

## Selection and claim boundary

Apply the frozen rule mechanically. This one-task exploratory study cannot establish a general LCI format benefit or justify production rollout.

## Relationship to v1

V1 was an infrastructure-only pilot: all 64 logical cells returned HTTP 403 and yielded zero usable model outcomes. No v1 answer, score, timing, or attempt is pooled into v2.
