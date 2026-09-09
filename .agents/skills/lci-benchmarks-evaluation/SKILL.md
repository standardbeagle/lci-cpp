---
name: lci-benchmarks-evaluation
description: "Use when: measuring lci (perf gate, real-project tests, goldens, repo-qa banks), reading ANALYSIS-*.md, checking a README speed/context claim, or comparing lci against ripgrep, Zoekt, Sourcegraph, ctags, an LSP, Aider, Cursor, Serena, or GitHub code search."
---

# lci benchmarks and evaluation

How lci is measured today, where the numbers live, and the procedure for an honest comparison against another tool. The perf gate guards latency regressions; integration goldens are the sole correctness oracle (Go parity retired); `benchmarks/repo-qa/` measures whether lci helps an agent. The methodology skills `design-unbiased-benchmarks` and `audit-benchmark-evidence` in `.agents/skills/` are the rulebook; this skill maps lci onto them and does not repeat them.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| ctest entry `lci_benchmarks` | `tests/CMakeLists.txt:341` | google-benchmark binary, label `benchmark`, excluded from the release ctest preset |
| CI job `benchmarks` | `.github/workflows/ci.yml:346` | main-only, self-hosted runner, waits for loadavg < 2, then `bench_gate.py --threshold 1.5` |
| `scripts/bench_gate.py` | `scripts/bench_gate.py:203` | default threshold 1.5x; `--update` rewrites the baseline; refuses judgment above `DEFAULT_MAX_LOAD` 2.0 |
| baseline | `tests/benchmarks/baseline/linux-x64.json` | 14 synthetic BM_ entries, host `beagle-ab`, dated 2026-07-12 |
| ctest entry `lci_real_project_suite` | `tests/CMakeLists.txt:285` | RUN_SERIAL, 600 s, env `LCI_CPP` + `PARITY_CORPORA` |
| ctest entry `lci_integration_suite` | `tests/CMakeLists.txt:238` | goldens diff; `LCI_UPDATE_GOLDENS=1` re-pins (`tests/integration/spec_runner.cpp:726`) |
| `tests/parity/corpora/prep_real.sh` | `tests/parity/corpora/prep_real.sh:1` | symlinks lci-go-repo / lci-cpp-repo / lci-test; Go repo is gone (see traps) |
| `scripts/add-real-projects.sh` | `scripts/add-real-projects.sh:23` | clones `real_projects/<lang>/<name>` submodules |
| `scripts/fetch-smoke-corpora.sh` | `scripts/fetch-smoke-corpora.sh:12` | big corpora for `tests/soak/index_size_smoke_test.cpp` |
| `scripts/profile_test_suite.py`, `check_test_gate_budget.py` | `scripts/` | test-suite wall-clock baseline and budget gate |
| repo-qa runner | `benchmarks/repo-qa/scripts/bench.py:58` | `workspace_config` builds the base / lci / lci-slim / lci-ann arms |
| repo-qa config | `benchmarks/repo-qa/config.kdl:89` | repos with minefield index, model aliases, tiers 0-3 |
| corpora pins | `benchmarks/repo-qa/exploration/corpora.json:25,71,121` | scikit-learn, pocketbase, next.js pinned commits; forged copies live under gitignored `.work/exploration/` |
| tool-surface snapshot | `benchmarks/repo-qa/comprehension/surface/tool-surface.json` | 15 tools; freshness test `benchmarks/repo-qa/tests/test_tool_surface.py:45` |
| rg-differential oracle | `tests/search_rg_differential_test.cpp:220` | literal search vs naive scanner and `rg --fixed-strings` |

## Code map

Perf gate:
- `tests/benchmarks/benchmark_main.cpp` — synthetic BM_ set: trigram extract/index/find, `BM_SearchSmallIndex`/`BM_SearchMediumIndex`, RE2 vs std::regex row filter, parser pool, fuzzy match, `BM_IndexingThroughput/{100,500,1000}`.
- `tests/benchmarks/real_project_benchmarks.cpp` — `BM_RealProjectIndexChi/Pocketbase/Fastapi`, `BM_RealProjectSearchChi/Fastapi`, `BM_RealProjectIndexAll`; excluded on CI (`--benchmark_filter=-RealProject`).
- `tests/benchmarks/real_project_advanced_benchmarks.cpp` — `BM_RealProjectCodeInsightOverview/Statistics`, `BM_RealProjectGetContext`, `BM_RealProjectFindFiles`, `BM_RealProjectSearchLatency`.
- `scripts/bench_gate.py` — `cmd_check` computes current/baseline ratio per name; mismatch in name set is also a failure.

