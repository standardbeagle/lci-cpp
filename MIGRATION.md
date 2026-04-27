# Migrating from Go LCI to C++ LCI

The C++ port of LCI is a drop-in replacement for the Go version. The binary
name, CLI flags, MCP tool names, JSON output formats, and HTTP API are all
identical.

## Quick Start

Replace the Go binary with the C++ binary:

```sh
# macOS (Homebrew)
brew install standardbeagle/tap/lci

# Linux (DEB)
sudo dpkg -i lci-0.1.0-Linux.deb

# Linux (RPM)
sudo rpm -i lci-0.1.0-Linux.rpm

# Manual (any platform)
cp lci-cpp /usr/local/bin/lci
```

Verify:

```sh
lci --version
```

No configuration changes are needed. The same `.lci.kdl` config files work
without modification.

## CLI Compatibility

Every command and flag is identical:

| Command         | Alias | Description                       |
|-----------------|-------|-----------------------------------|
| `search`        | `s`   | Semantic code search              |
| `grep`          | `g`   | Ultra-fast text search            |
| `status`        | `st`  | Index server status               |
| `server`        | `srv` | Start persistent server           |
| `shutdown`      | `stop`| Stop persistent server            |
| `mcp`           |       | MCP stdio server                  |
| `def`           | `d`   | Find symbol definition            |
| `refs`          | `r`   | Find symbol references            |
| `tree`          | `t`   | Function call hierarchy           |
| `list`          | `ls`  | List indexed files                |
| `config init`   | `i`   | Initialize config                 |
| `config show`   | `s`   | Show config                       |
| `config validate`| `v`  | Validate config                   |
| `debug info`    | `i`   | Debug information                 |
| `debug validate`| `v`   | System consistency check          |
| `debug deps`    |       | Dependency graph                  |
| `debug export`  | `e`   | Export debug JSON                 |
| `debug graph`   | `g`   | Export DOT graph                  |
| `git-analyze`   | `ga`  | Git change analysis               |
| `symbols`       | `sym` | List/filter symbols               |
| `inspect`       | `insp`| Deep inspect symbol               |
| `browse`        | `br`  | File outline view                 |

Global flags: `-c/--config`, `-d/--daemon`, `--include`, `--exclude`,
`-r/--root`, `--test-run`, `-V/--version`.

## MCP Tool Compatibility

All 17 MCP tools produce byte-identical JSON responses:

| Tool                    | Description                              |
|-------------------------|------------------------------------------|
| `info`                  | Tool help and examples                   |
| `search`                | Semantic code search                     |
| `get_context`           | Code object context lookup               |
| `semantic_annotations`  | Semantic label queries                   |
| `side_effects`          | Purity and side-effect analysis          |
| `code_insight`          | Codebase intelligence analysis           |
| `find_files`            | File discovery with filters              |
| `list_symbols`          | Symbol listing with filters              |
| `inspect_symbol`        | Deep symbol inspection                   |
| `browse_file`           | File outline/symbol view                 |
| `index_stats`           | Index statistics                         |
| `debug_info`            | Debug diagnostics                        |
| `git_analysis`          | Git change analysis                      |
| `context_manifest`      | Multi-symbol context expansion           |
| `search_definitions`    | Definition-focused search                |
| `grep`                  | Fast text search                         |
| `tree`                  | Call hierarchy tree                      |

## HTTP API Compatibility

The Unix socket HTTP server exposes the same 15 endpoints on the same paths.
Client libraries that talk to the Go server work without changes.

## Configuration

The `.lci.kdl` format is fully supported. No changes needed.

## Platform Support

| Platform              | Go      | C++     |
|-----------------------|---------|---------|
| Linux x86_64          | Yes     | Yes     |
| macOS arm64           | Yes     | Yes     |
| macOS x86_64          | Yes     | Yes     |
| macOS universal       | No      | Yes     |
| Windows x86_64        | Yes     | Yes     |

## Performance Differences

The C++ port provides measurable improvements in key areas:

- **Indexing throughput**: Higher files/second due to zero-copy parsing and
  arena allocation
- **Search latency**: Lower p50/p95/p99 from lock-free trigram index and
  cache-friendly data layout
- **Memory usage**: Reduced RSS from slab allocator and string interning
- **Startup time**: Faster cold start from static linking and no runtime

Run `scripts/benchmark-compare.sh` to measure on your hardware.

## Known Differences

There are no intentional behavior differences. If you find output that does
not match between Go and C++ versions, file an issue.
