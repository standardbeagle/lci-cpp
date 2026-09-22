# Stage-2 claim-validation pilot (opencode)

Run 2026-09-22. Pre-registration: `exploration/run-configs/stage2-pilot-opencode.json`.

## Result

The pilot shows no LCI effect in either direction. It was not built to
show one: at one repetition per cell there is no spread, and the
pre-registration forbids a lift, effect size or per-category rate. What the
pilot does show is how the instrument behaves on two real models. Most of
what it found was instrument defects. Four of them were fixed before this
run and one after it.

## What ran

- 6 claims derived by `scripts/select_pilot_claims.py` (one per author
  category, two per corpus, seed 7).
- 2 arms: treatment (LCI MCP tools + Read) and baseline (Grep + Glob + Read).
- 2 models, each running both arms: `cline-pass/cline-pass/deepseek-v4.1-flash`
  and `opencode-go/qwen3.8-flash`.
- 24 cells planned, 600 s per cell, opencode 1.18.32, lci 0.10.1.

| outcome | cells |
|---|---|
| answered | 12 |
| tool_violation | 4 |
| config_error (next.js corpus blocked) | 8 |
| timeout / provider_error | 0 |

The 8 next.js cells never started. The forged next.js corpus holds a
symlink with an absolute target, and `prepare_checkout` refuses it (task
`01M34T6RMY39ABDFZEHKH87448`). They are reported as non-results. The claim
subset was not re-drawn.

## Per cell

`ok` is verdict correctness against the forged-coordinate oracle. `env` is
how the answer object was delivered.

| model | claim | category | expected | arm | status | predicted | ok | env | calls |
|---|---|---|---|---|---|---|---|---|---|
| deepseek | pb-realtime-fanout-hub | misleading-doc | unsupported | treatment | answered | false | no | bare | 19 |
| deepseek | pb-realtime-fanout-hub | misleading-doc | unsupported | baseline | tool_violation | | | | 14 |
| deepseek | pb-signed-token-mint-verify | false-premise | false | treatment | answered | true | no | bare | 11 |
| deepseek | pb-signed-token-mint-verify | false-premise | false | baseline | answered | true | no | bare | 9 |
| deepseek | skl-conditional-method | wrong-layer | false | treatment | tool_violation | | | | 25 |
| deepseek | skl-conditional-method | wrong-layer | false | baseline | answered | true | no | bare | 7 |
| deepseek | skl-estimator-copy | dead-code | true | treatment | answered | false | no | bare | 4 |
| deepseek | skl-estimator-copy | dead-code | true | baseline | answered | false | no | bare | 13 |
| qwen | pb-realtime-fanout-hub | misleading-doc | unsupported | treatment | tool_violation | | | | 16 |
| qwen | pb-realtime-fanout-hub | misleading-doc | unsupported | baseline | answered | false | no | prose_wrapped | 11 |
| qwen | pb-signed-token-mint-verify | false-premise | false | treatment | tool_violation | | | | 10 |
| qwen | pb-signed-token-mint-verify | false-premise | false | baseline | answered | true | no | bare | 5 |
| qwen | skl-conditional-method | wrong-layer | false | treatment | answered | false | yes | fenced | 14 |
| qwen | skl-conditional-method | wrong-layer | false | baseline | answered | true | no | bare | 7 |
| qwen | skl-estimator-copy | dead-code | true | treatment | answered | false | no | prose_wrapped | 20 |
| qwen | skl-estimator-copy | dead-code | true | baseline | answered | true | yes | bare | 17 |

## Observations

These are counts from 12 answered cells. None of them is a rate, and none
supports a claim about LCI.

**Verdict accuracy is low in both arms.** 2 of 12 answered cells had the
correct verdict: deepseek 0 of 6, qwen 2 of 6. By arm, treatment 1 of 5 and
baseline 1 of 7.

**Four claims have a complete pair on one model.** In one pair the
treatment was correct and the baseline wrong (qwen, skl-conditional-method).
In one pair the baseline was correct and the treatment wrong (qwen,
skl-estimator-copy). In the other two both arms were wrong. The pairs do not
point in either direction.

