# Lightning Code Index (lci)

lci aims to make semantic code search fast enough for interactive use and
to cut the context an AI assistant has to read compared with grep. Tree-sitter
parsing across 13 languages. An MCP server for AI assistants, plus CLI, HTTP,
and Unix-socket interfaces.

Version tracks the `VERSION` field of `project(lci ...)` in [`CMakeLists.txt`](CMakeLists.txt) (currently `0.10.1`); check `lci --version` for what you have installed.

## Quick start

> **Platform availability:** prebuilt releases ship for **Linux x86_64**
> (`.tar.gz`, `.deb`, `.rpm`), **Windows x86_64** (`.tar.gz`), and **macOS
> arm64 / Apple Silicon** (`.tar.gz`). Intel (x86_64) macs are not yet served by
> a prebuilt binary — build from source on those. The installers fail fast with
> a clear message on any unsupported platform/arch.

### One-line install (recommended)

```sh
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/standardbeagle/lci-cpp/main/install.sh | sh
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/standardbeagle/lci-cpp/main/install.ps1 | iex
```

Both detect your OS/arch, download the matching release binary, and install
it (`/usr/local/bin` or `~/.local/bin` on Unix; `%LOCALAPPDATA%\Programs\lci`
on Windows). Override with `LCI_PREFIX`; pin a version with `LCI_VERSION=0.10.1`.

### Via npm

```sh
npm install -g @standardbeagle/lci
lci --version
```

### Via uv / pip

```sh
uv tool install lci-cli      # or: pipx install lci-cli
lci --version
```

The npm and uv packages download the prebuilt binary for your platform — no
compiler required.

### Updating

```sh
lci update            # self-update to the latest release
lci update --check    # report current vs latest without installing
lci update --version 0.10.1   # install a specific release
```

`lci update` works regardless of how lci was installed. Package-manager
users can also run `npm update -g @standardbeagle/lci` or
`uv tool upgrade lci-cli`.

### From a release artifact

```sh
# Linux: tarball or Debian package
curl -fLO https://github.com/standardbeagle/lci-cpp/releases/download/v0.10.1/lci-0.10.1-Linux.tar.gz
tar -xzf lci-0.10.1-Linux.tar.gz
sudo install lci-0.10.1-Linux/bin/lci /usr/local/bin/

# or
sudo dpkg -i lci-0.10.1-Linux.deb

lci --version   # 0.10.1
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
cpack -G TGZ    # produces lci-0.10.1-Linux.tar.gz
cpack -G DEB    # produces lci-0.10.1-Linux.deb
```

The CPack output contains only `bin/lci`. Tree-sitter grammars are
statically linked; the binary dynamically links libc, libstdc++, libssl
(via libcrypto), zlib, and brotli — the Debian package declares only
`libc6 (>= 2.35)` since the rest ship on modern Linux by default.

## What it does

| Surface | Use |
|---------|-----|
| `lci search PATTERN` | substring + symbol-aware ranked search across the corpus |
| `lci grep PATTERN` | literal text search over the corpus, grep-style output |
| `lci tree SYMBOL` | function call hierarchy with annotations |
| `lci def SYMBOL` / `lci refs SYMBOL` | definition + reference lookup |
| `lci git-analyze` | analyze working-set changes for duplicates, naming, complexity |
| `lci mcp` | MCP (Model Context Protocol) server over stdio for AI assistants |
| `lci server` | long-running HTTP server over a Unix socket; per-corpus daemon |
| `lci servers` | list every index server you are running, and the root each serves |
| `lci shutdown [--all]` | stop the server for this root, or every one of them |
| `lci status` | index health + runtime metrics |

Full CLI reference: `lci --help` or `lci <command> --help`.

### MCP tools

`lci mcp` exposes 15 tools to AI assistants: `search`, `find_files`,
`get_context`, `context`, `list_symbols`, `inspect_symbol`, `browse_file`,
`callers`, `semantic_annotations`, `side_effects`, `code_insight`,
`git_analysis`, `index_stats`, `debug_info`, and `info`. `code_insight`
emits its report in LCF (LCI Compact Format), a token-dense, agent-oriented
layout; the other tools return structured JSON.

`code_insight` is the session-startup workhorse: repository map, health
dashboard, entry points, complexity/coupling/cohesion statistics, module
and feature breakdowns, naming vocabulary, and git change/hotspot
analysis.

Full per-tool reference — parameters, modes, output shapes, errors — is in
[`docs/TOOLS.md`](docs/TOOLS.md).

### Server lifecycle

Any command that needs an index auto-starts a background server for that root
and leaves it running, because re-indexing per command would dominate every
query. Left unbounded that leaks: a caller that indexes a temp directory and
deletes it strands a multi-GB server per root (175 were once found on one
host). Three policies bound it, all under a `server` block in `.lci.kdl`:

| Setting | Default | Effect |
|---|---|---|
| `idle_timeout_sec` | `1800` | exit after this long with no real request (`/ping` doesn't count); the next command respawns |
| `max_instances` | `8` | a starting server asks the least-recently-active servers beyond this cap to stop |
| `max_rss_mb` | `4096` | self-cap; the server sheds cache and, if still over, exits rather than grow past this RSS (`0` disables) |
| — | always on | exit within ~500 ms of the indexed root being deleted |

`lci servers` shows what is running and which root each one serves — the socket
name hashes the root, so the filesystem alone cannot tell you. `lci shutdown`
stops the server for the current root; `lci shutdown --all` stops every one of
them.

Both read the instance registry, which only standalone servers publish into.
The index server embedded in `lci mcp` stays out of it deliberately: it belongs
to the MCP session, and evicting it would break that session while leaving the
process alive. Stop those by ending the MCP client. Servers from before 0.8.0
predate the registry entirely — they answer neither command, and must be killed
by signal.

## Architecture

- **Indexer pipeline**: scanner → parser pool (tree-sitter) → per-file
  extraction and per-file trigram bloom → integrator → search engine.
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
Python, Rust, C#, PHP, Zig, and C/C++.

## Testing

```sh
cmake --build build/release --parallel
ctest --preset release --parallel $(nproc)
```

The preset excludes the separately gated benchmark target. Run
`./build/release/tests/lci_benchmarks` explicitly for performance checks.
CI gates the synthetic suite against
`tests/benchmarks/baseline/linux-x64.json` via `scripts/bench_gate.py`
(fails on >1.5x regression); regenerate the baseline on the benchmark
runner with `scripts/bench_gate.py --update` when benchmarks change.

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
| MCP server | 15 tools, all engine-backed |
| Performance | synthetic benchmark suite gated in CI (fails on >1.5x regression vs stored baseline); real-project benches run locally |

## License

MIT. See [`LICENSE`](LICENSE).
