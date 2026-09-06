# D5 — Discovery sweep: separation, prediction diff, beachhead

Corpus: PocketBase @ `d438c6a96a0252ff9df62c1cfe193480ed9ff15d` (148 554 LOC, 448 files).
Oracle: gopls v0.23.0. Registry: `discovery/predictions.json` (`d3-discovery-predictions-v1`),
pre-registered 2026-07-17 before D4 executed anything and **not edited by this slice**.
Machine-readable output: `discovery/analysis/analysis.json` (`discovery_analysis_v1`).
Agent model: `opencode-go/glm-5.2`. **Provider spend: $0** (subscription plan; no metered
provider used).

## Headline

**There is no beachhead in this corpus, and the reason is more useful than the null.**

At the tool level LCI separates from the baseline by a wide margin — and it separates
**just as hard, or harder, in the parity controls as in the hypothesis family it was
supposed to win.** At the agent level, where the baseline can actually filter what grep
returns, the gap falls to +0.037 precision — far below the registered +0.20, and the same
size as the run-to-run range (0.029) across the two independent agent runs.

Two numbers carry the whole report:

| | hypothesis family<br>`callers_call_site_high` | matched easy control<br>`control_callers_call_site_control` |
|---|---|---|
| **tool** level Δ precision | **+0.227** (10/13, p=0.092) | **+0.423** (9/9, p=0.0039) |
| **agent** level Δ precision | **+0.037**, run-to-run range 0.029 | **−0.028**, run-to-run range 0.389 |

`PREDICTIONS.md` wrote the falsification clause in advance:

> **If the treatment wins here too, LCI's advantage is task shape or tool affordance, not
> collision noise** — falsifying the epic's mechanism even while its outcome prediction
> appears to succeed. #1 must never be read alone.

The control won harder than the hypothesis. **The registered mechanism is falsified.**

## What ran

| Family | declared n | tool cells | tool runs | agent cells | agent runs |
|---|---|---|---|---|---|
| `callers_call_site_high` | 13 | 13 | 3 | 3 | 2 |
| `shadowed_definition` | 17 | **NOT RUN** | — | **NOT RUN** | — |
| `transitive_callers_depth2` | 6 | 6 | 1 | 3 | 2 |
| `referenced_outside_tests` | 10 | 10 | 3 | not run | — |
| `interface_implementations` | 2 | 2 | 1 | not run | — |
| `control_unique_name_definition` | 31 | 31 | 1 | not run | — |
| `control_literal_string_search` | 26 | 26 | 3 | 3 | 2 |
| `control_callers_call_site_control` | 9 | 9 | 3 | 3 | 2 |
| `token_budgeted_completion` | 48 | 48 | 3 | 3 | 2 |

Totals: **145 tool cells × 2 arms** on run 1 (every plannable family, full declared n),
**96 of them repeated on runs 2 and 3**; **15 agent cells × 2 arms × 2 runs = 60 agent
runs**. Zero rows are broken-cell or zero-citation artifacts.

**Run-to-run spread at the tool level is exactly 0.0 on every family and every metric.**
That is expected and is stated rather than assumed: a tool-level cell is one deterministic
MCP call and one deterministic `grep` over a pinned checkout, with no model in the loop.
The consequence matters — every tool-level delta below is outside run-to-run variance *by
construction*, so the tool level cannot hide a result in noise. **The agent level is where
variance is real, and it is measured across two independent runs.**

### NOT RUN, with the reason — never a zero row

