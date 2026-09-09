---
name: lci-ops-diagnostics
description: "Use when: working on lci install scripts, self-update, `debug`/`status`/`list`/`memprofile` commands, --profile-* flags, packaging (CPack/npm/pip), CI legs, CMake presets, fuzz targets, binary size or runtime-dependency claims, or comparing lci's install footprint with other code tools."
---

# LCI Ops and Diagnostics

Everything a user touches to get lci onto a machine, keep it current, and find out what the
index server is doing: one-line installers, `lci update`, `lci status`, `lci list`, the
`lci debug` family, `lci debug memprofile`, gperftools profiling flags, and the packaging and
CI that produce the release artifacts. This skill maps that code and gives the evidence needed
for footprint comparisons with other tools.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| `lci --version` / `-V` | `src/cli/main.cpp:65` | Prints `lci version <kVersion>`; constant generated from `cmake/version.h.in` into `build/release/generated/lci/version.h` (project version in `CMakeLists.txt:16`). |
| `lci update [--check] [-f/--force] [--version X]` | `src/cli/main.cpp:480` -> `src/cli/update.cpp:8` -> `src/update/updater.cpp:385` | Self-update from GitHub releases via exec'd `curl`/`tar`/`sha256sum`. |
| `lci status [-j] [-v]` (alias `st`) | `src/cli/main.cpp:410` -> `src/cli/status.cpp:61` | Reads `/stats` from the running server via `lci::Client::get_stats` (`include/lci/server/client.h:81`). No `stats` verb exists; JSON via `--json`. |
| `lci list [-v]` (alias `ls`) | `src/cli/main.cpp:650` -> `src/cli/commands.cpp:816` | Stand-alone `FileScanner` walk, does not query the server. One absolute path per line. |
| `lci --test-run` (hidden global) | `src/cli/main.cpp:82`, helper at `:499`, dispatch `:987` | Emits the file list the indexer would consume; same scanner as `list`. |
| `lci --profile-cpu PATH` / `--profile-memory PATH` (hidden global) | `src/cli/main.cpp:88`, `:91`, pre-parse at `:1015-1034` | gperftools; fail fast when built without `LCI_ENABLE_GPERFTOOLS` (`src/cli/profiling.cpp:37`, `:65`). |
| `lci search --cpu-profile/--mem-profile/--compare-search` | `src/cli/main.cpp:228-238`, `src/cli/search.cpp:147`, `:278` | Per-search profile scope. `--compare-search` prints a note that the legacy path is gone. |
| `lci debug info [-v] [--incremental]` (alias `dbg i`) | `src/cli/main.cpp:716` -> `src/cli/debug.cpp:77` | Root, socket, ready/indexing state, file/symbol counts, threads, RSS, search stats. |
| `lci debug validate` | `src/cli/main.cpp:741` -> `src/cli/debug.cpp:154` | Fail-fast stub, exit 1: no server-side consistency check exists. |
| `lci debug deps` / `lci debug graph` | `src/cli/main.cpp:754`, `:792` -> `src/cli/debug.cpp:170`, `:174` | Fail-fast stubs, exit 1: symbol-linker engine deleted (S9). |
| `lci debug export -o FILE` | `src/cli/main.cpp:767` -> `src/cli/debug.cpp:180` | Writes status+stats JSON to a file. |
| `lci debug memprofile [--ratio] [--floor-mb] [--top]` (alias `mem`) | `src/cli/main.cpp:806` -> `src/cli/memprofile.cpp:93` | Indexes one file at a time, reports VmRSS delta offenders. |
| HTTP `/stats`, `/status`, `/ping` | `src/server/server.cpp:1197`, `:1173`, `:1169` (POST), `:1256`, `:1264` (GET) | What `status` and `debug info` read. |
| RSS self-cap reaper | `src/server/server.cpp:105` (`read_own_rss_mb`), `:1077-1107` | `server.max_rss_mb`; malloc_trim then loud exit. |
| `install.sh` / `install.ps1` | repo root | curl-or-irm one-liners; repo `standardbeagle/lci-cpp`; `LCI_VERSION`, `LCI_PREFIX`. |
| `packaging/npm` (`@standardbeagle/lci`) | `packaging/npm/package.json`, `postinstall.js`, `bin/lci.js` | Postinstall downloads the release tarball for the package version. |
| `packaging/python` (`lci-cli`) | `packaging/python/pyproject.toml`, `lci_cli/installer.py` | First run downloads and caches the binary. |
| CPack TGZ/DEB/RPM | `CMakeLists.txt:481-530` | Component `lci_runtime`; ships only `bin/lci`. |