Real-project tests (skip when corpus absent):
- `tests/helpers/real_project_helpers.h:59` — `find_real_project(lang, name)` under `real_projects/<lang>/<name>`; `list_available_real_projects` at line 91.
- `tests/integration/real_project_*.cpp` — 9 files; each defines its own `SKIP_IF_NO_REAL_PROJECT(lang, name)` macro (e.g. `real_project_performance_test.cpp:26`).
- `tests/integration/real_project_performance_test.cpp:42,77` — `ChiSearchUnder5ms`, `FastapiSearchUnder5ms` (best-of-5 min, bound 10000 us), `ChiGetContextUnder50ms`, `ChiOverviewUnder500ms`.
- `tests/helpers/performance_guards.h` — env-gated perf guards used by unit tests.

Correctness oracle:
- `tests/integration/spec_runner.{h,cpp}` — `SpecCase`, golden path, normalisation; `tests/integration/README.md` documents the update flow.
- `tests/integration/goldens/{cli,http,mcp}/` — 82 golden files; MCP goldens per tool directory.
- `tests/search_rg_differential_test.cpp` — `RandomLiteralPatternsMatchIndependentOracles`, `DirectedCornerPatternsMatchNaiveOracle`, `CaseInsensitive...`, `InvertMatchEqualsInvertOracle`, `ExcludeCommentsDropsCommentOnlyLines`.

Agent-level benchmark (`benchmarks/repo-qa/`):
- `README.md` — rubric (tokens, cost, wall, tool calls, fact accuracy, LLM judge), difficulty ladder, minefield tiers.
- `scripts/benchlib.py` — flat-KDL parser for `config.kdl`; `scripts/bench.py` run, `judge.py` score, `report.py` aggregate.
- `scripts/profile_repos.py` -> `repo-profiles.json` — minefield index computed by lci itself (self-referential; see traps).
- `scripts/tooleval.py` + `tool-cases/chi.json` — direct MCP calls with expected results, no model.
- `scripts/mock_lci_mcp.py` — hermetic MCP with lci tool names and injectable descriptions; serves permissive schemas.
- `scripts/enumerate_tool_surface.py` — probes live `tools/list` into `tool-surface.json`.
- `scripts/gopls_oracle.py`, `semantic_location_oracle.py`, `run_edit_oracles.py` — lci-independent answer keys (gopls, behaviour tests).
- `scripts/discovery_sweep.py` + `discovery/PREDICTIONS.md` + `discovery/predictions.json` — pre-registered two-arm sweep.
- `scripts/exploration_runner.py`, `exploration_corpus_forge.py`, `lint_exploration_leaks.py` — mutated-corpus exploration bank (30 tasks in `exploration/tasks/`).
- `edits/` (24 tasks, oracles, patches), `comprehension/`, `toolcalling/` (16 tasks), `response-shape/` — the other banks, each with `predictions.json` where pre-registered.
- `ANALYSIS-*.md` — conclusions per run; `results/` holds raw ledgers.
- `docs/plans/2026-07-18-unbiased-benchmark-skills-design.md`, `docs/solutions/lci/*-2026-07-18.md` — why the two methodology skills exist.

Suite performance docs: `docs/performance/test-suite-baseline.md` (S1 measurement), `test-sharding.md` (no-change decision, keep `gtest_discover_tests`), `integration-index-cache.md` (no cache written), `test-suite-final.md` (180 s budget FAIL), `test-suite-tail-variance.md` (RUN_SERIAL for bundles), `profiling-wsl2.md` (PMU works on WSL2).

## Config knobs

No `.lci.kdl` key or `include/lci/config.h` field governs benchmarking. The knobs are environment and bench config:

