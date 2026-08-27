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

---

# Outcome (2026-08-27, base arm, opencode/mimo-v2.5-free)

Run: results/naming-hard/base-arm.jsonl (11/11 answered, $0).

Grading: 11/11 CORRECT, including every target. Cost: controls ~8.4-13.8k
tokens / 3-4 tool calls; targets 5.8-15.5k / 2-7. No target exceeded 2x the
control median. Worst costs were nh-chi-5 (ambiguous Write, 15.5k) and
nh-chi-4 (bare Find, 7 tool calls) — directionally P2, but far below the
hard bar.

Verdict: P1 FAILED, P3 FAILED, P5 TRIGGERED — on this corpus, with these
question phrasings, at this model tier, the information metric did NOT
predict agent failure or meaningful cost separation. The naming signal must
not be advertised as predicting agent difficulty on small corpora.

Confounds identified (why the instrument was too easy), for iteration 2:
1. QUESTION LEAKAGE: the phrasings carry the disambiguator ("chi's Compress
   middleware", "for chi's logging middleware", "why might a search fail") —
   the question hands the model the winning grep strategy. The vague-name
   difficulty only bites when the TASK doesn't name the neighborhood.
   Iteration 2: edit-style tasks ("make X happen") that force symbol
   discovery without naming it.
2. CORPUS SIZE: chi is 171 symbols; expected-match counts of ~5-8 are
   trivially enumerable in one grep + two reads. The metric's failure mode
   needs expected-match counts in the hundreds (next.js-scale corpus) to
   exceed a context budget.
3. MODEL TIER: mimo-v2.5-free grep-navigates competently. The failure
   frontier for navigation may simply be below every currently-available
   free tier on repos this small — corpus scale, not model weakness, is the
   lever we control.

Harness defects found and fixed by this run (kept as its real yield):
- opencode_runner env leaked the parent's $PWD; node/bun trusts it over
  process.cwd(), so EVERY cell ran in the parent repo, with gold-answer
  leakage (nh-chi-4 read questions/chi.json). Fixed: pin PWD to workspace.
  Prior comprehension/response-shape results that used this runner are
  suspect and need a contamination audit before being cited.
- parse_events matched "step_finish" but opencode >=1.18 emits
  "step-finish": usage tokens always read zero. Fixed: accept both.
- Stale model id: opencode/deepseek-v4-flash-free retired; current weak
  tier is opencode/mimo-v2.5-free.