## Code map

Entry -> core -> storage/build:

- `src/cli/main.cpp` — CLI11 tree; every verb above is registered here. Key lambdas: `run_test_run` (`:504`), the `--profile-*` pre-parse loop (`:1015`).
- `src/cli/update.cpp` — `run_update(bool,bool,string)` builds `UpdateConfig` with `lci::kVersion` and calls the updater.
- `src/update/updater.cpp` / `include/lci/update/updater.h` — `run_update(const UpdateConfig&)` (`:385`), `detect_platform` (`:272`), `select_asset` (`:294`), `is_safe_download_url` (`:326`), `is_safe_asset_name` (`:351`), `expected_hash_for` (`:362`), `sha256_of_file` (`:108`), `make_private_workdir` (`:247`), `replace_self` (`:182`, `rename(2)` swap with rollback). `UpdateConfig.repo` defaults to `standardbeagle/lci-cpp` (`updater.h:66`).
- `src/cli/status.cpp` — `run_status`; text and `--json` both come from the server's `StatsResponse`, never from the CLI's own `/proc/self` (header comment `:16`).
- `src/cli/debug.cpp` — `run_debug_info`, `run_debug_export`, the three fail-fast stubs and `debug_removed_linker_command` (`:64`).
- `src/cli/memprofile.cpp` — `run_debug_memprofile`; two rulers (VmRSS for phases, per-file deltas), `FileDelta` (`:72`), `LanguageInit` (`:83`).
- `src/cli/profiling.cpp` / `profiling.h` — `ProfilerGuard` start/stop around gperftools `ProfilerStart` / `HeapProfilerStart`; compiled behind `LCI_HAVE_GPERFTOOLS`.
- `src/cli/commands.cpp:816` — `run_list`.
- `include/lci/cli/commands.h` — `GlobalFlags` (`:16`), declarations `run_status:160`, `run_list:259`, `run_debug_*:305-341`, `run_update:348`.
- `src/server/server.cpp` — `/ping`, `/status`, `/stats` routes; `read_own_rss_mb` reads `RssAnon` with `VmRSS` fallback; reaper tick enforces `max_rss_mb`.
- `src/server/client.cpp:235` — `get_stats` posts `/stats`.
- Build and packaging: `CMakeLists.txt:3-12` (ccache launcher), `:111-112` (version.h generation), `:193-198` (`BUILD_SHARED_LIBS OFF` so the tarball is self-contained), `:481-530` (CPack; DEB depends `libc6 (>= 2.35)`, RPM `glibc >= 2.35`); `src/CMakeLists.txt:176` (`LCI_ENABLE_GPERFTOOLS`, default OFF); `CMakePresets.json:15` (`FETCHCONTENT_BASE_DIR=$HOME/.cache/lci-cpp-deps/<preset>`), presets `profile:35`, `sanitizer:47`, `tsan:57`, `msan:67`, `fuzz:77`, `ci:91`, `vcpkg:99`, `macos-universal:107`; `vcpkg.json` (deps for CI/Windows/macOS legs).
- CI: `.github/workflows/ci.yml` jobs `linux:33` (gcc-13/clang-18 release, debug; sanitizer+tsan main-only; `CMAKE_BUILD_PARALLEL_LEVEL=6`), `windows:164` (runs the full `lci_tests.exe` + `spec_diff_unit_tests.exe`, `:225`), `macos:269` (`macos-latest`, the only billed leg), `benchmarks:346` (runs `scripts/bench_gate.py:430`). `.github/workflows/release.yml`: `build-linux:15` (cpack TGZ/DEB/RPM `:84-86`), `build-windows:107` (`x64-windows-static-md` triplet `:141`), `build-macos:182` (arm64), `publish` job generates `SHA256SUMS` and creates the GitHub release. No npm or PyPI publish step exists in any workflow.
- Fuzz: `tests/fuzz/*.cpp`, `tests/CMakeLists.txt:384-395` (`LCI_FUZZ_TARGETS`, 8 targets), `tests/fuzz/README.md`.
- Tests: `tests/updater_test.cpp` (asset selection, checksum parsing, URL/name safety, private workdir, platform detect), `tests/cli_test.cpp:3404` (`MemProfileTest`), `:3433-3502` (`CliDebugInfoTest`, `CliDebugValidateTest`, `CliDebugExportTest`), `:3526`, `:3552` (`CliStatusTest`). No test exercises `run_update` end to end (network).
- Perf gate: `scripts/bench_gate.py` against `tests/benchmarks/baseline/linux-x64.json`.
- Docs: `README.md:9-96` (install, update, from-source, ~28 MB claim), `docs/src/content/docs/getting-started.mdx`, `docs/src/content/docs/cli-usage.mdx:62-92`, `docs/superpowers/specs/2026-06-07-install-update-distribution-design.md`, `docs/performance/profiling-wsl2.md`, `docs/performance/test-suite-*.md`.
- Memory notes (user memory dir `~/.claude/projects/-home-beagle-work-core-lci-cpp/memory/`): `self-contained-release-binary-traps.md`, `wsl2-profiling-setup.md`, `memprofile-attribution-traps.md`, `server-lifecycle-reaper.md`, `errlookup-lci-oom.md`.

