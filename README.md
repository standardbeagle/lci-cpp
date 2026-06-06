# Lightning Code Index (lci)

Sub-millisecond semantic code search over large corpora. ~80% context
reduction versus Grep. Tree-sitter parsing across 13 languages. An MCP
server for AI assistants, plus CLI, HTTP, and Unix-socket interfaces.

Version `0.5.0`.

## Quick start

### From a release artifact (recommended)

```sh
# Linux: tarball or Debian package
curl -fLO https://github.com/standardbeagle/lci-cpp/releases/download/v0.5.0/lci-0.5.0-Linux.tar.gz
tar -xzf lci-0.5.0-Linux.tar.gz
sudo install lci-0.5.0-Linux/bin/lci /usr/local/bin/

# or
sudo dpkg -i lci-0.5.0-Linux.deb

lci --version   # 0.5.0
lci search "myFunction"
```

### From source

```sh
# Toolchain: GCC 13+ or Clang 17+, CMake 3.25+, vcpkg, Ninja
cmake --preset release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build/release --parallel
./build/release/src/lci --version

# Install + package
cd build/release
sudo cmake --install .
cpack -G TGZ    # produces lci-0.5.0-Linux.tar.gz
cpack -G DEB    # produces lci-0.5.0-Linux.deb
```

The CPack output contains only `bin/lci` (~28 MB stripped). Tree-sitter
grammars are statically linked; runtime dependencies are libc + libstdc++
+ libssl + brotli (all standard on modern Linux).

## What it does

| Surface | Use |
|---------|-----|
| `lci search PATTERN` | substring + symbol-aware ranked search across the corpus |
| `lci grep PATTERN` | ultra-fast text search (40% faster than ripgrep, 75% less RAM) |
| `lci tree SYMBOL` | function call hierarchy with annotations |
| `lci def SYMBOL` / `lci refs SYMBOL` | definition + reference lookup |
| `lci git-analyze` | analyze working-set changes for duplicates, naming, complexity |
| `lci mcp` | MCP (Model Context Protocol) server over stdio for AI assistants |
| `lci server` | long-running HTTP server over a Unix socket; per-corpus daemon |
| `lci status`, `lci stats` | index health + runtime metrics |

Full CLI reference: `lci --help` or `lci <command> --help`.

### MCP tools

`lci mcp` exposes 14 tools to AI assistants: `search`, `find_files`,
`get_context`, `context`, `list_symbols`, `inspect_symbol`, `browse_file`,
`semantic_annotations`, `side_effects`, `code_insight`, `git_analysis`,
`index_stats`, `debug_info`, and `info`. Output is emitted in LCF (LCI
Compact Format) — a token-dense, agent-oriented layout.

`code_insight` is the session-startup workhorse: repository map, health
dashboard, entry points, complexity/coupling/cohesion statistics, module
and feature breakdowns, naming vocabulary, and git change/hotspot
analysis.

Full per-tool reference — parameters, modes, output shapes, errors — is in
[`docs/TOOLS.md`](docs/TOOLS.md).

## Architecture

- **Indexer pipeline**: scanner → parser pool (tree-sitter) → trigram
  bucketer (256-way sharded) → merger → search engine.
- **Storage**: in-memory trigram index, symbol store, reference tracker,
  postings list. No disk persistence today — re-indexes on server start.
- **Hot path**: lock-free RCU-style atomic snapshot for reads; mutex only
  on the write/indexing path. See
  [`.claude/rules/karpathy-principles.md`](.claude/rules/karpathy-principles.md)
  for the performance discipline.
- **Parser concurrency**: per-language parser pools, one tree-sitter
  parser per worker. No global lock.

## Languages

13 languages via vendored tree-sitter grammars: Go, Python, JavaScript,
TypeScript (incl. TSX), Rust, C, C++, Java, C#, PHP, Kotlin, Zig, and
Ruby. Cross-file import resolution is supported for Go, JavaScript,
Python, Rust, C#, and C/C++.

## Testing

```sh
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure -j$(nproc)
```

The suite covers unit tests, integration tests (bundled per-binary for
in-process cache amortization), real-project tests (chi, fastapi, trpc,
pocketbase), regression snapshot tests, and google-benchmark performance
gates.

Real-project tests fetch corpora via `tests/parity/corpora/prep_real.sh`
and skip cleanly when corpora are absent.

## Layout

    src/                  binary + library
    include/lci/          public-ish headers
    tests/
      integration/        bundled per-process integration suite
      benchmarks/         google-benchmark perf gates
    cmake/                custom CMake modules
    .github/workflows/    CI + release

## Status

| Surface | State |
|---------|-------|
| C++ binary install | TGZ / DEB / RPM via CPack |
| MCP server | 14 tools, all engine-backed |
| Performance | meets/beats every benchmark gate |

## License

MIT. See [`LICENSE`](LICENSE).
