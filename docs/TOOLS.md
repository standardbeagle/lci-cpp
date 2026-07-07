# MCP Tools Reference

`lci mcp` runs a Model Context Protocol server over stdio. It exposes **14
tools** to AI assistants. This document describes each tool exactly as
implemented.

## Conventions

- **Invocation**: standard MCP `tools/call` with a `name` and an `arguments`
  object. Every tool returns its payload as the text of a single
  `content[]` entry.
- **Output formats**: most tools emit **JSON**. The analysis tools
  (`code_insight`) emit **LCF** (LCI Compact Format) — a token-dense,
  section-based text format (`LCF/1.0\nmode=...\ntier=...\ntokens=...\n---\n`
  followed by `== SECTION ==` blocks).
- **Object IDs**: `search`, `list_symbols`, `inspect_symbol`, `code_insight`,
  etc. emit short encoded `object_id` strings (e.g. `VE`, `tG`). Feed them to
  `get_context {"id": "..."}` (comma-separated for several) to drill in.
- **Errors**: failures return a structured error result
  `{"operation": "<tool>", "message": "...", "success": false}` with the
  result flagged as an error. Tools **fail fast** — no fake/zeroed payloads.
- **Determinism**: all list output is sorted with total-order tiebreakers
  (no hash-iteration order in user-visible output).

## Tool index

