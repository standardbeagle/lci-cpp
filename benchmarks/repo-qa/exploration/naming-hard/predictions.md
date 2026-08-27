# Naming-hard bench: pre-registered predictions (2026-08-27, BEFORE any model run)

Instrument: questions/naming-hard-chi.json — 8 targets mined from the
corpus-information naming signal (vague / ambiguous / obscure), 3
high-information controls. Ground truth grep-verified against the pinned
chi-base checkout, independent of LCI.

Grading: correctness against gold (judge per existing repo-qa oracle) AND
cost (total tokens, tool calls, wall time). "Hard for the model" = wrong
answer OR cost > 2x its own median control cost.

## Predictions (falsifiable)

P1. A cheap free-tier model (dsv4-free class), base arm (no LCI), fails or
    is >2x-costly on >=5 of the 8 targets, while passing all 3 controls
    cheaply.
P2. Cost ordering tracks the signal: ambiguous targets (nh-chi-1, -5) cost
    more than single-definition vague targets (nh-chi-3, -4), because the
    model must enumerate and discriminate candidates.
P3. nh-chi-8 (misspelled export) is the single most-failed task in the base
    arm: correct spelling in the search query returns nothing.
P4. The LCI arm's VOCABULARY section (ambiguous_names lists Flush(5);
    outliers lists SupressNotFound -> suppress) converts nh-chi-1 and
    nh-chi-8 from fail/costly to cheap-pass.
P5. If targets and controls show NO failure/cost separation in the base arm,
    the information metric does not predict agent difficulty on this corpus
    and must not be advertised as doing so (failed prediction = deliverable).

## Run recipe (rung 3, $0)

Per opencode-bench-run-recipe memory: git workspace, full provider/model id,
timeout >= 300s, grade hallucinated-but-confident answers as failures.
Weak->strong ladder: dsv4-free first; only escalate if it passes everything
(instrument too easy) per bench-discover-then-beachhead.