- **`shadowed_definition` (n=17, the epic's second hypothesis) — cannot be planned.** Its
  prompt template needs `{{receiver_type}}`; `discovery/cohorts/cohorts.json` publishes no
  such field, so `discovery/runner/cells.py` raises rather than substituting an empty
  string and handing an arm a literally broken question (D2b
  `01KXRGGQ3T8JZMM0QEKE756XBD`). **No claim in this report concerns shadowing.**
- **Agent level for `referenced_outside_tests`, `interface_implementations`,
  `control_unique_name_definition` — budgeted out**, not attempted.
- **Tool runs 2–3 for `control_unique_name_definition`, `transitive_callers_depth2`,
  `interface_implementations`** — their replicate runs were killed by host memory
  pressure. Those three families report a single tool run, so their deltas carry
  **`variance_unmeasured`** and the analyzer refuses a separation claim on them regardless
  of effect size. They are read as direction only.

### Cohort reduction (recorded, not silent)

The agent level ran `--max-cells 3` on 5 families. The reduction and every dropped slug are
recorded in each run's `_plan.json` and in `analysis.json`. **Every agent-level number
below is n=3 per family**, which is below the registry's own untestability floor (n<6 →
untestable). Agent-level results are therefore reported as **direction plus variance**, and
no agent-level significance is claimed anywhere in this report.

## Where separation is real, and what it is made of

Tool level, treatment (LCI) vs baseline (grep), spread 0.0 throughout:

| Family | metric | treatment | baseline | Δ | registry verdict |
|---|---|---|---|---|---|
| `callers_call_site_high` | precision | 0.278 | 0.051 | **+0.227** | effect met, **significance not reached** (10/13, p=0.092; ≥11/13 required) |
| `control_callers_call_site_control` | precision | 0.984 | 0.561 | **+0.423** | **falsified** — parity band was \|Δ\|≤0.05 |
| `control_unique_name_definition` | f1 | 0.729 | 0.292 | **+0.437** | **falsified** — parity band was \|Δ\|≤0.05 (1 run) |
| `control_literal_string_search` | f1 | 0.840 | 1.000 | **−0.160** | **confirmed** — grep wins, 12/12, p=0.0005 |
| `referenced_outside_tests` | f1 | 0.141 | 0.175 | −0.034 | **confirmed** — parity |
| `transitive_callers_depth2` | recall | 0.177 | 0.026 | +0.151 | **falsified** — needed ≥0.20 *and* unanimous 6/6 (1 run) |
| `interface_implementations` | f1 | 0.100 | 0.006 | +0.094 | not evaluable — n=2, no threshold registered, by design |
| `token_budgeted_completion` | DNF rate | 0.0 % | 0.0 % | 0.0 pp | **not informative at this level** — see miss 4 |

### The one number that explains the table

**Baseline recall is 1.000 on every graded family.** Raw `grep` always contains the answer.
What it lacks is precision — 0.051 on high-noise callers, 0.201 on unique-name definitions,
0.561 on the *easy* callers control — because the tool-level baseline is one unfiltered
grep with **no classification step**. Every tool-level "LCI win" here is a precision win
over an arm that was never allowed to filter, which is why the controls win too and why the
noise axis does not predict the win.

### Wins above noise ratio N — there is no knee

Cumulative floors over all 100 graded cells (2.0 and 8.0 are D2's own published cohort
thresholds, carried over, not retuned):

| call-site noise ≥ | cells | mean Δ | treatment win rate |
|---|---|---|---|
| 1.0 | 100 | +0.141 | 51.0 % |
| 2.0 | 84 | +0.135 | 50.0 % |
| 4.0 | 35 | +0.130 | 57.1 % |
| 8.0 | 31 | +0.136 | 58.1 % |
| 16.0 | 16 | +0.175 | 56.3 % |
| 32.0 | 10 | +0.296 | 60.0 % |

**There is no threshold N above which LCI starts winning.** The win rate is a coin flip
from noise 1 to noise 32, rising 9 points across a 32× change in the axis the epic named as
its mechanism. The mean delta does rise — but that is two cells doing the work:

Per-cell Δ precision in `callers_call_site_high`, by call-site noise:

| noise | symbol | Δ |
|---|---|---|
| 67.2 | `Close` | +0.004 |
| 64.0 | `FetchAuthUser` (lark) | **+0.984** |
| 64.0 | `FetchAuthUser` (planningcenter) | **+0.984** |
| 45.0 | `LastMessage` | +0.001 |
| 38.0 | `SetHidden` | −0.026 |
| 29.0 | `SetProxyRecord` | +0.032 |
| 23.0 | `Exists` | +0.033 |
| 20.0 | `setValue` | −0.050 |
| 14.0 | `Write` | +0.091 |
| 9.8 | `SetName` | +0.120 |
| 9.0 | `OnRecordAuthRequest` | **+0.889** |
| 8.1 | `BindFunc` | +0.011 |
| 8.0 | `nextFunc` | −0.125 |

