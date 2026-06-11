# Windows + macOS Port — Deferred Follow-ups

Tracked corners deliberately not closed in the Windows+macOS compilation/release
work (`2026-06-09-windows-macos-release.md`). Each is gated, not silently
dropped (karpathy #6). Tag when filed in Dart: `loop-fix`.

## 1. Port the parity / integration test runner to Windows

**What:** `tests/parity/runner/modes/{cli,mcp,http}.cpp` and
`tests/parity/runner/modes/child_guard.h` spawn child processes with the POSIX
`fork` / `dup2` / `execvp` / `waitpid` / `kill` pattern. The integration +
real-project suites, the parity runner, and the benchmark suite are gated to
**Linux only** in `tests/CMakeLists.txt` (they run only on the Linux CI legs):

```cmake
if(NOT WIN32 AND NOT APPLE)
    add_executable(lci_integration_tests ...)
    add_executable(lci_real_project_tests ...)
endif()
# ...
if(NOT WIN32 AND NOT APPLE)
    add_subdirectory(parity)
endif()
```

**Why deferred:** The enforced Windows/macOS green bar is the unit suite
(`lci_tests`) plus `spec_diff_unit_tests`, matching the Linux release gate.
Windows has no fork/exec; on macOS these suites are never executed, and even
compiling the runner there trips libc++'s stricter transitive includes
(`tests/parity/runner/modes/index.cpp` uses `close()` without `<unistd.h>`).
A real macOS/Windows port needs an async child-process abstraction
(non-blocking pipe reads, timeout, signal-equivalent kill) that the current
synchronous `lci::subprocess` does not expose.

**To close:** extend `lci::subprocess` with a streaming/async child handle
(read stdout/stderr without blocking, terminate with timeout) on
`CreateProcessW` + overlapped I/O / `WaitForSingleObject`, then drop the
`if(NOT WIN32)` gates and port the three mode handlers + `child_guard.h`.

## 2. macOS x86_64 coverage (universal binary or second leg)

**What:** the macOS release leg builds **arm64 only** (native on
`macos-latest`). `install.sh`'s `Darwin` branch does not arch-check, so an
Intel mac would download the arm64 `lci-<ver>-Darwin.tar.gz` and fail to run.

**Why deferred:** a universal binary needs a vcpkg universal/x86_64 triplet so
abseil/re2/efsw build multi-arch; `CMakePresets.json` already has a
`macos-universal` preset (`LCI_UNIVERSAL_BINARY=ON`) but the dependency side is
not wired.

**To close (pick one):**
- Wire the `macos-universal` preset end-to-end (vcpkg universal triplet for all
  native deps) and switch the release leg to it; or
- add an x86_64 macOS release leg; or
- at minimum, add an arch guard to `install.sh`'s `Darwin` branch so Intel macs
  fail fast with "build from source" instead of fetching a non-runnable binary.

## 3. efsw FSEvents teardown abort on macOS (FileWatcherTest excluded)

**What:** `FileWatcherTest.*` is excluded from the macOS unit-suite gate
(`--gtest_filter=-FileWatcherTest.*` in both `ci.yml` and `release.yml`).
Running it on macOS aborts during watcher teardown:

```
libc++abi: terminating due to uncaught exception of type std::system_error:
mutex lock failed: Invalid argument
Abort trap: 6   (exit code 134)
```

**Why:** vendored efsw's **FSEvents** backend (macOS) races on teardown — the
macOS analogue of the inotify race fixed for Linux by `cmake/patch-efsw.cmake`
(`bool mIsTakingAction` → `Atomic<bool>` in `FileWatcherInotify.hpp`). The
FSEvents backend (`FileWatcherFSEvents.cpp`) is unpatched. `src/indexing/
watcher.cpp` teardown is correct (`stop()` joins the efsw worker before the
object dies); the abort is inside efsw. Watcher logic is exercised by the same
`FileWatcherTest` suite on the Linux CI legs, so cross-platform coverage holds.

**To close:** port the inotify teardown patch to efsw's FSEvents backend
(guard the watch-action / run-loop mutex against post-destruction access), add
a `cmake/patch-efsw-fsevents.cmake` analogous to the inotify one, then drop the
macOS `-FileWatcherTest.*` filter.