| Knob | Where read | Effect |
|---|---|---|
| `LCI_CPP` | `tests/integration/spec_runner.cpp:276` | binary under test for spec/golden runs (ctest sets it) |
| `PARITY_CORPORA` | `tests/integration/spec_runner.cpp:194` | corpus root for spec fixtures |
| `LCI_UPDATE_GOLDENS=1` | `tests/integration/spec_runner.cpp:726` | rewrite goldens instead of diffing |
| `LCI_SMOKE_CORPORA_DIR` | `scripts/fetch-smoke-corpora.sh:13` | destination of soak corpora, default `.work/smoke-corpora` |
| `LCI_GO_REPO` / `LCI_CPP_REPO` / `LCI_TEST_REPO` | `tests/parity/corpora/prep_real.sh:22-24` | symlink targets |
| `--threshold`, `--max-load` | `scripts/bench_gate.py:203` | 1.5 and 2.0 defaults |
| `defaults.lci-bin`, `corpus-root`, `judge-model`, `timeout-seconds`, `concurrency` | `benchmarks/repo-qa/config.kdl:7-24` | repo-qa runner; `lci-bin` currently `build/src/lci` |
| `LCI_MOCK_DESCRIPTIONS` | `benchmarks/repo-qa/scripts/mock_lci_mcp.py` | description variant for the selection bench |

## Invariants and traps

- Perf gate under load fakes regressions (9 false ones up to 2.19x at loadavg 10+); the gate demotes itself above load 2.0 and CI snapshots pre-run load. `scripts/bench_gate.py:31`, `.github/workflows/ci.yml:398`.
- Gate suites must not hold absolute wall-clock assertions; `FastapiSearchUnder5ms` failed S12's gate twice as the sole failure. Re-run in isolation, then pre/post A/B interleaved, never batched. `.claude/rules/test-iteration-discipline.md` rules 6 and 6a.
- Goldens are the only oracle since Go parity was removed (30da5b6). A repo-wide output change must grep `tests/integration/goldens/` and re-pin in the same commit; run `lci_integration_suite` (~53 s) before the full gate. `.claude/rules/test-iteration-discipline.md` rules 4-5.
- Goldens embed absolute repo-root paths (`http_reindex`), so a worktree checkout fails them spuriously. `.claude/rules/worktree-isolation-and-goldens.md` rule 2.
- Any MCP schema or description edit must run `pytest benchmarks/repo-qa/tests/test_tool_surface.py`; ctest cannot see the snapshot. `.claude/rules/test-iteration-discipline.md` rule 7.
- `prep_real.sh` points at `/home/beagle/work/core/lci` which no longer exists; reference-port rule 2 (drive the Go binary) is unexecutable. Memory `go-reference-tree-deleted`.
- Real-project suites pass-by-skip in CI (`SKIP_IF_NO_REAL_PROJECT`); only a local checkout with `real_projects/` runs them. Locally present now: go/chi, go/pocketbase, python/fastapi, typescript/trpc.
- The repo-qa harness has recorded instrument defects, all in `.claude/rules/bench-harness-oracle-independence.md`: arms were never disjoint (treatment = lci in addition to grep, `bench.py:58`, rule 12); the competitor for a tool call is the agent's native `glob`/`read`, not a sibling tool (rule 14); a free-text grader must be validated on a real recorded answer per model tier before any scorecard (rule 16, the withdrawn response-shape table); a baseline with spread 0.0 is unmeasured, not certain (rule 15); an absence or presence claim about a tool must cite its probe (rules 10, 10a); a vendor model id is verified against `opencode models` (rule 10b); a validator that mirrors its extractor cannot catch it (rules 1, 5).
- Minefield tiers are computed by lci (`profile_repos.py`), so the difficulty axis shares a mechanism with the subject. Treat tier assignment as a hypothesis, not a control.
- Program-level results: tiers 0-2 dead parity (base 0.96 vs lci 0.96); tier 3 +0.05 facts driven by completion-rate insurance on weak models; lci costs 1.5-2.5x tokens; discovery sweep found no beachhead and falsified the registered mechanism; every description rewrite lost to native tools. `ANALYSIS-discovery.md`, `ANALYSIS-discovery-sweep.md`, `ANALYSIS-toolcalling-descriptions.md`.
- lci aims to cut agent context spend versus grep-style search; no committed measurement backs a specific percentage. Repo-qa tiers instead show lci costing 1.5-2.5x agent tokens (`ANALYSIS-discovery.md`).
- "Sub-millisecond" (`README.md:3`, `docs/src/content/docs/mcp-server.mdx:141`) is backed only by in-process search benchmarks (`BM_SearchSmallIndex` 12.6 us, `BM_SearchMediumIndex` 13.2 us in the baseline) and the 10 ms real-project guard; no end-to-end CLI or MCP round-trip number is pinned.
- Dogfood gap: `lci def SKIP_IF_NO_REAL_PROJECT` finds nothing; preprocessor macros are not indexed as symbols.
- Findings from the 2026-09-08 mapping (fixed or filed): see `docs/reviews/2026-09-08-skill-mapping-findings.md`.

