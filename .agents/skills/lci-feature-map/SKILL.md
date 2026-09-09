---
name: lci-feature-map
description: Router for the lci feature-area skills. Use when: tracking down code for an lci feature (search, grep, def/refs/callers/tree, get_context, code_insight, side_effects, git analysis, indexing, server, MCP, config, install/update, benchmarks), asking "where does lci do X", comparing lci with another product (ripgrep, ctags, LSP, Sourcegraph, Zoekt, Cursor, Aider, Serena, SonarQube, CodeScene), or unsure which lci-* skill applies.
---

# lci feature map

lci (Lightning Code Index) is a C++ code-intelligence engine: a trigram plus symbol index over a
repo, served from a per-root daemon over a Unix socket, exposed as a CLI, an HTTP API, and an MCP
server for AI agents. This skill routes to one feature-area skill. Each area skill carries the code
map, config knobs, traps, probe recipes, and a product-comparison section for that area.

## Route by surface

| You are looking at | Skill |
|---|---|
| `lci search`, `lci grep`, MCP `search`, HTTP `/search`, trigram/postings, ranking, synonyms/stemming | `lci-search` |
| `lci def/refs/callers/tree/symbols/inspect/browse/list`, MCP `list_symbols`/`inspect_symbol`/`browse_file`/`find_files`, HTTP `/definition` `/references` `/callers` `/tree`, symbol store, reference tracker, call graph, import resolution | `lci-symbol-navigation` |
| MCP `get_context`, MCP `context` manifests, object IDs, LCF output, token budget, context reduction claim | `lci-get-context` |
| tree-sitter parsing, per-language extractors, language support matrix, scope/type resolution, `@lci:` annotations | `lci-parsing-languages` |
| MCP `code_insight`, health/layer/module/feature/coupling/naming/clone/error-handling analyzers, entry points | `lci-code-insight` |
| MCP `side_effects`, `semantic_annotations`, purity, callee classification | `lci-side-effects` |
| `lci git-analyze`, MCP `git_analysis`, `code_insight mode=git_analyze`, hotspots, duplicates in changed code | `lci-git-analysis` |
| scanner, gitignore, budget/overflow, parser pool, trigram merger, master index, RCU snapshots, watch mode, reindex, memory | `lci-indexing-pipeline` |
| `lci server/servers/shutdown/status/stats`, socket, routes, registry, idle timeout, eviction, RSS cap, client | `lci-server-lifecycle` |
| `lci mcp`, JSON-RPC framing, tool registration/schemas, validation, error convention, `info`/`index_stats`/`debug_info`, tool-surface snapshot | `lci-mcp-server` |
| `.lci.kdl`, `lci config init/show/validate`, every config key, env overrides, path attributes | `lci-config` |
| install scripts, `lci update`, packaging, CI legs, presets, `debug`, `memprofile`, profiling flags | `lci-ops-diagnostics` |
| perf gates, repo-qa benches, goldens as oracle, how to run an lci-vs-X comparison | `lci-benchmarks-evaluation` |

## Route by question

- "Where is X implemented / who calls X / what tests pin X" -> the area skill's Code map and Probe recipes.
- "Does lci do Y, and how well vs product Z" -> the area skill's Product comparison, then
  `lci-benchmarks-evaluation` for the measurement procedure, then the existing
  `design-unbiased-benchmarks` and `audit-benchmark-evidence` skills for rigor.
- "What is the whole feature set" -> the Surfaces table of every area skill, or the summary below.

## Feature set in one screen

- Surfaces: CLI (search, grep, def, refs, callers, tree, symbols, inspect, browse, list, git-analyze,
  status, stats, server, servers, shutdown, mcp, update, config, debug, memprofile), HTTP over Unix
  socket (20 routes), MCP over stdio (15 registered tools, LCF or JSON output; `docs/TOOLS.md` documents 14, `callers` is missing there).
- Index: trigram index with certified-absence narrowing, symbol store, reference tracker, postings,
  call graph, import resolver. In-memory, RCU snapshots on the read path, no disk persistence.
- Languages: 13 via vendored tree-sitter grammars; cross-file import resolution for a subset.
- Analysis: code_insight report (health, layers, modules as graph communities, features, coupling,
  naming vocabulary, clones, entry points, error handling BETA), side effects and purity, git change
  analysis.
- Agent delivery: get_context sections and modes, context manifests for handoff, compact LCF format,
  token budgets, object IDs.
- Operations: per-root daemon with idle timeout, instance cap, RSS self-cap; self-update; npm, uv,
  deb, rpm, tarball packaging.

## Navigation rule

This repo builds lci. Use it before grep:
`build/release/src/lci search|def|refs|tree|symbols <q> -r .`.
Fall back to Grep only where lci lacks the capability, and record the gap
(`.claude/rules/test-iteration-discipline.md`).

## Maintenance

When a surface is added or removed (a CLI verb, MCP tool, HTTP route, config key), update the owning
area skill's Surfaces table and this routing table in the same commit. `docs/TOOLS.md` and
`benchmarks/repo-qa/comprehension/surface/tool-surface.json` are the MCP contract snapshots; the
skills point at them rather than copying them.