**Ten of thirteen cells sit inside ±0.13**, and the noisiest cell in the cohort (`Close`,
67.2×) is a dead heat. The easy control's nine cells, by contrast, all move together
(+0.107 … +0.915). A real noise mechanism would produce the opposite pattern.

### The product finding inside the hypothesis family

Treatment precision on the high-noise family is **0.278**, and recall **0.689** — versus
0.984 / 1.000 on the easy control. **LCI's own `callers` output degrades on common-named
methods**, just less catastrophically than raw grep. That is a measured capability gap in
the product on real data, and it is the most actionable line in this report.

## Tool-level correctness vs agent-level lift

Never averaged. The tool level asks *is LCI correct*; the agent level asks *does an agent
gain from it* — which is the product claim.

| Family | tool Δ | agent Δ | agent spread (max−min across runs) | reading |
|---|---|---|---|---|
| `callers_call_site_high` | +0.227 precision | **+0.037** | 0.029 | the tool-level gap does not survive an agent that can filter |
| `control_callers_call_site_control` | +0.423 precision | **−0.028** | 0.389 | **tool-correct, no agent lift** (LCI tool-level f1 0.86) |
| `control_literal_string_search` | −0.160 f1 | −0.026 | 0.055 | grep's tool-level edge also shrinks under an agent |
| `transitive_callers_depth2` | +0.151 recall | −0.238 | 0.475 | inside variance in both directions; uninterpretable at n=3 |
| `token_budgeted_completion` | 0.0 pp DNF | 0.0 pp DNF | 0.0 | no DNF in either arm at n=3 |

`control_callers_call_site_control` is the sweep's one **tool-correct / no-agent-lift**
family (named as such in `analysis.json`). Ordinarily that reads as an *adoption* problem —
LCI has the capability, the agent is not exploiting it. Here it reads differently, and the
difference matters: the tool-level "capability" was itself a filtering artifact, so there
is no adoption gap to close. **A tool-level win that vanishes at the agent level should be
treated as an instrument question before it is treated as a prompting question.**

### Cost, where the agent level does show a consistent signal

| Family | arm | tool calls | tokens | wall (s) | DNF |
|---|---|---|---|---|---|
| `callers_call_site_high` | LCI | 16.2 | 34 035 | 109 | 0 % |
| | grep | 12.2 | 22 550 | 80 | 0 % |
| `control_literal_string_search` | LCI | 6.7 | 30 164 | 154 | **16.7 %** |
| | grep | 2.5 | 14 975 | 200 | **50.0 %** |
| `transitive_callers_depth2` | LCI | 11.8 | 30 579 | 221 | **50.0 %** |
| | grep | 8.5 | 18 287 | 256 | **66.7 %** |
| `token_budgeted_completion` | LCI | **4.3** | **11 820** | 47 | 0 % |
| | grep | **16.3** | **32 236** | 105 | 0 % |

Tokens are **billable only** — input + output + reasoning, as `analyze_discovery.py`
sums them. `bench.py` also emits flat `cache_read` / `cache_write` counters; on these
rows they are ~85 % of the raw object and neither arm pays for them, so counting them
would report the cache-warm arm as the expensive one. They are not reported here.

Two things are visible and they point in opposite directions:

- **Completion.** In both families where anything failed to finish, the LCI arm finished
  more often (16.7 % vs 50.0 %; 50.0 % vs 66.7 %). That is the same direction as the prior
  tiers' only reproducible win (DNF 10.8 % → 6.5 %). At n=3 per family it is **direction
  only** — the registry itself computed that detecting a 4 pp DNF gap needs n≈900 per arm.
