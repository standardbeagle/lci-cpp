# Windows + macOS Port — Deferred Follow-ups

Tracked corners deliberately not closed in the Windows+macOS compilation/release
work (`2026-06-09-windows-macos-release.md`). Each is gated, not silently
dropped (karpathy #6). Tag when filed in Dart: `loop-fix`.

## 1. Port the parity / integration test runner to Windows

**What:** `tests/parity/runner/modes/{cli,mcp,http}.cpp` and
`tests/parity/runner/modes/child_guard.h` spawn child processes with the POSIX
`fork` / `dup2` / `execvp` / `waitpid` / `kill` pattern and have no Windows
implementation. They are gated out in `tests/CMakeLists.txt`:

```cmake
if(NOT WIN32)
    add_executable(lci_integration_tests ...)
    add_executable(lci_real_project_tests ...)
endif()
# ...
if(NOT WIN32)
    add_subdirectory(parity)
endif()
```

**Why deferred:** The enforced Windows green bar is the unit suite (`lci_tests`)
plus `spec_diff_unit_tests`, matching the Linux release gate. The integration
runner needs an async child-process abstraction (non-blocking pipe reads,
timeout, signal-equivalent kill) that the current synchronous
`lci::subprocess` does not expose.

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
