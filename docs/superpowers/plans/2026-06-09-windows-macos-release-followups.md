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

## 4. Windows runtime test port (seven suites excluded from the Windows gate)

**What:** the Windows unit gate (`ci.yml` + `release.yml`) runs the full
`lci_tests` suite EXCEPT seven suites, via
`--gtest_filter=-ServerTest.*:ClientTest.*:GitReportToJson.*:CodeInsightGitTest.*:ContextHandlerFixture.*:HandlersFixture.*:ExploreIndexTestFixture.*`.
~1670 unit tests + `spec_diff_unit_tests` run and must pass on Windows; these
seven (~75 tests) are deferred. The Windows build itself is fully green, and
the shipped `lci.exe` is unaffected — the gaps are in test fixtures and one
path-normalization helper, not the binary's real code paths.

**Two root causes:**

1. **Server/Client fixtures are Unix-domain-socket shaped.** `ServerTest` and
   `ClientTest` SetUp build a `.sock` file path (`temp_directory_path() /
   "lci_test_N.sock"`) and `make_client()` calls `httplib::Client::
   set_address_family(AF_UNIX)`. On Windows the transport is TCP
   (`localhost:<port>`); `server.cpp` parses the port with
   `stoi(sock.substr(sock.rfind(':')+1))`, and a Windows file path's
   drive-letter colon (`C:\…`) makes that `stoi` throw "invalid argument" in
   `SetUp()`, failing every test in both fixtures. Real `lci` is fine — it gets
   its address from `get_socket_path*()`, which already returns
   `localhost:<port>` on Windows.

   **To close:** give the fixtures a platform-appropriate address — on Windows
   a unique `localhost:<port>` (per-fixture port from a static counter) and a
   TCP `httplib::Client` (no `AF_UNIX`); on POSIX keep the `.sock` path. Then
   drop `ServerTest.*` / `ClientTest.*` from the Windows filter.

2. **`report_to_json` path relativisation is POSIX-shaped.** `GitReportToJson`,
   `CodeInsightGitTest`, the MCP `*HandlerFixture`s, and `ExploreIndexTestFixture`
   feed `/tmp/...`-rooted fixture paths and expect `report_to_json` to strip the
   root to a forward-slash relative path (`src/foo.cpp`). Under Windows
   `std::filesystem`, a `/`-rooted path relativises differently, so the result
   keeps a leading `/`. **To close:** make the git-report path relativisation
   operate on normalized forward-slash strings independent of the host
   `std::filesystem` separator semantics, then drop those suites from the
   Windows filter.

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