| Tool | Output | Purpose |
|------|--------|---------|
| [`info`](#info) | JSON | Per-tool help + server version |
| [`search`](#search) | JSON | Semantic + literal + regex content search |
| [`find_files`](#find_files) | JSON | File-path search (fuzzy / glob) |
| [`get_context`](#get_context) | JSON | Detailed context for object IDs or names, with call hierarchy + purity |
| [`context`](#context) | JSON | Save / load code-context manifests for agent handoff |
| [`list_symbols`](#list_symbols) | JSON | Enumerate + filter symbols (the "ls" for code) |
| [`inspect_symbol`](#inspect_symbol) | JSON | Deep inspect one symbol |
| [`browse_file`](#browse_file) | JSON | Symbol outline for a file |
| [`index_stats`](#index_stats) | JSON | Index status + health |
| [`debug_info`](#debug_info) | JSON | Deep diagnostics |
| [`semantic_annotations`](#semantic_annotations) | JSON | Query `@lci:` labels / categories |
| [`side_effects`](#side_effects) | JSON | Function purity + side-effect analysis |
| [`code_insight`](#code_insight) | LCF | Codebase intelligence (overview / stats / structure / git) |
| [`git_analysis`](#git_analysis) | JSON | Git change analysis (duplicates / naming / metrics) |

---

## info

Help for a specific tool, or server version. Case-insensitive `tool` arg.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `tool` | string | (empty) | `version`, `search`, `get_context`, `find_files`, `index_stats`, or omitted for an overview |

- `tool=version` → `{name, server_name, server_version, mcp_version, capabilities}`.
- `tool=<name>` → `{name, description, parameters, example(s)}`.
- omitted → `{available_tools[], quick_start, server, tagline, why_use_lci[]}`.

No errors; unknown tool names fall back to the overview.

---

## search

Sub-millisecond in-memory content search (not file paths). Multi-layer:
literal match → optional synonym expansion → regex fallback. Results carry
the enclosing symbol's metadata.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `pattern` | string | — | Search pattern (required unless `patterns` set). Literal, or regex with `flags=rx`. Case-insensitive by default. |
| `patterns` | string | (empty) | Comma-separated patterns for OR search; overrides semantic expansion. |
| `max` | integer | 50 | Max results, clamped [1, 100]. |
| `output` | string | `line` | `line` (1 ctx line), `ctx` (5), `ctx:N`, `full` (10), `files`, `count`. |
| `flags` | string | (empty) | Comma list: `cs` case-sensitive, `wb` word-boundary, `nt` no-tests, `nc` no-comments, `iv` invert, `rx` regex. |
| `include` | string | (empty) | Add-ons for strong matches (score ≥ 0.5): `breadcrumbs`, `refs`, `object_ids`/`ids`, `safety`, `deps`. Unknown token → error. |
| `symbol_types` | string | (empty) | Comma list filter, e.g. `function,class`. |
| `max_per_file` | integer | 0 | Cap matches per file (0 = no cap). |
| `semantic` | boolean | true | Synonym fan-out on multi-word patterns when `patterns` empty. |
| `languages` | array | (empty) | Language filter (aliases ok), e.g. `["go","python"]`. |
| `filter` | string | (empty) | Exclude-pattern for files. |

**Output** (default): `{results[], total_matches, showing, max_results}`. Each
result: `result_id`, `file`, `line`, `column`, `match`, `score`, `object_id`,
`symbol_name`, `symbol_type`, `is_exported`, optional `references`
{`incoming_count`,`outgoing_count`}, optional `breadcrumbs[]`, optional
`context_lines[]`. `output=files` → `{files[], total_matches, unique_files}`.
`output=count` → `{total_matches, unique_files, counts{}}`.

**Notable**: case-insensitive by default; regex auto-fallback (0.7 score
penalty) when a pattern *looks* regex-y; `refs`/`breadcrumbs` only attach to
matches with normalized score ≥ 0.5; enclosing symbol resolved O(1) via
`get_symbol_at_line` (stable empty fields when a match is outside any symbol).

**Errors**: missing `pattern` and `patterns`; unrecognized `include` token;
index unavailable.

---

## find_files

File-path search (like `find`/`fd`): fuzzy, glob, type filter.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `pattern` | string | — | Required. Path pattern; spaces → multi-word boost. |
| `max` | integer | 50 | Clamped [1, 200]. |
| `filter` | string | (empty) | Language name (`go`, `py`) or glob (`*.ts`). |
| `flags` | string | (empty) | `ci` case-insensitive, `exact` exact-path only. |
| `include_hidden` | boolean | false | Include dot-prefixed components. |
| `directory` | string | (empty) | Restrict to a path prefix. |

**Output**: `{results[], total_matches, pattern}`. Each result: `path`, `score`,
`match_type` (`exact`, `exact_filename`, `exact_filename_noext`, `substring`,
`fuzzy`, `path_component`, `word_substring`), `file_id`.

**Notable**: scoring hierarchy exact → filename → substring → fuzzy
(Levenshtein); multi-word patterns add +0.15/word (cap +0.5); deterministic
sort score desc, then `file_id`, then path.

**Errors**: missing `pattern`; no files in index; index unavailable.

---

## get_context

Detailed context for object IDs (from `search`) or a symbol name, with call
hierarchy and purity. `id` and `name` are mutually exclusive.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `id` | string | (empty) | Comma-separated object IDs. Aliases: `symbol_id`, `object_id`, `object_ids`, `oid`. Accepts `oid=VE,tG` form. |
| `name` | string | (empty) | Symbol name (enables name path + call hierarchy). Needs non-empty `mode`. |
| `mode` | string | (empty) | `full` (depth 5, ai text), `quick` (depth 2, sections relationships+structure), `relationships`, `semantic`, `usage`, `variables`. |
| `include_call_hierarchy` | boolean | false | Callers/callees/call_tree (name path only). |
| `max_depth` | int | 1 (or mode preset) | Call-tree depth, clamped [1, 10]. |
| `include_sections` / `exclude_sections` | array | — | `relationships`/`callers` honored; others omitted (engine not ported). |
| `symbol` + `path` | string | (empty) | Auto-search path: if both set with no `id`, returns a workflow hint, not context. |

**Output** (id path): `{count, contexts[], errors[]}`. Each context:
`file_path`, `line`, `object_id`, `symbol_name`, `symbol_type`, `is_exported`,
`signature` (if any), `definition`, `context[]`, and `purity`
{`is_pure`, `purity_score`, `confidence`, `local_effects[]`,
`transitive_effects[]`, `reasons[]`} for functions/methods. Name+mode path
adds `callers[]`, `callees[]`, `call_tree` (cycle-marked).

**Errors**: neither `id` nor `name`; both `id` and `name`; per-id resolution
errors collected in `errors[]`; index unavailable.

---

## context

Save / load code-context manifests for agent handoff. Compact (2–5 KB)
manifests of references (files, symbols, line ranges, roles, expansion
directives) that hydrate back into full source + call graph + purity.

| Param | Type | Req | Description |
|-------|------|-----|-------------|
| `operation` | string | yes | `save` or `load`. |
| `refs` | array | save | Each: `f` (file, req), `s` (symbol), `l`{`s`,`e`} (1-indexed lines), `role`, `n` (note), `x` (expansion directives). |
| `task` | string | no | Free-form task text, persisted. |
| `to_file` / `to_string` | string / bool | save | Write manifest to file, or return JSON string. One required. |
| `append` | bool | no | Merge with existing manifest at `to_file`. |
| `from_file` / `from_string` | string | load | Read manifest. One required. |
| `format` | string | no | Load output: `full` (source), `signatures`, `outline`. Default `full`. |
| `filter` / `exclude` | array | no | Include / exclude refs by role, before expansion. |
| `max_tokens` | int | no | Token budget; hydration stops + sets `truncated`. 0 = unlimited. |

**Manifest** (compact Go-shape keys): `t` task, `c` created (RFC3339Nano),
`v` version, `p` project_root, `r` refs[], `s` stats {`rc`,`tl`,`fc`,`rb`}.
Files written atomically (temp + rename). Verbose key aliases accepted on load
only (warn once on stderr).

**save output**: `{saved | manifest, stats{ref_count,file_count,total_lines}, ref_count, file_count}`.
**load output**: `{task, refs[] (with source, symbol_type, signature, is_exported, is_external), stats{refs_loaded,symbols_hydrated,tokens_approx,expansions_applied,truncated}, warnings[]}`.

Expansion directives in `x`: `callees[:N]`, `callers`, `implementations`,
`tests`, `pattern` — applied within the remaining token budget.

**Errors**: empty `refs`; no `to_file`/`to_string`; no `from_file`/`from_string`;
file not found; invalid JSON / manifest; index unavailable; invalid `operation`.

---

## list_symbols

Enumerate + filter symbols across the index. The "ls" for code.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `kind` | string | — | **Required.** Comma list: `func`/`fn`, `type`, `struct`, `interface`/`iface`, `method`, `class`/`cls`, `enum`, `variable`/`var`, `constant`/`const`, `field`, `property`, `module`, `namespace`, `constructor`, `trait`, or `all`. |
| `file` | string | (empty) | Glob path filter (exact / basename / suffix). |
| `exported` | boolean | (omit) | true = exported only; false = unexported only. |
| `name` | string | (empty) | Case-insensitive substring on name. |
| `receiver` | string | (empty) | Filter methods by receiver type. |
| `min_complexity` / `max_complexity` | int | — | Cyclomatic complexity bounds. |
| `min_params` / `max_params` | int | — | Parameter-count bounds. |
| `flags` | string | (empty) | `async`, `variadic`, `generator`, `method` (AND). |
| `sort` | string | `name` | `name`, `complexity`, `refs`, `line`, `params` (file/line tiebreak). |
| `max` | integer | 50 | Clamped [1, 500]. |
| `offset` | integer | 0 | Pagination offset. |
| `include` | string | `signature,ids` | `signature`, `doc`, `refs`, `callers`, `callees`, `scope`, `ids`, `all`. |

**Output**: `{symbols[], total, showing, has_more}`. Each symbol always has
`name`, `type`, `file`, `line`, `is_exported`; conditionally `object_id`,
`signature`, `doc_comment`, `complexity`, `parameter_count`, `receiver_type`,
`incoming_refs`/`outgoing_refs`, `callers[]`, `callees[]`, `scope_chain[]`.

**Errors**: missing `kind`; index unavailable.

---

## inspect_symbol

Deep inspection of one symbol by name or ID.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | (empty) | Exact name (may be ambiguous). One of name/id required. |
| `id` | string | (empty) | Encoded object ID(s), comma-separated. |
| `file` | string | (empty) | Glob disambiguator. |
| `type` | string | (empty) | Symbol-type disambiguator. |
| `include` | string | `all` | `signature`, `doc`, `callers`, `callees`, `type_hierarchy`, `scope`, `refs`, `annotations`, `flags`, `all`. |
| `max_depth` | integer | 3 | Type-hierarchy depth, clamped [1, 10]. |

**Output**: `{symbols[], count}`. Each symbol: `name`, `object_id`, `type`,
`file`, `line`, `is_exported`, `complexity`, `parameter_count`; conditionally
`signature`, `doc_comment`, `function_flags[]`, `variable_flags[]`,
`callers[]`, `callees[]`, `type_hierarchy{implements,implemented_by,extends,extended_by}`,
`scope_chain[]`, `incoming_refs`/`outgoing_refs`, `annotations[]`,
`receiver_type`.

**Errors**: neither `name` nor `id`; index unavailable.

---

## browse_file

Symbol outline for a single file, with optional stats.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `file` / `file_id` | string / int | — | One required (file path glob, or numeric id). |
| `kind` | string | (empty) | Symbol-kind filter (as `list_symbols`). |
| `exported` | boolean | (omit) | Export filter. |
| `sort` | string | `line` | `line`, `name`, `type`, `complexity`, `refs`. |
| `max` | integer | 100 | Clamped [1, 1000]. |
| `include` | string | `signature,ids` | As `list_symbols`. |
| `show_stats` | boolean | false | Append file-level stats. |

**Output**: `{file{path,file_id,language}, symbols[], total, stats?}`. `stats`:
`symbol_count`, `function_count`, `type_count`, `exported_count`,
`max_complexity`, `avg_complexity` (computed over the whole file, not the
filtered subset).

**Errors**: neither `file` nor `file_id`; file not found (JSON with `hint`);
index unavailable. (`show_imports` is accepted but currently unused.)

---

## index_stats

Index status + health.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `mode` | string | `summary` | `summary`, `detailed`, `progress`, `health`. |
| `include_memory` | boolean | false | Append `memory_usage`. |
| `include_components` | boolean | false | Force `component_health` in summary. |

- **summary** → core metrics: `file_count`, `symbol_count`, `reference_count`,
  `total_size_bytes`, `index_time_ms`, plus `timestamp`, `server_ready`,
  `status` (`ready`/`indexing`), `progress` (when indexing).
- **detailed** / **health** → adds `component_health`
  (`symbol_index_ready`, `trigram_index_ready`, `ref_tracker_ready`,
  `call_graph_populated`, `file_content_store_ready`, `reference_stats`) +
  `warnings[]` + `memory_usage`.
- **progress** → summary with an inline `progress` object during indexing.

**Errors**: index unavailable. (`include_watch_mode` accepted but unused.)

---

## debug_info

Deep diagnostics for index troubleshooting.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `mode` | string | `overview` | `overview`, `symbols`, `references`, `types`, `files`. |
| `file_id` | integer | 0 | For `files` mode. |
| `file_path` | string | (empty) | For `files` mode. |
| `max_results` | integer | 20 | Clamped [1, 100]. |
| `verbose` | boolean | false | Per-symbol detail in `files` mode. |

- **overview** → `total_files/symbols/references`, `unique_languages`,
  `avg_symbols_per_file`, `avg_refs_per_symbol`, `language_breakdown{}`,
  `type_breakdown{}`.
- **symbols** / **types** → `symbols_by_type{}`.
- **references** → `top_referenced_symbols[]` (name, type, file, incoming_refs),
  sorted desc, capped at `max_results`.
- **files** → `files_by_language{}`; with `file_id`/`file_path` adds `file_info`
  (+ `symbols[]` when `verbose`).

All responses carry `mode` + `timestamp_ms`.

**Errors**: unknown mode; index unavailable.

---

## semantic_annotations

Query symbols by `@lci:` labels / categories, including labels propagated
through the call graph.

Annotations can live inline as `@lci:` comments near symbols, or in external
JSON manifests when source files should not be edited. MCP loads these project
root manifests at startup:

- `.lci/annotations/*.json`
- `.lci/annotations.json`
- `.lci-annotations.json`
- `lci-annotations.json`

Each manifest can be a JSON array or an object with an `annotations` array:

```json
{
  "annotations": [
    {
      "file": "okhttp/src/commonJvmAndroid/kotlin/okhttp3/OkHttpClient.kt",
      "symbol": "address",
      "labels": ["api-usage", "value-flow"],
      "category": "connection-routing",
      "tags": {
        "flow": "OkHttpClient.connectionSpecs -> Address.connectionSpecs"
      }
    }
  ]
}
```

Entries resolve against indexed symbols by `file` plus `symbol` and/or `line`.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `label` | string | (empty) | Label to search (one of label/category required). |
| `category` | string | (empty) | Category to search. |
| `min_strength` | number | 0.0 | Min propagated-label strength. |
| `include_direct` | boolean | true* | Include direct `@lci:` annotations. |
| `include_propagated` | boolean | true* | Include call-graph-propagated labels. |
| `max_results` | integer | 100 | Clamped [1, 10000]. |

*If neither include flag is set, both default on.

**Output**: `{annotations[], total_count}`. Each annotation: `symbol_name`,
`file_id`, `symbol_id`, `file_path`, `line`, optional `direct_labels[]`,
`category`, `tags{}`, `propagated_labels[]` (`label`, `strength`, `hops`,
`source_name`, `source_file`). Category results de-duped against label results
by `symbol_id`.

**Errors**: neither `label` nor `category`.

---

## side_effects

Function purity + side-effect analysis (param/global/closure/field writes,
I/O, throws), with transitive call-graph analysis.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `mode` | string | `summary` | `symbol`, `file`, `pure`, `impure`, `category`, `summary`. |
| `symbol_name` | string | (empty) | For `symbol` mode. |
| `file_path` | string | (empty) | For `file` mode (or symbol lookup). |
| `category` | string | (empty) | For `category` mode. |
| `include_reasons` | boolean | false | Impurity reason strings. |
| `include_transitive` | boolean | false | Transitive effects from callees. |
| `include_confidence` | boolean | false | Confidence levels. |
| `max_results` | integer | 100 | Clamped [1, 10000]. |

- **symbol** → one function's purity record (categories, access patterns,
  violations, error handling).
- **file** → all functions in `file_path`.
- **pure** / **impure** → filtered list + `total_count`.
- **category** → functions exhibiting a category. Valid: `param_write`,
  `receiver_write`, `global_write`, `closure_write`, `field_write`, `io`,
  `database`/`db`, `network`/`net`, `throw`/`throws`/`panic`, `channel`/`chan`,
  `async`, `external_call`/`external`, `dynamic_call`/`dynamic`,
  `reflection`/`reflect`, `uncertain`/`unknown`.
- **summary** → `{summary{total_functions, pure_functions, impure_functions,
  purity_ratio, with_param_writes, with_global_writes, with_io_effects,
  with_throws, with_external_calls}, total_count}`. Conditional pure-counting
  (won't report all-pure on an unannotated corpus).

**Errors**: `symbol` without name/path; symbol not found; `file` without path;
`category` missing/unknown; unknown mode.

---

## code_insight

Codebase intelligence in **LCF** text. Default mode `overview`. The
session-startup workhorse.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `mode` | string | `overview` | `overview`, `detailed`, `statistics`, `unified`, `structure`, `git_analyze`, `git_hotspots`. |
| `analysis` | string | `modules` | For `detailed`: `modules`, `layers`, `features`, `terms`. |
| `scope` | string | `staged` | For `git_analyze`: `staged`, `wip`, `commit`, `range`. |
| `base_ref` / `target_ref` | string | (empty) | For `git_analyze` range. |
| `time_window` | string | `30d` | For `git_hotspots`: `7d`/`30d`/`90d`/`1y`. |
| `file_pattern` | string | (empty) | For `git_hotspots`: glob filter. |
| `max_results` | integer | 50 | Passed to the engine. |
| `languages` | array | — | Language filter (aliases). |

**Modes / sections**

- **overview** → `== SUMMARY ==` (files/symbols/dirs/depth + langs),
  `== REPOSITORY MAP ==` (module=… files=…, ≤15), `== ENTRY POINTS ==` (≤12),
  `== HEALTH ==` (score, complexity, smells, problematic_symbols, purity +
  query hint), `== VOCABULARY ==` (naming outliers + aliases), `== OBJECT IDs ==`
  hint, `== NEXT STEPS ==`.
- **statistics** → `== STATISTICS ==` (complexity avg/median + distribution,
  coupling, cohesion, quality, top-3 high_complexity, top-3 low_cohesion).
- **unified** → SUMMARY + REPOSITORY MAP + ENTRY POINTS + HEALTH + `== MODULES ==`
  + `== DEPENDENCIES ==` + STATISTICS + VOCABULARY + NEXT STEPS.
- **structure** → `== STRUCTURE ==` (dirs/files/symbols/depth, types by ext,
  code/test/config/doc categories, top dirs).
- **detailed** (`analysis=`) → `== MODULES ==` / `== LAYERS ==` /
  `== FEATURES ==` / `== TERMS ==`.
- **git_analyze** → `== GIT CHANGES ==` (scope, files_changed, added/modified/
  deleted, risk; findings duplicates/naming/metrics; top recommendation; top-5
  metrics issues by file:line). Real `git::Analyzer` data.
- **git_hotspots** → `== GIT HOTSPOTS ==` (window, files_analyzed, commits,
  hotspots, anti_patterns; top-8 churned files; top-5 collision zones). Real
  `git::FrequencyAnalyzer` data over a rolling time window (time-volatile).

> The git modes surface real data that the original Go formatter computed but
> discarded. Both **fail fast** if the project root is not a git repository.

**Notable**: all list output deterministically sorted; LCF token estimate in
the header; trailing newline stripped so payloads end on `---`.

**Errors**: invalid `mode`; invalid `detailed` analysis; non-git root (git
modes); git analysis failure.

---

## git_analysis

Git change analysis against the working set: duplicates, naming
inconsistencies, function complexity. Builds a `git::Provider` from the index
project root and runs `git::Analyzer`. Emits the canonical report shape
(shared with the HTTP `/git-analyze` endpoint).

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `scope` | string | `staged` | `staged`, `wip` (all uncommitted), `commit`, `range`. |
| `base_ref` | string | (empty) | Base ref for `commit`/`range` (e.g. `HEAD~1`, `main`). |
| `target_ref` | string | (empty) | Target ref for `range` (defaults to HEAD). |
| `focus` | array | (all) | Subset of `duplicates`, `naming`, `metrics`. |
| `similarity_threshold` | number | 0.8 | Duplicate-detection threshold (0–1). |
| `max_findings` | integer | 20 | Max findings per category. |

**Output** (JSON):

```json
{
  "summary": {
    "files_changed": 1,
    "symbols_added": 0,
    "symbols_modified": 14,
    "symbols_deleted": 0,
    "duplicates_found": 0,
    "naming_issues_found": 0,
    "metrics_issues_found": 6,
    "risk_score": 0.36,
    "top_recommendation": "Extract parts of this function into smaller helper functions"
  },
  "metrics_issues": [
    {
      "severity": "warning",
      "description": "Function 'foo' is too long (103 lines)",
      "symbol": { "name": "foo", "type": "function", "file_path": "src/x.cpp",
                  "line": 83, "end_line": 185, "complexity": 10,
                  "lines_of_code": 103, "nesting_depth": 6 },
      "issue_type": "long_function",
      "issue": "Function has 103 lines, exceeding threshold of 100",
      "suggestion": "Extract parts of this function into smaller helper functions",
      "new_metrics": { "complexity": 10, "lines_of_code": 103, "nesting_depth": 6 }
    }
  ],
  "naming_issues": [
    {
      "severity": "info",
      "description": "...",
      "new_symbol": { "name": "...", "type": "...", "file_path": "...", "line": 0 },
      "issue_type": "case_mismatch",
      "issue": "...",
      "suggestion": "...",
      "similar_names": null
    }
  ],
  "duplicates": [
    {
      "severity": "warning",
      "description": "...",
      "new_code": { "file_path": "...", "start_line": 0, "end_line": 0, "symbol_name": "..." },
      "existing_code": { "file_path": "...", "start_line": 0, "end_line": 0, "symbol_name": "..." },
      "similarity": 0.95,
      "type": "...",
      "suggestion": "..."
    }
  ],
  "metadata": {
    "base_ref": "HEAD",
    "target_ref": "WORKING",
    "scope": "wip",
    "analysis_time_ms": 74,
    "analyzed_at": "2026-06-06T12:00:00Z"
  }
}
```

`metrics_issues`, `naming_issues`, and `duplicates` are omitted when empty.
File paths are normalized relative to the project root.

**Errors**: invalid `scope`; `not a git repository: <root>` (fail-fast);
`git analyze failed`.
