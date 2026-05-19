# Lightning Code Index (lci) — C++ port

Sub-millisecond semantic code search over large corpora. 79.8% context
reduction vs Grep. Tree-sitter parsing across 13 languages. MCP server
for AI assistants. CLI + HTTP + Unix-socket interfaces.

This repository is the **C++ port**, version `0.5.0`. It supersedes the
original Go implementation (`0.4.1`); the Go binary is now optional and
used only as a parity reference (see [Parity](#parity-with-go-reference)).

## Quick start

### From release artifact (recommended)

```sh
# Linux: tarball or Debian package
curl -fLO https://github.com/standardbeagle/lci/releases/download/v0.5.0/lci-0.5.0-Linux.tar.gz
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
| `lci mcp` | MCP (Model Context Protocol) server over stdio for AI assistants |
| `lci server` | long-running HTTP server over Unix socket; per-corpus daemon |
| `lci status`, `lci stats` | index health + runtime metrics |

Full CLI reference: `lci --help` or `lci <command> --help`.

## Architecture

- **Indexer pipeline**: scanner → parser pool (tree-sitter) → trigram
  bucketer (256-way sharded) → merger → search engine.
- **Storage**: in-memory trigram index, symbol store, reference tracker,
  postings list. No disk persistence today — re-indexes on server start.
- **Hot path**: lock-free RCU-style atomic snapshot for reads; mutex only
  on the write/indexing path. See [`.claude/rules/karpathy-principles.md`]
  (.claude/rules/karpathy-principles.md) for the perf discipline.
- **Parser concurrency**: per-language parser pools, one tree-sitter
  parser per worker. No global lock.

13 languages supported via vendored tree-sitter grammars: Go, Python,
JavaScript, TypeScript, TSX, Rust, C, C++, Java, plus a fallback path.

## Parity with Go reference

The C++ port maintains output parity with the Go reference (`lci 0.4.1`)
across **147 parity descriptors** covering CLI, MCP, HTTP, and index
modes. Tests diff canonicalized output of both implementations on the
same corpus + invocation.

As of `0.5.0`, the parity suite runs against **frozen snapshots** of Go
output committed to [`tests/parity/snapshots/`](tests/parity/snapshots/).
No Go binary is required on the host. The snapshots are regenerated
from a fresh Go binary via the
[`Snapshot Refresh`](.github/workflows/snapshot-refresh.yml) workflow.

See [`tests/parity/README.md`](tests/parity/README.md) for the full
parity harness, refresh procedure, and triage flow.

Documented intentional divergences (e.g. `Threads`+`RSS` C++ runtime
metrics vs `Goroutines`+`Heap` Go runtime metrics) live in
[`tests/parity/KNOWN_DIVERGENCE.md`](tests/parity/mcp/KNOWN_DIVERGENCE.md).

## Testing

```sh
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure -j$(nproc)
```

The suite covers unit tests, integration tests (bundled per-binary for
in-process cache amortization), real-project tests (chi, fastapi, trpc,
pocketbase), parity tests (snapshot-driven), benchmarks, and parity
unit tests for the diff library.

Real-project tests fetch corpora via `tests/parity/corpora/prep_real.sh`
and skip cleanly when corpora are absent.

## Layout

    src/                  binary + library
    include/lci/          public-ish headers
    tests/
      integration/        bundled per-process integration suite
      parity/             snapshot-driven Go-parity harness
      parity/snapshots/   frozen Go reference output
      benchmarks/         google-benchmark perf gates
    cmake/                custom CMake modules
    .github/workflows/    CI + release + snapshot-refresh

## Status

| Surface | State |
|---------|-------|
| Feature parity vs Go 0.4.1 | 147/147 parity green |
| C++ binary install | TGZ / DEB / RPM via CPack |
| Snapshot-based parity (no Go binary) | live; default path |
| Performance | meets/beats Go on every benchmark gate |
| Go binary | optional — kept only as snapshot regen source |

## License

MIT. See [`LICENSE`](LICENSE).
