# MASK_AUDIT.md

**Auto-generated** by `python3 tests/parity/scripts/mask_audit.py`.
Run after editing any descriptor and check in the result.

Coverage: 83 parity descriptors.

Bucket meanings:
- **(a) Non-determinism / RPC envelope.** Always safe to ignore.
- **(b) Intentional C++ enrichment / runtime difference.**
  Documented `_rationale` required.
- **(c) Workspace-state / content-dependent.** Varies with WIP
  or repo content under test.
- **(d) Ranking / scanner-order divergence.** Same multiset,
  different order.
- **(?) Unclassified.** Update rules or per-descriptor `_rationale`.

## Per-descriptor mask summary

| Descriptor | # | _rationale | a | b | c | d | ? |
|---|---:|:---:|---:|---:|---:|---:|---:|
| `cli/config/show` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/config/validate` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/debug/info` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/debug/validate` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/git/git-analyze` | 3 | ✓ | 2 | 0 | 1 | 0 | 0 |
| `cli/search/assembly-rejected` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/basic` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/search/case-insensitive` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/compact` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/search/enhanced-rejected` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/exclude-path` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/grep-exclude-path` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/grep` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/search/include-path` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/json` | 2 |   | 0 | 0 | 0 | 2 | 0 |
| `cli/search/no-results` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/regex-count` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/regex-files-only` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/regex-invert` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/regex-max-count` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/regex` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/word-regexp` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/status/json` | 9 | ✓ | 2 | 6 | 0 | 1 | 0 |
| `cli/status/text` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/browse-json` | 1 | ✓ | 0 | 0 | 0 | 1 | 0 |
| `cli/symbols/browse-stats-json` | 1 | ✓ | 0 | 0 | 0 | 1 | 0 |
| `cli/symbols/browse-stats` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/browse` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/def` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/inspect-json` | 1 | ✓ | 0 | 1 | 0 | 0 | 0 |
| `cli/symbols/inspect-missing-json` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/inspect` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/list` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/refs-json` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/refs` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/symbols` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/symbols/tree` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/version` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `http/browse-file` | 2 |   | 0 | 0 | 0 | 2 | 0 |
| `http/definition` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `http/git-analyze` | 12 | ✓ | 2 | 0 | 10 | 0 | 0 |
| `http/inspect-symbol` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `http/list-symbols` | 1 |   | 0 | 0 | 0 | 1 | 0 |
| `http/ping` | 2 |   | 2 | 0 | 0 | 0 | 0 |
| `http/references` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `http/reindex` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `http/search` | 2 |   | 0 | 1 | 0 | 1 | 0 |
| `http/stats` | 12 |   | 3 | 6 | 1 | 2 | 0 |
| `http/status` | 3 | ✓ | 1 | 1 | 0 | 1 | 0 |
| `http/tree` | 1 | ✓ | 0 | 1 | 0 | 0 | 0 |
| `mcp/browse_file/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/mode-git_analyze` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/mode-git_hotspots` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/mode-statistics` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/mode-structure` | 1 | ✓ | 0 | 0 | 1 | 0 | 0 |
| `mcp/code_insight/mode-unified` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/context_manifest/basic` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `mcp/context_manifest/save-compact-keys` | 1 | ✓ | 0 | 0 | 1 | 0 | 0 |
| `mcp/context_manifest/save-to_string-body-shape` | 1 | ✓ | 0 | 0 | 1 | 0 | 0 |
| `mcp/context_manifest/save-verbose-keys-rejected` | 1 | ✓ | 0 | 0 | 1 | 0 | 0 |
| `mcp/debug_info/basic` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `mcp/find_files/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/get_context/basic` | 1 | ✓ | 0 | 0 | 1 | 0 | 0 |
| `mcp/git_analysis/basic` | 1 | ✓ | 0 | 0 | 1 | 0 | 0 |
| `mcp/grep/basic` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `mcp/index_stats/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/index_stats/mode-references` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/index_stats/mode-symbols` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/index_stats/mode-types` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/index_stats/wait-ready` | 1 | ✓ | 0 | 0 | 1 | 0 | 0 |
| `mcp/info/basic` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `mcp/inspect_symbol/basic` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `mcp/list_symbols/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/search/basic` | 1 | ✓ | 0 | 0 | 1 | 0 | 0 |
| `mcp/search_definitions/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/semantic_annotations/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/side_effects/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/tools_list/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `mcp/tree/basic` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `probes/deps` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `probes/export` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `probes/graph` | 0 |   | 0 | 0 | 0 | 0 | 0 |

**Totals:** 60 masks across 83 descriptors. a=12, b=16, c=20, d=12, ?=0.

## Unclassified fields needing manual review

None. ✓

## Descriptors with masks but no `_rationale`

- `cli/search/json` (2 masks)
- `http/browse-file` (2 masks)
- `http/list-symbols` (1 masks)
- `http/ping` (2 masks)
- `http/search` (2 masks)
- `http/stats` (12 masks)