- **Token cost is not consistently lower.** On the budget-constrained family the LCI arm
  used **2.7× fewer billable tokens and 3.8× fewer calls**; on the callers family it used
  **1.5× more** of both. The registered mechanism ("answers arrive in fewer, denser calls") holds
  where the task is a lookup and inverts where the agent has to reconcile LCI's output
  against the files.

## Where the predictions failed

Misses lead. Eight (family, level) predictions did not hold; each gets a named hypothesis
for **why the intuition was wrong**.

### Miss 1 — `control_callers_call_site_control` (tool): predicted **parity**, measured **LCI +0.423** — larger than the hypothesis family

*Registered mechanism:* "One grep returns at most a couple of hits per call site, nearly
all true. Holding task shape fixed and varying only call-site collision noise is what
isolates the noise mechanism."

*Why it was wrong:* the prediction reasoned about **how many hits grep returns**, not about
**who filters them**. At ≤2 hits per call site the baseline still cites the definition line
and any comment mention alongside the real call site — precision 0.561, not 1.0. The
mechanism assumed a classification step the tool-level baseline does not have and was never
given. **The mental model treated "grep returns few hits" as equivalent to "grep is
right".** These are different claims, and the whole tool level rests on the difference.

*Consequence:* this control was authored to catch exactly this and it caught it. The epic's
collision-noise mechanism is falsified on its own pre-registered terms.

### Miss 2 — `control_unique_name_definition` (tool): predicted **parity**, measured **LCI +0.437**

*Registered mechanism:* "def_count=1 means exactly ONE hit is definition-shaped… the
baseline greps the name and takes the single `func` line."

*Why it was wrong:* same root cause, and this is the **instrument null check**, so its
failure is load-bearing. "Takes the single `func` line" is a *reading* step the baseline
cannot perform in one tool call; its precision is 0.201 because it cites every hit, one of
which is the definition. Since the instrument is biased toward the treatment on definition
lookups *generally*, **`shadowed_definition` could not have been attributed to shadowing
even if it had run.** The unrunnable family and the failed control compound: the entire
definition-lookup half of the epic is uninterpretable at this level.

### Miss 3 — `transitive_callers_depth2` (tool and agent): predicted **LCI wins**, measured **no separation**

*Registered mechanism:* enumeration load — depth-2 closure needs one grep round per depth-1
caller.

*Why it was wrong:* **the level cannot express the task.** Both tool-level arms get exactly
one call, so neither performs a second round; recall is 0.177 and 0.026 — both answering a
depth-1 question against a depth-2 key. This is a known harness limitation
(`01M1VSHDWG8QQ27S8XM8FMA7HH`), not a measurement. At the agent level the sign flips
(−0.238) with a run-to-run range of 0.475 — pure noise at n=3. **The prediction is neither confirmed
nor falsified; it is untested**, and the +0.151 is not banked.

### Miss 4 — `token_budgeted_completion` (tool): predicted **LCI wins on DNF**, measured **0.0 % vs 0.0 %**

A direct tool call cannot fail to complete — there is no budget to exhaust, so DNF is
identically zero for both arms across all 48 cells and 3 runs. **Registry finding: a family
whose metric is undefined at one of the two levels should declare the level it applies to.**
Running its 48 cells at the tool level consumed a large share of the sweep's oracle budget
to produce a structurally guaranteed row.

### Miss 5 — `callers_call_site_high` (agent): predicted **LCI wins**, measured **+0.037 against a 0.029 run-to-run range**

*Why it was wrong:* the epic's central hypothesis was argued from what grep *returns*. An
agent does not stop at what grep returns — it reads the hits. Once the baseline is allowed
the reading step, 89 % of the tool-level gap disappears. **The hypothesis was really a
hypothesis about tool output, and it was being sold as a hypothesis about agent
performance.**

### Miss 6 — `control_literal_string_search` (agent): predicted **grep wins**, measured **parity** (−0.026, run-to-run range 0.055)

