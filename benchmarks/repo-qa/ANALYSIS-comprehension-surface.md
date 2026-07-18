# LCI whole-surface output comprehension

Grid: 112/112 cells; statuses `{'answered': 111, 'malformed_answer': 1}`.
Annotated wins: 4; current wins: 0; parity: 10.
The fileblob.Close pattern did not generalize across the surface: annotation won 4 of 14 tools, reached parity on 10, and only browse_file was both a measured winner and production-faithful. Three other wins used idealized arms.
Variance caveat: code_insight/current/strong has one scored repetition because the other response was malformed and remained unscored; all other arms have two scored repetitions.

A format win means the tested models can consume that canned representation. It does not prove the production LCI tool emits it.

## Per-tool results

| Tool | Winner | Exact Δ | Noise floor | Tier split | Production faithful | Historical calls |
|---|---:|---:|---:|---|---:|---:|
| browse_file | annotated | +0.500 | 0.433 | weak_more | true | 204 |
| code_insight | parity | +0.000 | 0.020 | none | true | 23 |
| context | parity | +0.000 | 0.020 | none | false | 7 |
| debug_info | parity | +0.250 | 0.484 | weak_more | true | 2 |
| find_files | parity | +0.000 | 0.020 | none | true | 381 |
| get_context | parity | +0.000 | 0.020 | none | false | 320 |
| git_analysis | annotated | +1.000 | 0.500 | none | false | 2 |
| index_stats | parity | +0.000 | 0.020 | none | true | 15 |
| info | annotated | +1.000 | 0.500 | none | false | 7 |
| inspect_symbol | parity | +0.000 | 0.020 | none | true | 230 |
| list_symbols | parity | +0.000 | 0.020 | none | true | 73 |
| search | parity | +0.000 | 0.020 | none | false | 2170 |
| semantic_annotations | parity | +0.250 | 0.331 | weak_more | true | 34 |
| side_effects | annotated | +1.000 | 0.500 | none | false | 2 |

## Comprehension–production boundary

- **browse_file**: Comprehension result only: models can consume this format; this does not prove LCI emits it.
- **code_insight**: Comprehension result only: models can consume this format; this does not prove LCI emits it.
- **context**: Comprehension result only: models can consume this format; this does not prove LCI emits it. The annotated arm is idealized and requires production capability work.
- **debug_info**: Comprehension result only: models can consume this format; this does not prove LCI emits it.
- **find_files**: Comprehension result only: models can consume this format; this does not prove LCI emits it.
- **get_context**: Comprehension result only: models can consume this format; this does not prove LCI emits it. The annotated arm is idealized and requires production capability work.
- **git_analysis**: Comprehension result only: models can consume this format; this does not prove LCI emits it. The annotated arm is idealized and requires production capability work.
- **index_stats**: Comprehension result only: models can consume this format; this does not prove LCI emits it.
- **info**: Comprehension result only: models can consume this format; this does not prove LCI emits it. The annotated arm is idealized and requires production capability work.
- **inspect_symbol**: Comprehension result only: models can consume this format; this does not prove LCI emits it.
- **list_symbols**: Comprehension result only: models can consume this format; this does not prove LCI emits it.
- **search**: Comprehension result only: models can consume this format; this does not prove LCI emits it. The annotated arm is idealized and requires production capability work.
- **semantic_annotations**: Comprehension result only: models can consume this format; this does not prove LCI emits it.
- **side_effects**: Comprehension result only: models can consume this format; this does not prove LCI emits it. The annotated arm is idealized and requires production capability work.

## Prediction misses

- **code_insight**: winner predicted annotated but observed parity; delta=0.000, noise_floor=0.020; tier sensitivity predicted weak_more but observed none
- **context**: winner predicted annotated but observed parity; delta=0.000, noise_floor=0.020; tier sensitivity predicted weak_more but observed none
- **debug_info**: tier sensitivity predicted none but observed weak_more
- **find_files**: winner predicted annotated but observed parity; delta=0.000, noise_floor=0.020; tier sensitivity predicted weak_more but observed none
- **get_context**: winner predicted annotated but observed parity; delta=0.000, noise_floor=0.020; tier sensitivity predicted weak_more but observed none
- **git_analysis**: tier sensitivity predicted weak_more but observed none
- **info**: winner predicted parity but observed annotated; delta=1.000, noise_floor=0.500
- **inspect_symbol**: winner predicted annotated but observed parity; delta=0.000, noise_floor=0.020; tier sensitivity predicted weak_more but observed none
- **list_symbols**: winner predicted annotated but observed parity; delta=0.000, noise_floor=0.020; tier sensitivity predicted weak_more but observed none
- **search**: winner predicted annotated but observed parity; delta=0.000, noise_floor=0.020; tier sensitivity predicted weak_more but observed none
- **semantic_annotations**: tier sensitivity predicted none but observed weak_more
- **side_effects**: tier sensitivity predicted weak_more but observed none

## Controls

- **index_stats**: null confirmed; exact Δ +0.000.
- **info**: SUSPECT — investigate; exact Δ +1.000.
  Investigation: Invalid null control: the current info blob paraphrases the oracle answer, while the annotated blob states the exact grading phrase. The measured gap tests answer wording, not neutral formatting; exclude it from production evidence.
- **semantic_annotations**: null confirmed; exact Δ +0.250.

## Production-side priority

1. **browse_file** — score 102.000 = exact gain 0.500 × 204 historical calls.
2. **git_analysis** — score 2.000 = exact gain 1.000 × 2 historical calls (requires capability work; idealized arm).
3. **side_effects** — score 2.000 = exact gain 1.000 × 2 historical calls (requires capability work; idealized arm).
