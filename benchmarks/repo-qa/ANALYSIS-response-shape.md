# Response-format comprehension scorecard

Complete: **yes**  
Interpretation valid: **no**

| Model | Shape | Cells | Correct | Evidence | Hallucination | Omissions | Completion | Latency (s) | Tokens in/out |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| opencode/deepseek-v4-flash-free | shape_17 | 12/12 | 66.7% | 66.7% | 91.7% | 0.33 | 100.0% | 9.93 | 0.00/0.00 |
| opencode/deepseek-v4-flash-free | shape_42 | 12/12 | 58.3% | 58.3% | 100.0% | 0.33 | 100.0% | 11.28 | 0.00/0.00 |
| opencode-go/glm-5.2 | shape_17 | 9/12 | 55.6% | 55.6% | 77.8% | 0.44 | 75.0% | 78.97 | 0.00/0.00 |
| opencode-go/glm-5.2 | shape_42 | 8/12 | 50.0% | 62.5% | 87.5% | 0.50 | 66.7% | 72.10 | 0.00/0.00 |

## Model-class rollups

- `strong|shape_17`: correctness 55.6%, evidence 55.6%, completion 75.0%
- `strong|shape_42`: correctness 50.0%, evidence 62.5%, completion 66.7%
- `weak|shape_17`: correctness 66.7%, evidence 66.7%, completion 100.0%
- `weak|shape_42`: correctness 58.3%, evidence 58.3%, completion 100.0%

## Failures

- `malformed_answer`: 7