## Probe recipes

```sh
LCI=build/release/src/lci; R=$(git rev-parse --show-toplevel)
# perf gate, same shape as CI
cmake --build $R/build/release --parallel --target lci_benchmarks
$R/build/release/tests/lci_benchmarks --benchmark_filter=-RealProject --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true --benchmark_min_time=0.1s \
  --benchmark_out=$R/.work/bench.json --benchmark_out_format=json
python3 $R/scripts/bench_gate.py --baseline $R/tests/benchmarks/baseline/linux-x64.json --current $R/.work/bench.json
# real-project benches and latency tests (need real_projects/)
$R/build/release/tests/lci_benchmarks --benchmark_filter=RealProject
$R/build/release/tests/lci_real_project_tests --gtest_filter='RealProjectSearchLatencyTest.*'
# oracles
ctest --test-dir $R/build/release -R lci_integration_suite --output-on-failure
$R/build/release/tests/lci_tests --gtest_filter='SearchRgDifferentialTest.*'
# raw latency of the product surfaces (server warm)
$LCI search "Depends" -r $R/real_projects/python/fastapi --json | head -c 300
$LCI status -r $R/real_projects/python/fastapi
# agent-level bench
cd $R/benchmarks/repo-qa && scripts/bench.py run --repos chi --models godsv4pro --variants base,lci --difficulties easy --reps 1 --out results/smoke-x
scripts/judge.py --dir results/smoke-x --skip-llm && scripts/report.py --dir results/smoke-x
/usr/bin/python3 -m pytest $R/benchmarks/repo-qa/tests -q
```

## Product comparison

Field, by axis they compete on. All competitor facts below are `(unverified, from model knowledge)` unless a probe command is given.

| Product | Competes on | Independent probe |
|---|---|---|
| ripgrep | literal/regex search latency, recall | `rg --fixed-strings -n <p> <root>`; the repo's own oracle in `tests/search_rg_differential_test.cpp` |
| Zoekt | trigram index search at scale, ranking | run `zoekt-index` + `zoekt` on the same pinned corpus |
| Sourcegraph / SCIP | definitions, references, cross-repo nav | SCIP index dump vs `lci def/refs` on the same symbol list |
| universal-ctags | symbol extraction breadth per language | `ctags -R --output-format=json` symbol count vs `lci symbols` |
| LSP servers (gopls, pyright, tsserver) | precise def/refs/callers | `gopls_oracle.py` already builds keys for Go corpora |
| Aider repo-map | token-budgeted repo summary for an agent | compare tokens and coverage of `code_insight overview` vs the repo map |
| Cursor codebase indexing | embedding retrieval inside an editor | closed; agent-level A/B only |
| Serena MCP | LSP-backed MCP tools for agents | same MCP tool-calling harness, swap the server |
| GitHub code search | hosted indexed search | hosted; latency comparison is not like-for-like |

lci's claims and how to check them:

| Claim | Where made | Measure | Existing numbers |
|---|---|---|---|
| sub-millisecond search | `README.md:3`, `mcp-server.mdx:141` | `lci_benchmarks --benchmark_filter=Search`; for end-to-end, time `lci search --json` against a warm server | `tests/benchmarks/baseline/linux-x64.json` (12.6-13.2 us in-process); 10 ms guard in `real_project_performance_test.cpp` |
| context reduction vs grep | `README.md:3`, `handlers_analysis.cpp:2320` | tokens of lci payload vs `rg` payload for the same question set | no committed measurement; tier results show lci costs 1.5-2.5x agent tokens (`ANALYSIS-discovery.md`) |
| >1.5x regression gate | `README.md:187` | `bench_gate.py` | baseline json |
| helps agents on treacherous repos | `benchmarks/repo-qa/README.md` | `bench.py --tier 3` | `ANALYSIS-tier3-postfix.md` +0.015 aggregate, 4/6 models |