## Config knobs

| Knob | Default | Where |
|---|---|---|
| `server.idle_timeout_sec` | 1800 (0 disables) | `include/lci/config.h:66`, `ServerConfig` |
| `server.max_rss_mb` | 4096 (0 disables; Linux-only enforcement) | `include/lci/config.h:80`; enforced `src/server/server.cpp:1087` |
| `insight.error_report` | `"off"` (`off|capture|on`) | `include/lci/config.h:155`; env `LCI_ERROR_REPORT` overrides (`:153`) |
| `LCI_VERSION` env | unset = latest | `install.sh:7,52`, `install.ps1:33` |
| `LCI_PREFIX` env | `~/.local/bin` (sh), `%LOCALAPPDATA%\Programs\lci` (ps1) | `install.sh`, `install.ps1:8,60` |
| `update --version` | empty = latest | `UpdateConfig.target_version`, `updater.h:68` |
| `LCI_ENABLE_GPERFTOOLS` (CMake) | OFF | `src/CMakeLists.txt:176` |
| `FETCHCONTENT_BASE_DIR` (CMake) | `$HOME/.cache/lci-cpp-deps/<preset>` | `CMakePresets.json:15` |
| `--ratio`, `--floor-mb`, `--top` | see `MemprofileOptions` | `src/cli/main.cpp:813-824` |

There is no `.lci.kdl` key for update channel, telemetry, or profiling; those are CLI flags and build options only.

## Invariants and traps

- `debug validate` must never print a pass verdict: `/status` has no `error` writer, so the old branch fabricated "All consistency checks passed!" for every server. Now a fail-fast stub, exit 1. `.claude/rules/karpathy-principles.md` rule 6; `src/cli/debug.cpp:147-159`; pinned by `CliDebugValidateTest`.
- `debug deps` / `debug graph` are stubs because the symbol-linker engine they read was deleted (Go imports always external, SymbolID collisions past 10k). `src/cli/debug.cpp:162-176`. Their registrations still exist in `main.cpp` pending removal.
- `status` text and `--json` must come from the server, not the CLI's own `/proc/self`; the RED for this needed an independent producer (`lci::Client::get_stats`). `.claude/rules/worktree-isolation-and-goldens.md` rule 6; `src/cli/status.cpp:16`; `CliStatusTest.ThreadsAndRssMatchServerStatusJson`.
- Self-update trusts nothing from the API response: URLs must be https on a GitHub host (`is_safe_download_url`), asset names have no path or shell metachars, the release must ship `SHA256SUMS` or the updater refuses (`updater.cpp:553-560`), the download lands in a private 0700 workdir, and the swap is `rename(2)` with rollback. `install.sh:136-139` is weaker: it warns and continues when `SHA256SUMS` is missing.
- Version comparison in the updater is string equality, not ordering (`updater.cpp:459`). A dev binary newer than the latest release is told "Update available: 0.10.1 -> 0.7.0" and `lci update` would downgrade it. Observed live 2026-09-08.
- Release-binary trap class: green CI, broken tarball. Two instances: shared `libprofiler.so.0` auto-linked from the runner (fixed, gated behind `LCI_ENABLE_GPERFTOOLS`, `src/CMakeLists.txt:165-176`), and Windows abseil/re2 DLLs from the default triplet (fixed with `x64-windows-static-md`, `release.yml:132-141`). Memory note `self-contained-release-binary-traps.md`.
- `BUILD_SHARED_LIBS OFF` is forced (`CMakeLists.txt:193-198`) because efsw and json-schema-validator otherwise build shared on a fresh CI configure.
- `memprofile` per-file numbers were misattributed before `4cfd0e6` (parser init charged to the first file of a language; RSS arena chunking). Memory note `memprofile-attribution-traps.md`; header comment `src/cli/memprofile.cpp:10`.
- `sanitizer` preset does not compile abseil under this host's GCC; prove memory-safety criteria with a standalone ASan driver over the exact TU instead. `.claude/rules/test-iteration-discipline.md` rule 8 (sanitizer section).
- Fuzz builds must be uniformly ASan-instrumented; a partial build aborts in abseil `AddrIsInMem`. `tests/fuzz/README.md:3-8`.
- Worktree removal leaves `*-build` Makefiles in the shared dep cache pointing at the dead path; repair with `cmake --preset release` in the primary checkout. `.claude/rules/worktree-isolation-and-goldens.md` rule 6.
- The Linux gate cannot see MSVC errors; Windows CI runs the full `lci_tests` binary, so tests touching env/time go through `tests/helpers/portable_env.h`. `.claude/rules/test-iteration-discipline.md` rule 8.
- `RealProjectSearchLatencyTest.FastapiSearchUnder5ms` asserts absolute wall-clock inside the gate and fails under host load. `.claude/rules/test-iteration-discipline.md` rule 6.
- WSL2 hardware PMU counting works on this host (folklore says otherwise); PEBS/perf-mem are bare-metal only. `docs/performance/profiling-wsl2.md`.
- Findings from the 2026-09-08 mapping (fixed or filed): see `docs/reviews/2026-09-08-skill-mapping-findings.md`.
- Homebrew formula exists at `Formula/lci.rb` (repo root, not under `packaging/`); check its pinned version against the release before citing it. No Scoop, winget, or AUR packaging in tree.
- Dogfood: `lci def run_update` correctly returns both overloads; `lci def select_asset` found the anonymous-namespace function. No gap hit in this mapping.

