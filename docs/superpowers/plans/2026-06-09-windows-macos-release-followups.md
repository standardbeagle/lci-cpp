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

## 4. Windows runtime test port — RESOLVED 2026-06-15

**What:** the Windows unit gate (`ci.yml` + `release.yml`) excluded seven suites
(`ServerTest`, `ClientTest`, `GitReportToJson`, `CodeInsightGitTest`,
`ContextHandlerFixture`, `HandlersFixture`, `ExploreIndexTestFixture`). Both
root causes are now fixed and all seven are back in the Windows gate (only the
fork/exec integration/parity/benchmark suites remain gated off Windows in
`tests/CMakeLists.txt` — see #1).

**Root cause 1 — Server/Client fixtures were Unix-domain-socket shaped.**
`ServerTest`/`ClientTest` SetUp built a `.sock` file path and `make_client()`
called `set_address_family(AF_UNIX)`. On Windows the transport is TCP
(`localhost:<port>`); a temp `.sock` path is `C:\...\x.sock`, whose drive-letter
colon made `server.cpp`'s `stoi(sock.substr(sock.rfind(':')+1))` throw in
`SetUp()`, failing every test in both fixtures. The shipped binary was fine — it
gets its address from `get_socket_path*()` (already `localhost:<port>` on
Windows).

**Fix 1:** `tests/helpers/test_socket.h` — `next_test_server_address()` returns a
`.sock` path on POSIX / a process-unique `localhost:<port>` on Windows, and
`make_test_http_client()` selects AF_UNIX vs TCP to match `lci::Client`. Both
fixtures use it; their per-fixture `counter_` members were removed.

**Root cause 2 — `report_to_json` path relativisation was POSIX-shaped.**
`normalize_rel` used `std::filesystem::relative`. On Windows a `/`-rooted path
(no drive letter) is not `is_absolute()`, so the helper returned it un-stripped
with a leading `/`, failing the `front() != '/'` assertions.

**Fix 2:** `git/serialize.cpp` `normalize_rel` is now a purely lexical,
separator-independent string operation (normalize `\`→`/`, detect POSIX-root or
drive specifier, strip the `project_root` prefix on a segment boundary, preserve
absolute paths outside the root). No `std::filesystem` — identical result on
every platform, so the Linux legs now fully cover the Windows behaviour. Verify
locally with `scripts/parity-gap-tests.sh --win`; Windows CI is the gate.

## 5. macOS Go extractor misses exported symbols — RESOLVED 2026-06-14

**What:** four Go suites were excluded from the macOS unit gate
(`GoExtractorTest.*`, `GoLinkerIntegrationTest.*`,
`AllLinkerIntegrationTest.GoMultiFileProject`). On macOS the Go linker failed to
surface exported symbols — e.g. `has_export(t, "Hello")` returned `false` for a
file containing `func Hello()`. The same tests passed on Linux, and other
languages extracted correctly on macOS.

**Root cause (NOT a libc++ lifetime bug):** `GoExtractor` in
`src/symbollinker/go_linker.cpp` invoked

```cpp
add_symbol(table, std::move(name), is_exported(name));   // extract_function
add_symbol(table, std::move(var_name), is_exported(var_name)); // extract_var_declaration
```

The `std::move(name)` argument and the `is_exported(name)` argument to the *same*
call are **unsequenced** — C++ leaves the evaluation order of function arguments
unspecified. GCC evaluates right-to-left, so `is_exported` read the intact string
first (Linux passed). Clang/AppleClang evaluates left-to-right, so the string was
moved out into `add_symbol`'s by-value parameter *before* `is_exported` ran;
`is_exported` then saw a moved-from (empty) string, `name.empty()` was true, and
every exported Go symbol was marked non-exported. Driver is the **compiler
front-end**, not the stdlib — reproduced on Linux with `clang++-18`/`clang++-17`
(buggy → `exported=0`) vs `g++` (`exported=1`); the fixed pattern yields
`exported=1` on both.

**Fix:** compute the visibility bool before the move at both buggy sites:

```cpp
bool exported = is_exported(name);
add_symbol(table, std::move(name), exported);
```

(The other `add_symbol` sites in the file move `full_name` while reading a
*different* unmoved variable, so they were already order-safe.) The four suites
were dropped from the macOS CI filter; only `FileWatcherTest` (#3) remains
excluded on macOS. Verified locally via `scripts/parity-gap-tests.sh --mac`;
macOS CI leg is the production gate.