*Why it was wrong:* two reasons, and the second is an instrument defect. First, at the
agent level both arms score badly (0.235 / 0.261) because exhaustive literal enumeration is
hard for a model regardless of tool. Second — **the agent-level treatment arm still has
grep** (see caveat 1), so this control has no teeth at the agent level: it was designed
around the treatment *not* having grep. Its tool-level result, where the disjointness is
real, stands and is the sweep's one confirmed separation.

### Miss 7 — `interface_implementations` (tool): predicted **LCI wins** (directional only), measured **+0.094, not evaluable**

Registered with no threshold and null confidence because n=2 cannot falsify anything. Both
arms are near-zero (0.100 / 0.006). Recorded, not read. Its two cells are also
`callers_call_site_high` members, so per the registry it is **not independent evidence**.

### Predictions that held

- **`control_literal_string_search` (tool) — grep wins, exactly as registered** (Δ f1
  −0.160, 12/12 discordant cells to the baseline, p=0.0005). This is why the rest of the
  report can be believed: **the instrument can report an LCI loss.** The treatment reaches
  0.840 through `search` alone with no grep fallback — a stronger showing than registered —
  but it loses, in the registered direction, past the registered margin.
- **`referenced_outside_tests` — parity, as registered** (Δ f1 −0.034, inside ±0.10), though
  for a sharper reason than registered: **neither arm partitions by path at all** in one
  call. Both cite everything and score precision 0.079 / 0.102 against a production-only
  answer set. Per the registry's own rule this is *"no separation detected, underpowered to
  detect one"* — never *"parity established"*.
- The registry's `registry_findings` also held: `mcp__lci__references` does not exist
  (`01M1VR4TAQ2W3N380QVG6WHQQQ`), so `predictions.json`'s arm definition still names a
  phantom tool. D4 routed that family through `search`. **`predictions.json` was not edited
  by this slice**, per the amendment policy.

## Beachhead recommendation

**None. This is a null result and it is not being massaged into a marginal win.**

- `callers_call_site_high` meets its effect size (+0.227 ≥ 0.20) but reaches only p=0.092
  against a registered p<0.05 — 10/13 where 11/13 was required. Per `PREDICTIONS.md`'s
  "What D5 may not say", the honest reading is **"directionally consistent, underpowered,
  needs a bigger or different corpus" — not a beachhead.**
- That directional consistency is undermined from inside: the matched easy control wins
  *harder* and unanimously, so whatever is being measured is not the registered mechanism.
- The gap does not survive contact with an agent (+0.037 against a 0.029 run-to-run range).
- The only confirmed separation in the sweep runs the other way (grep wins on literal
  search).

### The next lever, in priority order

1. **Fix the comparison before spending anything else.** The tool-level baseline is
   unfiltered `grep`; the treatment is a semantic index. That comparison measures a
   filtering step, not a product. Either give the baseline arm a classification pass at the
   tool level, or stop treating tool-level precision deltas as product evidence and make
   the agent level the only claim-bearing level. **Everything below is wasted until this is
   settled** — a bigger corpus measured with the current instrument yields a bigger version
   of the same artifact.
2. **Make the agent arms actually disjoint** (caveat 1). The registered treatment has no
   grep; the implemented one does. Until that holds, no agent-level control can prove an
   LCI loss, and the agent level is the level that matters.
3. **Unblock `shadowed_definition`** — publish `receiver_type` in `cohorts.json`
   (`01KXRGGQ3T8JZMM0QEKE756XBD`). It is the largest untested hypothesis (n=17) and the
   only registered mechanism — receiver-typed identity — that a filtering step plausibly
   cannot reproduce. Its own control needs fixing in the same pass (miss 2).
4. **Make the depth-2 family expressible** (`01M1VSHDWG8QQ27S8XM8FMA7HH`). Enumeration load
   is the one registered mechanism this sweep never tested at all.
5. **Chase completion, not accuracy.** The only LCI-positive signal that appears twice,
   with a prior behind it, is completion rate (16.7 % vs 50.0 % and 50.0 % vs 66.7 % DNF)
   and call/token cost on lookup-shaped tasks (2.7× fewer billable tokens). Both are underpowered
   here by design. A bench built around *finishing within budget* rather than *set-wise
   accuracy* would be measuring the thing that has actually reproduced across tiers.