## Probe recipes

```sh
LCI=build/release/src/lci
ROOT=$(git rev-parse --show-toplevel); S=$(mktemp -d)

$LCI --version
$LCI status -r $ROOT; $LCI status --json -r $ROOT
$LCI list -r $ROOT | wc -l          # 1356 on 2026-09-08
$LCI --test-run -r $ROOT | head
$LCI debug info -r $ROOT; $LCI debug info --incremental -r $ROOT
$LCI debug validate; echo "exit=$?"  # expect exit 1 with the stub message
$LCI debug export -o $S/dbg.json -r $ROOT
$LCI debug memprofile --top 10 -r $ROOT
$LCI update --check                  # network; string-equality compare (see traps)
$LCI --profile-cpu $S/cpu.prof search foo -r $ROOT   # errors unless built with -DLCI_ENABLE_GPERFTOOLS=ON

# Binary footprint (local release build, not the CI artifact)
ls -l $LCI; file $LCI; ldd $LCI
cp $LCI $S/lci-stripped && strip $S/lci-stripped && ls -l $S/lci-stripped

# Packaging
cmake --build $ROOT/build/release --parallel --target lci && (cd $ROOT/build/release && cpack -G TGZ)   # cpack must run in the build dir
python3 $ROOT/scripts/bench_gate.py --help

# Targeted tests (build only lci_tests first)
cmake --build $ROOT/build/release --parallel --target lci_tests
$ROOT/build/release/tests/lci_tests --gtest_filter='Updater*:CliStatusTest.*:CliDebug*:MemProfileTest.*'
ctest --test-dir $ROOT/build/release -R 'Updater|CliStatus|CliDebug|MemProfile' -j4
```

Goldens: none for this area under `tests/integration/goldens/` (status/debug output is asserted in `tests/cli_test.cpp`, not golden-pinned). Fuzz corpora: `tests/fuzz/corpus/`.

## Product comparison

Comparable tools for install footprint and diagnostics: ripgrep, universal-ctags, Zoekt, Sourcegraph (SCIP indexers + server), Serena (Python MCP over LSP), code-index-mcp (Python MCP), also ast-grep and semgrep as single-binary or pip-installed code tools.

lci's claims in this area:

