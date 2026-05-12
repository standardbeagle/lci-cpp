# MASK_AUDIT.md

**Auto-generated** by `python3 tests/parity/scripts/mask_audit.py`.
Run after editing any descriptor and check in the result.

Coverage: 75 parity descriptors.

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
| `cli/search/assembly-rejected` | 1 | ✓ | 0 | 0 | 0 | 0 | 1 |
| `cli/search/basic` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/search/case-insensitive` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/compact` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/search/enhanced-rejected` | 1 | ✓ | 0 | 0 | 0 | 0 | 1 |
| `cli/search/grep` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/search/json` | 2 |   | 0 | 0 | 0 | 2 | 0 |
| `cli/search/no-results` | 0 | ✓ | 0 | 0 | 0 | 0 | 0 |
| `cli/search/regex` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `cli/status/json` | 8 | ✓ | 3 | 4 | 0 | 1 | 0 |
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
| `http/browse-file` | 10 |   | 8 | 0 | 0 | 2 | 0 |
| `http/definition` | 8 |   | 8 | 0 | 0 | 0 | 0 |
| `http/fileinfo` | 23 |   | 7 | 4 | 12 | 0 | 0 |
| `http/git-analyze` | 14 | ✓ | 10 | 0 | 4 | 0 | 0 |
| `http/inspect-symbol` | 8 |   | 8 | 0 | 0 | 0 | 0 |
| `http/list-symbols` | 9 |   | 8 | 0 | 0 | 1 | 0 |
| `http/ping` | 10 |   | 10 | 0 | 0 | 0 | 0 |
| `http/references` | 8 |   | 8 | 0 | 0 | 0 | 0 |
| `http/reindex` | 8 |   | 8 | 0 | 0 | 0 | 0 |
| `http/search` | 10 |   | 8 | 1 | 0 | 1 | 0 |
| `http/stats` | 19 |   | 10 | 6 | 1 | 2 | 0 |
| `http/status` | 10 | ✓ | 8 | 1 | 0 | 1 | 0 |
| `http/tree` | 9 | ✓ | 8 | 1 | 0 | 0 | 0 |
| `index/lci-cpp-repo` | 22 | ✓ | 8 | 1 | 7 | 6 | 0 |
| `index/lci-go-repo` | 22 | ✓ | 8 | 1 | 7 | 6 | 0 |
| `index/synthetic-multilang` | 22 | ✓ | 8 | 1 | 7 | 6 | 0 |
| `mcp/browse_file/basic` | 4 |   | 4 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/basic` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/mode-git_analyze` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/mode-git_hotspots` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/mode-statistics` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `mcp/code_insight/mode-structure` | 5 | ✓ | 4 | 0 | 1 | 0 | 0 |
| `mcp/code_insight/mode-unified` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `mcp/context_manifest/basic` | 4 |   | 4 | 0 | 0 | 0 | 0 |
| `mcp/debug_info/basic` | 4 |   | 4 | 0 | 0 | 0 | 0 |
| `mcp/find_files/basic` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `mcp/get_context/basic` | 5 | ✓ | 4 | 0 | 1 | 0 | 0 |
| `mcp/git_analysis/basic` | 5 | ✓ | 4 | 0 | 1 | 0 | 0 |
| `mcp/grep/basic` | 4 |   | 4 | 0 | 0 | 0 | 0 |
| `mcp/index_stats/basic` | 5 | ✓ | 4 | 0 | 1 | 0 | 0 |
| `mcp/index_stats/mode-references` | 5 | ✓ | 4 | 0 | 1 | 0 | 0 |
| `mcp/index_stats/mode-symbols` | 5 | ✓ | 4 | 0 | 1 | 0 | 0 |
| `mcp/index_stats/mode-types` | 5 | ✓ | 4 | 0 | 1 | 0 | 0 |
| `mcp/index_stats/wait-ready` | 5 | ✓ | 4 | 0 | 1 | 0 | 0 |
| `mcp/info/basic` | 4 |   | 4 | 0 | 0 | 0 | 0 |
| `mcp/inspect_symbol/basic` | 4 |   | 4 | 0 | 0 | 0 | 0 |
| `mcp/list_symbols/basic` | 4 |   | 4 | 0 | 0 | 0 | 0 |
| `mcp/search/basic` | 4 |   | 4 | 0 | 0 | 0 | 0 |
| `mcp/search_definitions/basic` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `mcp/semantic_annotations/basic` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `mcp/side_effects/basic` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `mcp/tree/basic` | 4 | ✓ | 4 | 0 | 0 | 0 | 0 |
| `probes/deps` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `probes/export` | 0 |   | 0 | 0 | 0 | 0 | 0 |
| `probes/graph` | 0 |   | 0 | 0 | 0 | 0 | 0 |

**Totals:** 342 masks across 75 descriptors. a=242, b=21, c=47, d=30, ?=2.

## Unclassified fields needing manual review

- cli/search/assembly-rejected: _rationale
- cli/search/enhanced-rejected: _rationale

## Descriptors with masks but no `_rationale`

- `cli/search/json` (2 masks)
- `http/browse-file` (10 masks)
- `http/definition` (8 masks)
- `http/fileinfo` (23 masks)
- `http/inspect-symbol` (8 masks)
- `http/list-symbols` (9 masks)
- `http/ping` (10 masks)
- `http/references` (8 masks)
- `http/reindex` (8 masks)
- `http/search` (10 masks)
- `http/stats` (19 masks)
- `mcp/browse_file/basic` (4 masks)
- `mcp/context_manifest/basic` (4 masks)
- `mcp/debug_info/basic` (4 masks)
- `mcp/grep/basic` (4 masks)
- `mcp/info/basic` (4 masks)
- `mcp/inspect_symbol/basic` (4 masks)
- `mcp/list_symbols/basic` (4 masks)
- `mcp/search/basic` (4 masks)