Comparison axes:

| Axis | How lci does it | How to check the competitor | Notes |
|---|---|---|---|
| search latency | in-memory postings + trigram prefilter, RE2 verify | same query, same corpus, warm, best-of-K min; report min and median | lci has no disk persistence (`README.md:161`); include cold-start indexing time as a separate row |
| search recall/precision | literal exact; certified-absence narrowing | `rg` is the oracle for literals; for regex use `rg -e` | `search` caps at 100 hits; render `truncated` |
| symbol def/refs precision | tree-sitter + receiver-type resolution, 13 languages | LSP as oracle (`gopls_oracle.py`) | overload/stdlib-name collisions corrupt callers (memory `insight-verification-2026-08-16`) |
| tokens per answered question | MCP payload size, `--group`, slim variant | opencode `step_finish` tokens per arm (`bench.py`) | arms must be disjoint; disable native grep/glob for the treatment or state that the delta measures "in addition to" |
| agent accuracy | fact regexes + LLM judge | same banks, same models | strong models sit near ceiling below 150k LOC |
| index build cost | `BM_IndexingThroughput`, `BM_RealProjectIndex*` | competitor's index command wall and RSS | memory work in `ref-tracker-memory-refactor` |

Honest gaps vs the field: no disk persistence, re-index on every server start; hit cap 100 with no pagination; no cross-repo index; no embeddings or semantic ranking beyond token/relevance; Go parity oracle gone, so language coverage is validated only by goldens and local real-project runs; agent-level wins so far are reliability on weak models, not accuracy.

### Run a comparison against product X

1. Pick one axis from the table and write the decision it informs. Load `design-unbiased-benchmarks` and follow its manifest schema; validate with its `scripts/validate_manifest.py`.
2. Sweep broad and cheap first (memory `bench-discover-then-beachhead`): many cells, weak models, easy-mode tasks where a difference should be obvious. Do not open with the elaborate anti-memorisation instrument.
3. Pre-register per task family: `lci wins` / `parity` / `X wins` with rationale, plus control families predicted parity, committed before any run (`discovery/PREDICTIONS.md` is the template). Failed predictions are the deliverable; ship them as prominently as wins.
4. Choose an oracle that shares no mechanism with lci or X: `rg --fixed-strings` for literal search, gopls/pyright/tsserver for def/refs/callers, behaviour tests for edits. Prove it accepts a known-good case and rejects a plausible wrong one.
5. Pick the corpus from `benchmarks/repo-qa/exploration/corpora.json` pinned commits (pocketbase d438c6a, scikit-learn 0885712, next.js 9753217) or a `real_projects/` submodule; record commit, forge version, seed. Never commit `.work/` corpora.
6. Build disjoint arms. For tool-level: call lci and X directly, render both into one graded syntax (`discovery/runner/rendering.py` pattern). For agent-level: treatment config must disable the baseline's mechanism; pre-flight one cell per arm and read the tool-call trace for the forbidden tool.
7. Vendor models in this order: `opencode-go/*`, then zen, then cline, `baseten/*` last (memory `vendor-model-provider-order`). Verify every id with `opencode models`; a hang with an empty event stream is `provider_unserved`, not a result.
8. Measure per cell: tokens in/out (allowlisted counters, never "sum every numeric key"), wall latency (min and median, interleaved arms), precision and recall against the oracle, completion rate. Minimum 3 reps; report spread; apply a minimum-successes gate so a floor-pinned baseline yields `no_effect`.
9. Report with `audit-benchmark-evidence` `references/report-template.md`; score every rubric gate; state the claim boundary ("lci wins where semantic navigation is mechanically required"), never "lci beats X generally".
10. Commit the manifest, predictions, raw ledgers, and an `ANALYSIS-<name>.md` beside the existing ones; cite the immutable revision and this skill's probes.

## Related skills

lci-feature-map, lci-search, lci-symbol-navigation, lci-get-context, lci-parsing-languages, lci-code-insight, lci-side-effects, lci-git-analysis, lci-indexing-pipeline, lci-server-lifecycle, lci-mcp-server, lci-config, lci-ops-diagnostics, design-unbiased-benchmarks, audit-benchmark-evidence