| Claim | Where | How to measure | Existing numbers |
|---|---|---|---|
| Release ships only `bin/lci`, ~28 MB stripped | `README.md:93`, `getting-started.mdx:99` | `tar tzf lci-*.tar.gz`; `strip` a release build and `ls -l` | Local build 2026-09-08: 36.5 MB unstripped, 33.8 MB stripped (local toolchain, not the CI artifact; the 28 MB figure is unverified against a current release) |
| Runtime deps: libc, libstdc++, libssl, brotli | `README.md:94-95` | `ldd bin/lci` on the release tarball | Local build: libbrotli{enc,dec,common}, libz, libssl3, libcrypto3, libstdc++, libm, libgcc_s, libc |
| Prebuilt Linux x86_64 (tgz/deb/rpm), Windows x86_64 (tgz), macOS arm64 (tgz) | `README.md:12`, `release.yml` | `gh release view --repo standardbeagle/lci-cpp` | Updater tests pin `LinuxArm64Unsupported` (`updater_test.cpp:38`) |
| Self-update works regardless of install method, checksum-verified | `README.md:50-60`, `cli-usage.mdx:86-92` | `lci update --check`; inspect `updater.cpp:506-560` | No end-to-end test; unit tests cover asset select, hash parse, URL safety |
| No disk persistence, re-index on server start | `README.md:161` | `lci status` build time after `lci shutdown` | `status` on self repo: 1355 files, 1.4 s build, 38.9 MB index (2026-09-08) |
| Server bounds its own RSS (`max_rss_mb`) | `include/lci/config.h:74-80` | Set a low cap, index a large corpus, watch server log | Memory note `errlookup-lci-oom.md` (26 GB incident, cap added `d71ac3e`) |

Comparison axes:

| Axis | lci | How to check the competitor | Notes |
|---|---|---|---|
| Install channels | curl/irm one-liner, npm, uv/pip wrapper, tgz/deb/rpm, source | Read each project's README install section; `brew info`, `apt show`, `cargo install` | lci has a Homebrew formula (`Formula/lci.rb`) but no Scoop/winget/AUR. ripgrep ships in most distro repos and Homebrew; universal-ctags in distro repos (unverified, from model knowledge). |
| Binary size | 33.8 MB stripped local; README says ~28 MB | `ls -l $(which rg)`, `ls -l $(which ctags)`, `du -sh` a Zoekt build | ripgrep is a few MB; Zoekt is a Go multi-binary set; Serena/code-index-mcp are Python trees plus a venv (all unverified, from model knowledge). Measure before quoting. |
| Runtime deps | libc, libstdc++, libssl, brotli, zlib (dynamic); tree-sitter grammars static | `ldd` / `otool -L` on the competitor binary; `pip show` deps for Python tools | ripgrep is typically fully static or libc-only (unverified). Serena requires a Python runtime plus per-language LSP servers (unverified). |
| Self-update | `lci update` with SHA256SUMS verification | grep competitor for `self-update`/`upgrade` subcommand | ripgrep, ctags, Zoekt have none in-binary; they rely on package managers (unverified, from model knowledge). |
| Platform matrix | Linux x64, Windows x64, macOS arm64 (universal option in CMake); no Linux arm64 artifact | Release page asset list | Linux arm64 gap is explicit in `updater_test.cpp:38` and `postinstall.js:31`. |
| Diagnostics | `status`, `debug info/export`, `list`, `memprofile`, optional gperftools flags | ripgrep `--debug`/`--trace`; ctags `--list-*`, `--_interactive`; Zoekt web `/debug`; Sourcegraph admin UI | lci has no consistency validator (`debug validate` is a stub). |
| Persistence | None; rebuild per server start | Zoekt/Sourcegraph/ctags write index files; ripgrep has no index | Both a simplicity win and a cold-start cost; measure with `status` build time. |
| Memory bound | `server.max_rss_mb` self-cap, idle timeout, LRU eviction of peer servers | Look for cgroup/ulimit guidance in competitor docs | Sourcegraph and Zoekt are server deployments with their own sizing guides (unverified). |
| Fuzz/sanitizer coverage | 8 libFuzzer targets, ASan/TSan CI legs on main | Check competitor CI config for `cargo fuzz`, oss-fuzz entries | ripgrep and tree-sitter are in OSS-Fuzz (unverified, from model knowledge). |
| CI matrix | gcc-13, clang-18, MSVC (full unit suite), AppleClang; sanitizer+tsan main-only | Competitor workflow files | Only the macOS leg is billed. |

Honest gaps vs the field: no distro packaging (Homebrew formula in-tree only, not a tap release you have verified); npm and pip wrappers are stale at 0.6.0 and unpublished by CI; no Linux arm64 build; the binary is an order of magnitude larger than ripgrep because 13 tree-sitter grammars, RE2, abseil and httplib are linked in; no on-disk index so every server start re-parses the corpus; the updater cannot tell newer from older; `debug validate/deps/graph` are stubs; profiling flags require a dev build.

## Related skills

lci-feature-map (router), lci-server-lifecycle, lci-config, lci-indexing-pipeline, lci-mcp-server, lci-benchmarks-evaluation, lci-search, lci-symbol-navigation, lci-get-context, lci-parsing-languages, lci-code-insight, lci-side-effects, lci-git-analysis.