**No answered cell detected the false premise.** On pb-signed-token-mint-verify
all three answered cells returned `true` where the oracle expects `false`.
False-premise detection is the metric stage 2 was designed around.

**No model returned `unsupported` in any cell.** The 12 answers split 6
`true` and 6 `false`. The one claim whose expected verdict was `unsupported`
received `false` twice. Either these models do not use the third verdict, or
the prompt does not make it a real option. The pilot cannot tell which. It
should be checked before any larger run, because 10 of the 30 claims in the
bank expect `unsupported`.

**The envelope rule changes the result.** 9 answers were bare JSON, 2 were
prose-wrapped and 1 was fenced. The one correct treatment answer was
fenced, so under the strict bare-only rule the correct counts would be
treatment 0 of 5 and baseline 1 of 7. Both are reported, as the
pre-registration requires.

**Tool violations fell mostly on the treatment arm: 3 against 1.**

| arm | model | claim | cause |
|---|---|---|---|
| treatment | deepseek | skl-conditional-method | read outside the checkout, then searched for `*claim*` and `*.json` (answer-key search); every attempt refused |
| treatment | qwen | pb-signed-token-mint-verify | called LCI `search` with an argument `rx` that does not exist |
| treatment | qwen | pb-realtime-fanout-hub | called `lci_grep` and `lci_read`, tools that do not exist |
| baseline | deepseek | pb-realtime-fanout-hub | attempted `bash`, which is denied |

Two of the three treatment violations are invented LCI tool calls. Under the
strict gate the LCI surface loses cells to malformed calls from these flash
models. Four cells do not make a pattern, but a comparison that counts only
answered cells would hide this cost.

**Cost is not measured.** The adapter passed only input and output tokens
into the record and dropped cache reads, and the provider reports most of a
long prompt as cache reads. qwen recorded 186 to 240 input tokens across six
cells. This is fixed in `c6f36e4` for later runs. The pre-registered estimate,
built from the 160 discovery cells, was $0.17 for 24 cells against a $2.00
ceiling. No measured figure exists for this run.

## Instrument defects found and fixed

Each was found while running the pilot. The first four were fixed before
any cell of this run was scored. `c6f36e4` was found in this run's records
and applies to later runs only.

| commit | defect |
|---|---|
| `737368a` | registering the LCI server exposed all 15 tools; the arm allows 8 |
| `1133f8d` | opencode resolved the checkout to the enclosing lci-cpp repository; an agent read the task answer key, the scorer and the other arm's transcript (the gate rejected every read) |
| `0554e5e` | opencode's request timeouts equalled the cell deadline, so one hung provider request consumed the whole cell |
| `28a6d72` | a valid answer inside prose and a code fence was scored malformed |
| `c6f36e4` | cache tokens were dropped from the record |

Found and filed, not yet fixed:

- `01M34T6RMY39ABDFZEHKH87448`: the next.js corpus is blocked by an
  absolute symlink.
- `01M35B2YYVXR8SVV8RNPF0VK2B`: the checkout path shows the agent the task
  id and the words `claim-validation`, which prompted the answer-key search
  above.
- The gate's hand-written LCI argument schemas still allow arguments the
  server has removed (`browse_file.show_imports`, `inspect_symbol.max_depth`,
  several `get_context` fields). This is permissive only, and the server
  rejects those arguments itself.

## Before a larger run

1. Fix the checkout-path leak and the next.js corpus.
2. Find out why no model returns `unsupported`.
3. Decide whether an attempted call to a denied or invented tool should
   void the cell. The current rule costs the treatment arm more cells than
   the baseline.

## Reproducing

The scores are regenerated from the committed records:

```
/usr/bin/python3 scripts/score_claim_validation.py \
  --tasks-dir exploration/tasks \
  --records results/exploration-stage2-pilot/run-records-<model>.jsonl \
  --out-dir <dir>
```

This was run for both models and the output compared with `cmp` against
`results/exploration-stage2-pilot/scores-<model>/`. Both `scores.json` and
`aggregate.json` are byte-identical.
