# Preregistration manifest

Store the manifest as JSON. The validator checks structure, not scientific truth.

```json
{
  "schema": "benchmark.preregistration.v1",
  "benchmark_id": "stable-id",
  "decision": "Decision this evidence informs",
  "claim_boundary": "Narrowest claim the design supports",
  "experimental_unit": "One model response to one frozen task and arm",
  "population": "Tasks and systems to which results may generalize",
  "arms": [
    {"id": "opaque-a", "treatment": "Declared difference", "production_faithful": true},
    {"id": "opaque-b", "treatment": "Declared difference", "production_faithful": true}
  ],
  "equivalence": {
    "invariants": ["same question", "same answer facts", "same time budget"],
    "known_differences": ["declared treatment only"],
    "verification": "How equivalence is checked"
  },
  "hypotheses": [{"id": "h1", "prediction": "opaque-a wins", "metric": "exact_rate"}],
  "metrics": [{"id": "exact_rate", "primary": true, "direction": "higher"}],
  "strata": ["model capability tier"],
  "controls": [{"id": "null-1", "kind": "null", "expected": "parity"}],
  "repetitions": 2,
  "stopping_rule": "Run the complete frozen grid",
  "noise_rule": "max(0.02, pooled within-cell standard deviation)",
  "practical_effect_threshold": 0.02,
  "exclusions": ["malformed provider response remains unscored"],
  "failure_policy": "Classify infrastructure failures; never score them as wrong answers",
  "oracle": {
    "source": "Independent source of truth",
    "independence_argument": "Why it cannot share the tested failure mode",
    "good_case": "Known-good case",
    "bad_case": "Plausible wrong case that must fail"
  },
  "prompt_neutrality": "How instructions, labels, order, and blinding avoid leading",
  "leakage_controls": ["No oracle or solution material in model-visible context"],
  "analysis_plan": "Frozen computation, aggregation, and comparisons",
  "reporting_plan": "Raw data, JSON analysis, report, misses, nulls, caveats",
  "fixture_digests": {"fixtures.json": "sha256:..."},
  "analysis_revision": "git commit or immutable digest",
  "registered_at": "RFC 3339 timestamp"
}
```

Run:

```bash
python3 .agents/skills/design-unbiased-benchmarks/scripts/validate_manifest.py path/to/manifest.json
```
