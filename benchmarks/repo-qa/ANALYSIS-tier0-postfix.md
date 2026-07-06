# Tier-0 re-run after the find_files / search-output fixes

Re-ran tier 0 (chi + sinatra, base + lci) against the **fixed** build-tree binary
(`build/src/lci`, commits `3abebc2` + `0895aae` + `88dd30e`) into
`results/tier0-postfix2`. All prior tier-0 dirs (`results/tier0`,
`results/tier0-postslim`) ran the crippled binary (find_files 100 % dead, test/ui
files unindexed) and are invalid as accuracy baselines. Facts scored with
`judge.py --skip-llm` (judge-independent). 108 runs, 54 of them lci-variant.

## Provider reality (2026-07-06)

Both models the old baseline used are **dead right now**:

- `haiku45` (github-copilot) — 5 min, zero completions. Copilot plan throttle
  (same failure the config header documents). **Dropped.**
- `gpt5mini` (openrouter) — **0/18 ok in both variants**, every run
  `provider_error` at ~7 s, `tokens=0`. **Dropped from all tables below.**

The runnable fleet was the user-preferred opencode-go accounts: `godsv4pro`
(deepseek-v4-pro) and `gokimi27` (kimi-k2.7-code). Because neither old-baseline
model runs today, there is **no same-model pre/post facts delta** — the
binary-fix before/after is shown structurally (find_files went from 100 % dead
to returning results; new output surfaces verified by direct MCP probe) rather
than as an accuracy line.

## Headline (post-fix, `results/tier0-postfix2`)

| model | variant | n | facts | tokens (avg) | wall s | tool calls | lci calls | ok |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| godsv4pro | base | 18 | 0.888 | 42,292 | 67.2 | 2.7 | 0.0 | 18 |
| godsv4pro | **lci** | 18 | **0.896** | 118,826 | **41.3** | 7.6 | 3.9 | 18 |
| gokimi27 | base | 18 | 0.910 | 96,148 | 51.6 | 6.3 | 0.0 | 18 |
| gokimi27 | **lci** | 18 | 0.851 | 130,783 | 53.8 | 7.4 | 3.9 | 18 |
| gpt5mini | base/lci | 36 | — | 0 | 7.7 | 0 | 0 | **0** (dead) |

Reference only (crippled binary, now-dead models): `tier0-postslim` gpt5mini-lci
0.866, haiku45-lci 0.822; `tier0` gpt5mini-lci 0.819.

**Read of the numbers.** Facts are at parity: godsv4pro-lci +0.008 vs its base,
gokimi27-lci -0.059. The gokimi27 dip is **single-rep answer-quality variance,
not tooling** — its four regressed questions (chi-e3, chi-h1, chi-m3, sin-m3) all
show clean, non-empty tool traces; the model simply wrote a thinner answer after
reading the right file. The real signal is elsewhere:

- **LCI made godsv4pro faster** (67.2 s -> 41.3 s wall) by replacing broad reads
  with targeted search — the intended effect.
- **find_files is alive.** 9/54 runs called it, **9/11 calls returned content**
  (`output_chars >= 80`); pre-fix every find_files call came back empty and the
  model fell to `read`.
- **New include params are adopted.** Across 109 searches, `path=` used **35x**,
  `filter=` **16x**, with **9** broad->narrow refinements. bash-after-lci fallback
  fired in only **1/54** runs — LCI answered.

## Trace comb (54 lci runs) — findings ranked by frequency

| # | finding | count | verdict |
|---|---|---:|---|
| 1 | `read` after search (incl. paged re-reads of same file) | 123 reads / 58 same-file | expected; search locates, model reads to quote |
| 2 | `search path=` used | 35 | POSITIVE — narrowing adopted |
| 3 | `search filter=` used | 16 | POSITIVE |
| 4 | broad->narrow search refinement | 9 | POSITIVE |
| 5 | find_files calls returning content | 9/11 | POSITIVE — was 0 pre-fix |
| 6 | **search param/validation errors** | 3 | all `paths` (plural) — unrecoverable, see below |
| 7 | near-empty nav output (<80 ch) | find_files 2, search 1, list_symbols 1 | 2 find_files = the `**/` bug + a hallucinated filename |
| 8 | bash fallback after an lci call | 1/54 | POSITIVE — LCI rarely insufficient |

## Ranked tool / output improvements (with evidence)

### 1. HIGH — `find_files` `**/` recursive globs silently return nothing
LLMs default to `**/name`. The engine drops leading-`**/` + concrete filename:

```
**/mux.go                 -> 0     (mux.go -> 1)
**/chain.go               -> 0     (chain.go -> 1, *chain.go -> 1)
**/middleware/compress.go -> 0     (middleware/compress.go -> 1)
**/*.go                   -> 64    (*.go -> 74  — even the wildcard form under-matches)
_examples/**/*.go         -> 19    (** in the MIDDLE works)
```

Live hit — godsv4pro chi-e3:
```
lci_find_files {"pattern":"**/chain.go"}  -> output_chars=56  (empty)
lci_read       chain.go                    (fell back)
```
Fix: treat a leading `**/` as "zero or more path segments" so `**/mux.go`
matches `mux.go` at root, and make `**/*.go` == `*.go`. This is a correctness
bug, not cosmetics — it turns the most natural "find this file anywhere" query
into a silent miss.

### 2. HIGH — `search` unknown-param error omits the allow-list -> unrecoverable thrash
`find_files` / `get_context` / `browse_file` answer an unknown param with the
allowed set; **`search` does not**:

```
search  {"pattern":"...","paths":"lib/sinatra/base.rb"}
  -> {"error":{...,"type":"multiple_validation_errors"},
      "validation_errors":[{"error_message":
        "validation failed for additional property 'paths': ... false-schema"}]}
```
No "Allowed: pattern, path, filter, ..." hint -> the model can't self-correct.
godsv4pro sin-h3 issued `paths` **three times**, never recovered, fell to grep:
**14 tool calls, 59 s, facts 0.5** (its worst run). Fix: give `search` the same
`unknown parameter(s): X. Allowed: ...` message the other tools already emit.

### 3. MED-HIGH — parameter names are inconsistent across tools
One scoping concept, four spellings — models transfer the wrong one:

| tool | file/dir scope | add-ons |
|---|---|---|
| search | `path=` | `include=` (values: object_ids,breadcrumbs,refs,safety,deps) |
| find_files | `directory=` (+ `pattern=`) | `filter=` |
| browse_file | `file=` (NOT `path=`) | `include=` |
| get_context | — | `include_call_hierarchy=` ... (booleans, NOT `include=`) |

Observed errors from this: `search paths=` (#2), and by direct probe
`browse_file path=...` -> `unknown parameter(s): path. Allowed: file, ...`,
`get_context include="callers,callees"` -> `unknown parameter include`. Fix:
accept `path` as an alias on browse_file/find_files, and either accept a
plural `paths`/`path` on search or, at minimum, name it in the error.

### 4. MED — `get_context` is thin by default and mute on qualified names
`get_context name=Mux` returns only the definition — no callers/callees — despite
AGENTS.md advertising "call hierarchy, callers/callees". The hierarchy only
appears with `include_call_hierarchy=true`, and the natural `include=` errors
out (#3). On Ruby qualified names it nearly empties:
```
get_context {"name":"Sinatra::Error","include_all_references":true} -> 138 chars
inspect_symbol {"name":"process_route","include":"signature,doc"}   -> 176 chars
```
Note the win it should lean on: `search` already ships caller counts inline
(`"callers":258` on chi's `Handler` type, `78` on `Router`) — chokepoint
questions are answerable from one search. get_context should surface a caller
**count** in its default payload the same way.

### 5. LOW — `search` success payloads trip an "error" substring heuristic
Queries *about* errors (`pattern:"def error"`) get flagged as tool errors by the
harness `'"error"' in out[:200]` check (2 of the 5 "errors" this run were false
positives). Minor, but LCI could avoid bare `"error"` keys inside successful
result objects to keep signal clean.

### 6. LOW — find_files default `showing=50`
74-file corpora truncate at 50 (`truncated:true` is present, `total_matches`
is now true — good). A model wanting the full list must pass `max`. Consider a
higher default or echoing `pass max=N for all` in the truncated payload.

## What the fix got right (verified by direct MCP probe)
- `dirs` histogram, true `total_matches` + `truncated`, inline `callers` counts,
  `path=`/`filter=` scoping, and `similar_symbols` + actionable `hint` on empty
  (`Recoverr` -> `[Recoverer, recover]`) all work.
- Bad object_id fails fast into an `errors[]` array (no zeroed output).

## Verdict
The fix is real: find_files returns results, the new scoping/telemetry surfaces
are adopted, and LCI cut wall time where it was allowed to. Accuracy is at
parity on this cheap tier (as expected). The next tool wins are correctness/UX,
not ranking: fix the `**/` glob (#1) and the allow-list-less search error (#2)
first — both produced observed empty results / unrecoverable thrash this run.