6. **Only then, scale the corpus.** PocketBase holds few high-noise and few many-implementor
   functions — `interface_implementations` is n=2 and confounded. Prior tiers put
   grep-parity territory near ~150k LOC and PocketBase is 148k.

**A cohort-selected sweep did not rescue the 0.96-vs-0.96 null of the prior tiers. It
replaced it with a specific, fixable diagnosis: the axis the product bet on does not predict
its wins, and the wins it does show at the tool level are an artifact of the comparison.**
That diagnosis is the deliverable.

## Instrument caveats carried into every number above

1. **The agent-level arms are NOT disjoint, contrary to the registry.**
   `predictions.json` defines the treatment as "LCI MCP + Read, **no Grep, no Glob**", but
   `bench.workspace_config` (`benchmarks/repo-qa/scripts/bench.py:58`) only *adds* the LCI
   MCP server for the `lci` variant — it never disables the native grep/glob tools. **At
   the agent level the treatment keeps grep.** Outside this slice's fileScope: reported,
   not patched. It removes `control_literal_string_search`'s teeth at the agent level and
   makes every agent-level treatment number an "LCI *in addition to* grep" measurement, not
   the registered "LCI *instead of* grep".
2. **The agent level runs a different LCI binary from the tool level.** The workspace config
   takes `lci-bin` from `config.kdl` (`build/src/lci`, built 2026-07-15); the sweep's
   `--lci-bin` reaches only the tool level. Also outside fileScope.
3. **`search` payloads truncate at 100 hits and render as complete**
   (`01M1VSHDVWEF7MHQWHW8J5W68H`) — affects the `search`-routed families
   (`control_literal_string_search`, `referenced_outside_tests`,
   `token_budgeted_completion`) on cells above 100 true hits (`BindFunc` 100/146, `Close`
   100/409 confirmed live). Direction is known: it suppresses treatment **recall**, so
   those treatment numbers are floors.
4. **Tool-level `transitive_callers_depth2` is depth-1 in both arms**
   (`01M1VSHDWG8QQ27S8XM8FMA7HH`).
5. **A single tool or provider error aborts the whole sweep** (`BrokenCellError`). Correct
   for a pre-registered measurement, but the agent level had to be driven under a retry
   loop; resume-safety made that harmless (both runs completed on attempt 1).
6. `zero_citation_count` is **0 on every arm of every family**, so no row here is a "cited
   nothing" artifact of the kind D4's first attempt produced.
7. Three families carry **one tool run only** (host memory pressure killed their
   replicates), so their deltas are flagged `variance_unmeasured` and no separation is
   claimed from them.

## Reproducing

    # tool level (deterministic; one gopls key per unique symbol, cache-warm after the first)
    PATH=/usr/local/go/bin:$PATH /usr/bin/python3 scripts/discovery_sweep.py run \
      --levels tool --corpus <checkout>/real_projects/go/pocketbase \
      --lci-bin /home/beagle/work/core/lci-cpp/build/release/src/lci \
      --out .work/discovery/tool-run1        # --families ... to exclude shadowed_definition

    # agent level, reduced cohort, $0 model
    ... --levels agent --corpus-repo pocketbase --model opencode-go/glm-5.2 \
        --max-cells 3 --timeout 300 --out .work/discovery/agent-runA

    # analysis over N independent runs
    /usr/bin/python3 scripts/analyze_discovery.py \
      --runs .work/discovery/tool-run1,...,.work/discovery/agent-runB \
      --not-run 'shadowed_definition=cohorts.json publishes no receiver_type field; discovery/runner/cells.py build_plan raises rather than substituting an empty string (D2b 01KXRGGQ3T8JZMM0QEKE756XBD)' \
      --out discovery/analysis

Sweep results live under `benchmarks/repo-qa/.work/discovery/` and are **gitignored** — they
carry corpus content. Only `discovery/analysis/analysis.json` is committed.
