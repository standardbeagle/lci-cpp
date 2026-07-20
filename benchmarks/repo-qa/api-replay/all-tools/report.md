# All-LCI-tools OpenCode replay pilot

Status: **execution complete; semantic evidence incomplete; no format selection**

## Outcome

The pilot covered all 14 live LCI MCP tools with one genuine success and one genuine
negative-path output per tool. Across four encodings, two models, and two repetitions,
all 448 planned direct OpenCode continuation requests were attempted. There were 437
answers and 11 HTTP-200 empty completions. No timeout, quota, 4xx, 5xx, transport
failure, or retry occurred.

The pilot cannot support a format winner. Its preregistration prohibited selection
with two repetitions, and the semantic oracle remains incomplete: 370 answers pass
the repaired deterministic heuristics, 11 are unscored infrastructure completions,
and 67 require follow-up. A fully grounded cross-model LLM pass reviewed all of those
cases (before the two safe `inspect_symbol` promotions): 22 were called correct, 38
incorrect, three ambiguous, and six produced structurally invalid judgments. Two
correct location-wording cases were safely promoted; the other 20 correct judgments
remain regression candidates rather than score overrides.

## Completion and diagnostics

| Model | Format | Answered | Empty | Mean input tokens | Mean output tokens | Mean latency |
|---|---|---:|---:|---:|---:|---:|
| DeepSeek V4 Flash | production JSON/text | 52/56 | 4 | 7,833 | 144 | 2.65 s |
| DeepSeek V4 Flash | readable XML | 53/56 | 3 | 8,322 | 186 | 3.05 s |
| DeepSeek V4 Flash | path records | 52/56 | 4 | 8,866 | 179 | 2.98 s |
| DeepSeek V4 Flash | tagged blocks | 56/56 | 0 | 8,214 | 215 | 3.41 s |
| GLM-5.2 | production JSON/text | 56/56 | 0 | 7,340 | 93 | 8.18 s |
| GLM-5.2 | readable XML | 56/56 | 0 | 7,835 | 86 | 6.55 s |
| GLM-5.2 | path records | 56/56 | 0 | 8,286 | 114 | 9.54 s |
| GLM-5.2 | tagged blocks | 56/56 | 0 | 7,714 | 103 | 9.99 s |

These are diagnostics, not selection evidence. In particular, DeepSeek's zero empty
tagged-block completions is an exploratory transport observation with only 56 cells
and no preregistered completion-effect test.

## Oracle iterations

The first adjudication batch was rejected because case-folded truth caused valid
identifier capitalization to be judged wrong. The second was rejected because
success truth omitted the source tool result, making supported extra details appear
unsupported. The third included original-cased required atoms and the complete
genuine tool output. Six of its 69 judgments still failed structured consistency.

Only one deterministic improvement passed both positive and adversarial tests:
`inspect_symbol` now recognizes `path:line` and `path at line N`. Broader proposed
rules for `info` and `get_context` failed discrimination by accepting known-wrong or
unresolved answers, so they were not promoted.

## Claim boundary and next step

This establishes that the transport harness can replay all LCI tool shapes, including
LCF text, large contexts, empty results, corrective responses, and explicit errors.
It does not establish relative format quality. The next iteration must freeze a
task-specific semantic claim schema, add source-grounded adversarial cases per tool,
and resolve or replace the adjudicator before collecting a higher-repetition grid.
