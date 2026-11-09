# lci-cpp agent notes

lci (Lightning Code Index): C++ code-intelligence engine. CLI + HTTP-over-Unix-socket + MCP server over
one in-memory trigram and symbol index, served by a per-root daemon. Rules live in `.claude/rules/`.

## Exposure

Every listener is local to one machine, and none of them may be widened without Andy's decision.

- `lci server`, and the index server embedded in `lci mcp`, on Linux and macOS: a Unix socket, mode
  `0600` in a `0700` per-user directory. Reachable by the owning user only.
- The same server on Windows: `loopback`, TCP on `127.0.0.1`. It has no authentication, so any local
  account can query the index and call `/shutdown`. This is a known gap, not an accepted design.
- `lci mcp` itself speaks over stdio and opens no other listener.

Caps, set in `IndexServer::register_handlers` and `include/lci/server/server.h`: 16 MiB request body,
30 s socket read and write timeouts, 64 requests per keep-alive connection, a fixed pool of
`max(hardware threads, 64)` workers, and 32 queued requests, past which connections are closed.
Tests: `ServerCaps*` in `tests/server_test.cpp`.

## Navigate with the product

This repo builds lci. Use it for code navigation before grep:
`build/release/src/lci search|def|refs|tree|symbols <query> -r .`

## Feature-area skills (`.agents/skills/`, mirrored at `.claude/skills/`)

Start at `lci-feature-map`; it routes by surface and by question. Each area skill holds the code map,
config knobs, invariants and traps, probe recipes, and a product-comparison section.

- `lci-feature-map` — router: which skill owns a surface or a comparison question
- `lci-search` — `search`/`grep`, trigram and postings, ranking, synonyms and stemming
- `lci-symbol-navigation` — def/refs/callers/tree, list_symbols/inspect_symbol/browse_file/find_files, call graph, imports
- `lci-get-context` — `get_context`, `context` manifests, object IDs, LCF, token budget, context-reduction claim
- `lci-parsing-languages` — tree-sitter extractors, 13-language capability matrix, scope and type resolution
- `lci-code-insight` — `code_insight` report and its analyzers
- `lci-side-effects` — `side_effects`, `semantic_annotations`, purity and callee classification
- `lci-git-analysis` — `git-analyze`/`git_analysis`, hotspots, changed-code metrics
- `lci-indexing-pipeline` — scanner, gitignore, budget, master index, RCU, watch mode, memory
- `lci-server-lifecycle` — daemon, socket, routes, registry, idle timeout, eviction, RSS cap
- `lci-mcp-server` — JSON-RPC layer, tool registration and schemas, validation, error convention
- `lci-config` — `.lci.kdl` key table, `config init/show/validate`, env overrides
- `lci-ops-diagnostics` — install, `update`, packaging, CI, presets, `debug`, `memprofile`, profiling
- `lci-benchmarks-evaluation` — perf gates, repo-qa benches, goldens as oracle, running an lci-vs-X comparison
- `design-unbiased-benchmarks`, `audit-benchmark-evidence` — rigor for any measurement or claim
