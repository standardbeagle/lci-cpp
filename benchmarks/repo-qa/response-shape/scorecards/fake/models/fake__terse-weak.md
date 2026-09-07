# Response-format comprehension scorecard

Complete: **yes**  
Interpretation valid: **yes**

| Model | Shape | Cells | Correct | Evidence | Hallucination | Omissions | Completion | Latency (s) | Tokens in/out/reasoning | Tokens cache read/write |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fake/terse-weak | shape_17 | 12/12 | 83.3% | 100.0% | 0.0% | 0.17 | 100.0% | 0.00 | 2167.00/55.00/0.00 | 0.00/0.00 |
| fake/terse-weak | shape_42 | 12/12 | 83.3% | 100.0% | 0.0% | 0.17 | 100.0% | 0.00 | 2167.00/55.00/0.00 | 0.00/0.00 |

## Cell accounting

planned 72 = recorded 72 + missing 0; recorded = graded 72 + failed 0. Reconciled: **yes**

## Model-class rollups

Rollups summarise the rows above and never replace them; each names its members.

## Paired effects (Arm B `shape_42` minus Arm A `shape_17`)

Decision rule applied as pre-registered: {"completion_noninferiority": 0.02, "correctness_delta": 0.05, "hallucination_noninferiority": 0.02}  
Analysis revision: `paired-task-v2`

| Group | Pairs | Correctness Δ | Spread | Hallucination Δ | Completion Δ | Decision |
|---|---:|---:|---:|---:|---:|---|
| model `fake/terse-weak` | 12 | 0.00 | 0.00 | 0.00 | 0.00 | unmeasured (correctness unmeasured, hallucination within_bound, completion within_bound) |

Degenerate spread or a floor/ceiling-pinned baseline makes these unmeasured, not certain: `fake/terse-weak`.

> Hallucination is an UPPER BOUND. A claim is judged unsupported unless a frozen fact is contained in it, so a supported fact re-expressed as different prose still counts against the arm. The true rate is at or below every number reported here.
