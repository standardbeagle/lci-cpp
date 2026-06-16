# Windows + macOS Compilation & Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Subagent model policy (user directive):** dispatch implementation subagents with `model: "sonnet"` for code tasks and `model: "haiku"` for mechanical edits (YAML, test path swaps, comment updates). Verification/review stays in the main loop.

**Goal:** Make the Windows (MSVC) and macOS (AppleClang) CI legs green and add both platforms to the tagged release workflow — Windows first.

**Architecture:** Introduce two small portability modules (`lci::portable` for time/pid/exe-path/double-parse, `lci::subprocess` for shell-free process spawning), migrate the ~12 POSIX-only call sites onto them, gate the fork/exec-based parity runner out of Windows builds (filed as follow-up), and extend `release.yml` with `build-windows` and `build-macos` jobs. Asset names stay cpack defaults because `install.ps1`, `install.sh`, and `updater.cpp::select_asset` already contract on `*win64*.tar.gz` / `*Darwin.tar.gz` / `*Linux.tar.gz`.

**Tech Stack:** C++20 (`<bit>`, `__cpp_lib_atomic_shared_ptr` feature test), CMake 3.25 presets, vcpkg manifest (pinned commit `f3e10653`), GitHub Actions (self-hosted Windows runner `[self-hosted, windows, lci-windows]`, GitHub-hosted `macos-latest`), CPack TGZ.

---

## Ground truth (from CI run 27209456924, 2026-06-09)

**Windows MSVC build errors (all of them):**

| Site | Error |
|---|---|
| `src/analysis/call_graph.cpp:171` | `__builtin_ctzll` not found |
| `src/mcp/handlers_index.cpp:73,77` | `localtime_r`/`gmtime_r` not found |
| `src/mcp/handlers_context.cpp:37,40` | `localtime_r`/`gmtime_r` not found |
| `src/git/frequency_analyzer.cpp:422` | `gmtime_r` not found |
| `src/git/provider.cpp:70,79` | `popen`/`pclose` not found |
| `src/update/updater.cpp:57,64` | `popen`/`pclose` not found |
| `src/update/updater.cpp:472` | `getpid` not found |
| `tests/lib/spec_diff/tests/assert_matches_test.cpp:23` | `mkstemp` not found |
| `tests/parity/runner/modes/child_guard.h:28` | `sys/wait.h` missing |
| `tests/parity/runner/modes/index.cpp:20,22` | `mkstemp`/`close` not found |

**macOS AppleClang/libc++ build errors:**

| Site | Error |
|---|---|
| 6× `std::atomic<std::shared_ptr<…>>` members | libc++ has not implemented C++20 `atomic<shared_ptr>` (`_Atomic cannot be applied … not trivially copyable`) |
| `src/config/config.cpp:145` + 3 sites in `src/core/semantic_annotator.cpp` (322, 352, plus 333 is `int` — fine) | floating-point `from_chars` deleted in libc++ |
| `__compressed_pair` static_cast errors on `make_shared<FileContentStore>` | cascade from the `atomic<shared_ptr<FileContentSnapshot>>` member failing to instantiate — expected to clear with the atomic fix; re-check after Task 10 |

Also latent (build stopped before reaching it): `src/cli/cli_core.cpp:71` uses glibc-only `program_invocation_name` → fails on macOS too.

## Decisions

- **D1 — No shell, ever.** `run_git` and the updater currently build `/bin/sh -c` strings (`cd X && git … 2>/dev/null`). Replace with argv-based `lci::subprocess` (posix_spawnp on POSIX, CreateProcessW on Windows). This deletes `shell_quote` and the whole injection class (see memory: run-git shell injection).
- **D2 — Windows test gate = unit suite (`lci_tests`) + spec_diff unit tests**, same enforced green bar as the Linux release leg. The parity runner (`tests/parity/runner/modes/{cli,mcp,http}.cpp`, `child_guard.h`) is fork/exec end-to-end; porting it is a separate task filed in Dart with `loop-fix` tag (karpathy #6: skipped corner = bug filed, never silent). Gate with `if(NOT WIN32)` + comment referencing the Dart task.
- **D3 — Asset names stay cpack defaults**: `lci-<ver>-win64.tar.gz`, `lci-<ver>-Darwin.tar.gz`. Both `install.ps1:39` (regex `win64|windows` + `.tar.gz`) and `updater.cpp::select_asset` already match these. **Do not introduce ZIP.** Windows 10+ ships `tar.exe`; install.ps1 already uses it.
- **D4 — macOS ships arm64-only** (`macos-latest` native). Universal binary needs a vcpkg universal triplet (abseil/re2 multi-arch) — out of scope, filed as follow-up. Risk documented: `install.sh` Darwin branch doesn't arch-check; an x86_64 mac would fetch a non-runnable binary. Acceptable for first mac release; note it in release notes.
- **D5 — `AtomicSharedPtr<T>` wrapper**, native `std::atomic<std::shared_ptr<T>>` where `__cpp_lib_atomic_shared_ptr` is defined (libstdc++, MSVC), `std::atomic_load/store_explicit` free functions on libc++. Call sites keep `.load()/.store()` spelling — drop-in member-type swap, zero hot-path change on Linux. (Trigram/Postings RCU stays tsan-clean; re-run tsan preset after Task 10.)

**Baseline floor (karpathy #1):** before starting, record `./build/release/tests/lci_tests` pass count on Linux (expected: 1713 green per memory). Every task ends with this suite green. Final check re-runs tsan preset on `lci_tests`.

---

## File structure

| File | Responsibility |
|---|---|
| `include/lci/core/portable.h` + `src/core/portable.cpp` | time_r wrappers, `process_id()`, `executable_path()`, `parse_double()` |
| `include/lci/core/subprocess.h` + `src/core/subprocess.cpp` | argv-based `run_capture`, `run_status`, `spawn_detached` |
| `include/lci/core/atomic_shared_ptr.h` | `AtomicSharedPtr<T>` (D5) |
| `include/lci/mcp/time_format.h` | shared `format_rfc3339_nano_local` (kills the documented duplicate in handlers_context.cpp:26-29) |
| `tests/portable_test.cpp`, `tests/subprocess_test.cpp` | unit tests, real processes / real clock |

Modified: `call_graph.cpp`, `provider.cpp`, `updater.cpp`, `cli_core.cpp`, `cli/server.cpp`, `handlers_index.cpp`, `handlers_context.cpp`, `frequency_analyzer.cpp`, `config.cpp`, `semantic_annotator.cpp`, 5 index headers + `postings` header, `tests/CMakeLists.txt`, 4 test files, `spec_runner.cpp`, `.github/workflows/ci.yml`, `.github/workflows/release.yml`, `src/CMakeLists.txt` (add new .cpp files to `lci_lib`).

---

## Phase 1 — Portable foundation (all verifiable on Linux)

### Task 1: `lci::portable` module

**Files:**
- Create: `include/lci/core/portable.h`
- Create: `src/core/portable.cpp`
- Create: `tests/portable_test.cpp`
- Modify: `src/CMakeLists.txt` (add `core/portable.cpp` to the `lci_lib` source list — find the existing `core/` entries and append alphabetically)
- Modify: `tests/CMakeLists.txt:24-70` (add `portable_test.cpp` to `lci_tests` sources)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/portable_test.cpp
#include <gtest/gtest.h>

#include <lci/core/portable.h>

#include <ctime>
#include <filesystem>

namespace lci {
namespace {

TEST(PortableTest, GmtimeUtcMatchesKnownEpoch) {
    std::tm tm{};
    ASSERT_TRUE(portable::gmtime_utc(0, tm));
    EXPECT_EQ(tm.tm_year, 70);
    EXPECT_EQ(tm.tm_mon, 0);
    EXPECT_EQ(tm.tm_mday, 1);
    EXPECT_EQ(tm.tm_hour, 0);
}

TEST(PortableTest, LocaltimeLocalProducesValidTm) {
    std::tm tm{};
    ASSERT_TRUE(portable::localtime_local(std::time(nullptr), tm));
    EXPECT_GE(tm.tm_year, 126);  // >= 2026
}

TEST(PortableTest, ProcessIdIsPositiveAndStable) {
    int pid = portable::process_id();
    EXPECT_GT(pid, 0);
    EXPECT_EQ(pid, portable::process_id());
}

TEST(PortableTest, ExecutablePathPointsAtTestBinary) {
    auto p = portable::executable_path();
    EXPECT_TRUE(std::filesystem::exists(p));
    EXPECT_NE(p.filename().string().find("lci_tests"), std::string::npos);
}

TEST(PortableTest, ParseDoubleBasic) {
    double v = 0;
    EXPECT_TRUE(portable::parse_double("0.85", v));
    EXPECT_DOUBLE_EQ(v, 0.85);
    EXPECT_TRUE(portable::parse_double("3", v));
    EXPECT_DOUBLE_EQ(v, 3.0);
}

TEST(PortableTest, ParseDoubleRejectsGarbage) {
    double v = 42.0;
    EXPECT_FALSE(portable::parse_double("", v));
    EXPECT_FALSE(portable::parse_double("abc", v));
}

TEST(PortableTest, ParseDoubleAcceptsNumericPrefix) {
    // Mirrors std::from_chars semantics relied on by semantic_annotator:
    // a valid numeric prefix parses successfully.
    double v = 0;
    EXPECT_TRUE(portable::parse_double("1.5x", v));
    EXPECT_DOUBLE_EQ(v, 1.5);
}

}  // namespace
}  // namespace lci
```

- [ ] **Step 2: Run to verify it fails to compile** — `cmake --build build/release --parallel --target lci_tests` → expected: `lci/core/portable.h: No such file or directory`.

- [ ] **Step 3: Implement**

```cpp
// include/lci/core/portable.h
#pragma once

#include <ctime>
#include <filesystem>
#include <string_view>

namespace lci {
namespace portable {

/// Thread-safe gmtime. MSVC only has gmtime_s (reversed args); POSIX has
/// gmtime_r. Returns false if conversion failed.
inline bool gmtime_utc(std::time_t t, std::tm& out) {
#if defined(_WIN32)
    return gmtime_s(&out, &t) == 0;
#else
    return gmtime_r(&t, &out) != nullptr;
#endif
}

/// Thread-safe localtime, same contract as gmtime_utc.
inline bool localtime_local(std::time_t t, std::tm& out) {
#if defined(_WIN32)
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

/// Current process id (getpid / _getpid).
int process_id();

/// Absolute path of the running executable. Throws std::runtime_error on
/// failure (fail fast — no caller has a sane fallback).
std::filesystem::path executable_path();

/// Locale-independent string->double. Exists because libc++ (macOS) deletes
/// floating-point std::from_chars. Same accept-a-numeric-prefix semantics as
/// from_chars: returns true if a value was parsed, even with trailing bytes.
bool parse_double(std::string_view text, double& out);

}  // namespace portable
}  // namespace lci
```

```cpp
// src/core/portable.cpp
#include <lci/core/portable.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>

#include <vector>
#else
#include <unistd.h>
#endif

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <version>

namespace lci {
namespace portable {

int process_id() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

std::filesystem::path executable_path() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    return std::filesystem::path(buf);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // queries required size
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        throw std::runtime_error("_NSGetExecutablePath failed");
    }
    std::error_code ec;
    auto canon = std::filesystem::canonical(buf.data(), ec);
    return ec ? std::filesystem::path(buf.data()) : canon;
#else
    std::error_code ec;
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        throw std::runtime_error("cannot resolve /proc/self/exe: " +
                                 ec.message());
    }
    return path;
#endif
}

bool parse_double(std::string_view text, double& out) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(),
                                     out);
    return ec == std::errc{};
#else
    // libc++ has no floating-point from_chars. strtod is locale-sensitive,
    // but lci never calls setlocale, so LC_NUMERIC stays "C".
    char buf[64];
    if (text.empty() || text.size() >= sizeof(buf)) return false;
    std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
    char* end = nullptr;
    double v = std::strtod(buf, &end);
    if (end == buf) return false;
    out = v;
    return true;
#endif
}

}  // namespace portable
}  // namespace lci
```

- [ ] **Step 4: Run** `cmake --build build/release --parallel --target lci_tests && ./build/release/tests/lci_tests --gtest_filter='PortableTest.*'` → expected: 7 PASS.
- [ ] **Step 5: Full unit suite** `./build/release/tests/lci_tests` → green (floor check).
- [ ] **Step 6: Commit** `git add -A && git commit -m "feat(core): portable time/pid/exe-path/parse_double layer"`

### Task 2: `__builtin_ctzll` → `std::countr_zero`

**Files:** Modify: `src/analysis/call_graph.cpp:171` (+ includes block at top of file)

- [ ] **Step 1:** Add `#include <bit>` to the include block of `src/analysis/call_graph.cpp` (keep alphabetical order with the other standard headers).
- [ ] **Step 2:** Replace line 171:

```cpp
// before
                int bit = __builtin_ctzll(b);
// after
                int bit = std::countr_zero(b);
```

(`std::countr_zero` on x86-64 compiles to the identical `tzcnt`; zero perf change. `b` is `uint64_t`, never 0 inside `while (b)` — same precondition as before.)

- [ ] **Step 3:** `cmake --build build/release --parallel --target lci_tests && ./build/release/tests/lci_tests --gtest_filter='*CallGraph*:*Analysis*'` → PASS, then full `./build/release/tests/lci_tests` → green.
- [ ] **Step 4: Commit** `git commit -am "fix(analysis): std::countr_zero for MSVC (no __builtin_ctzll)"`

### Task 3: `lci::subprocess` module

**Files:**
- Create: `include/lci/core/subprocess.h`
- Create: `src/core/subprocess.cpp`
- Create: `tests/subprocess_test.cpp`
- Modify: `src/CMakeLists.txt` (add `core/subprocess.cpp`), `tests/CMakeLists.txt` (add `subprocess_test.cpp`)

- [ ] **Step 1: Write the failing test** (real processes, no mocks — karpathy #5)

```cpp
// tests/subprocess_test.cpp
#include <gtest/gtest.h>

#include <lci/core/subprocess.h>

#include <filesystem>

namespace lci {
namespace {

namespace fs = std::filesystem;

TEST(SubprocessTest, CapturesStdout) {
    std::string out;
    ASSERT_TRUE(subprocess::run_capture({"git", "--version"}, "", out));
    EXPECT_NE(out.find("git version"), std::string::npos);
}

TEST(SubprocessTest, NonZeroExitReturnsFalse) {
    std::string out;
    // `git rev-parse --show-toplevel` outside any repo exits non-zero.
    EXPECT_FALSE(subprocess::run_capture(
        {"git", "-C", fs::temp_directory_path().string(), "rev-parse",
         "--show-toplevel"},
        "", out));
}

TEST(SubprocessTest, MissingBinaryReturnsFalse) {
    std::string out;
    EXPECT_FALSE(subprocess::run_capture({"lci-no-such-binary-xyzzy"}, "", out));
}

TEST(SubprocessTest, CwdIsApplied) {
    auto tmp = fs::temp_directory_path();
    std::string out;
#if defined(_WIN32)
    ASSERT_TRUE(subprocess::run_capture({"cmd.exe", "/c", "cd"}, tmp.string(), out));
#else
    ASSERT_TRUE(subprocess::run_capture({"pwd"}, tmp.string(), out));
#endif
    // Compare canonically: /tmp may be a symlink (macOS /private/tmp).
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    EXPECT_EQ(fs::canonical(out), fs::canonical(tmp));
}

TEST(SubprocessTest, ArgsWithSpacesAndQuotesSurvive) {
    std::string out;
    // echo through git's own arg handling: `git config --get` with an
    // impossible key just exits 1; instead use --version style echo via
    // `git -c x.y="a b" config x.y` round-trip in a scratch repo? Too heavy.
    // Simplest cross-platform arg-integrity probe: cmake -E echo.
    ASSERT_TRUE(subprocess::run_capture(
        {"cmake", "-E", "echo", "a b", "c\"d", "%PATH%", "$HOME"}, "", out));
    EXPECT_NE(out.find("a b"), std::string::npos);
    EXPECT_NE(out.find("c\"d"), std::string::npos);
    EXPECT_NE(out.find("%PATH%"), std::string::npos);  // no env expansion
    EXPECT_NE(out.find("$HOME"), std::string::npos);   // no shell involved
}

TEST(SubprocessTest, RunStatusReturnsExitCode) {
    EXPECT_EQ(subprocess::run_status({"cmake", "-E", "true"}), 0);
    EXPECT_NE(subprocess::run_status({"cmake", "-E", "false"}), 0);
    EXPECT_EQ(subprocess::run_status({"lci-no-such-binary-xyzzy"}), -1);
}

}  // namespace
}  // namespace lci
```

- [ ] **Step 2:** Build → expected compile failure (header missing).
- [ ] **Step 3: Implement**

```cpp
// include/lci/core/subprocess.h
#pragma once

#include <string>
#include <vector>

namespace lci {
namespace subprocess {

/// Runs argv[0] (PATH-searched) with the given args in `cwd` (empty =
/// inherit), capturing stdout into `out`. stderr is discarded. No shell is
/// involved on any platform — args are passed verbatim, so there is no
/// quoting/injection surface. Returns true iff the process spawned and
/// exited 0.
bool run_capture(const std::vector<std::string>& argv, const std::string& cwd,
                 std::string& out);

/// Runs argv inheriting stdio. Returns the child's exit code, or -1 if the
/// process could not be spawned (or died on a signal).
int run_status(const std::vector<std::string>& argv);

/// Spawns argv as a fully detached background process (new session on POSIX,
/// no console on Windows; stdio redirected to the null device). Returns true
/// on successful spawn. Used to launch the index server daemon.
bool spawn_detached(const std::vector<std::string>& argv);

}  // namespace subprocess
}  // namespace lci
```

```cpp
// src/core/subprocess.cpp
#include <lci/core/subprocess.h>

#include <array>
#include <cstdio>

#if defined(_WIN32)

#include <windows.h>

namespace lci {
namespace subprocess {
namespace {

// Quotes one argument per the MSVC CRT argv parsing rules
// (the documented "ArgvQuote" algorithm). CreateProcessW takes a single
// command line; the child's CRT re-splits it, so quoting must round-trip.
void append_quoted(std::wstring& cmd, const std::wstring& arg) {
    if (!cmd.empty()) cmd += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
            cmd += *it;
        } else {
            cmd.append(backslashes, L'\\');
            cmd += *it;
        }
    }
    cmd += L'"';
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

std::wstring build_cmdline(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (const auto& a : argv) append_quoted(cmd, widen(a));
    return cmd;
}

// Spawns with the given stdout handle (or null). Returns process handle or
// nullptr.
HANDLE spawn(const std::vector<std::string>& argv, const std::string& cwd,
             HANDLE out_write, bool detached) {
    std::wstring cmd = build_cmdline(argv);
    std::wstring wcwd = widen(cwd);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE null_dev = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, 0, &sa,
                                  OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = null_dev;
    si.hStdOutput = out_write != nullptr ? out_write : null_dev;
    si.hStdError = null_dev;

    DWORD flags = CREATE_NO_WINDOW;
    if (detached) flags |= DETACHED_PROCESS;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                             /*bInheritHandles=*/TRUE, flags, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    if (null_dev != INVALID_HANDLE_VALUE) CloseHandle(null_dev);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

}  // namespace

bool run_capture(const std::vector<std::string>& argv, const std::string& cwd,
                 std::string& out) {
    out.clear();
    if (argv.empty()) return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);  // parent end stays ours

    HANDLE proc = spawn(argv, cwd, wr, /*detached=*/false);
    CloseHandle(wr);
    if (proc == nullptr) {
        CloseHandle(rd);
        return false;
    }

    std::array<char, 4096> buf{};
    DWORD n = 0;
    while (ReadFile(rd, buf.data(), static_cast<DWORD>(buf.size()), &n,
                    nullptr) &&
           n > 0) {
        out.append(buf.data(), n);
    }
    CloseHandle(rd);

    WaitForSingleObject(proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    return code == 0;
}

int run_status(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    std::wstring cmd = build_cmdline(argv);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

bool spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    HANDLE proc = spawn(argv, "", nullptr, /*detached=*/true);
    if (proc == nullptr) return false;
    CloseHandle(proc);
    return true;
}

}  // namespace subprocess
}  // namespace lci

#else  // POSIX

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace lci {
namespace subprocess {
namespace {

// Builds a null-terminated char* view of argv. Lifetimes: borrowed from the
// caller's strings; valid until the vector<string> mutates.
std::vector<char*> to_cargv(const std::vector<std::string>& argv) {
    std::vector<char*> v;
    v.reserve(argv.size() + 1);
    for (const auto& a : argv) v.push_back(const_cast<char*>(a.c_str()));
    v.push_back(nullptr);
    return v;
}

}  // namespace

bool run_capture(const std::vector<std::string>& argv, const std::string& cwd,
                 std::string& out) {
    out.clear();
    if (argv.empty()) return false;

    int fds[2];
    if (pipe(fds) != 0) return false;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY,
                                     0);
    if (!cwd.empty()) {
        // posix_spawn_file_actions_addchdir_np: glibc 2.29+, macOS 10.15+.
        posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    }

    auto cargv = to_cargv(argv);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return false;
    }

    std::array<char, 4096> buf{};
    ssize_t n = 0;
    while ((n = read(fds[0], buf.data(), buf.size())) > 0) {
        out.append(buf.data(), static_cast<size_t>(n));
    }
    close(fds[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int run_status(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    auto cargv = to_cargv(argv);
    pid_t pid = -1;
    if (posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(),
                     environ) != 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

bool spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY,
                                     0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY,
                                     0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY,
                                     0);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif

    auto cargv = to_cargv(argv);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    return rc == 0;
}

}  // namespace subprocess
}  // namespace lci

#endif
```

Note: `posix_spawnp` (not fork) — the server process is heavily threaded; fork-in-threaded-process for the updater path is a latent deadlock. `addchdir_np` exists on glibc ≥2.29 and macOS ≥10.15 (both true for the supported floor: DEB requires glibc ≥2.35).

- [ ] **Step 4:** `cmake --build build/release --parallel --target lci_tests && ./build/release/tests/lci_tests --gtest_filter='SubprocessTest.*'` → 6 PASS.
- [ ] **Step 5:** Full `./build/release/tests/lci_tests` → green.
- [ ] **Step 6: Commit** `git commit -am "feat(core): shell-free subprocess module (posix_spawn / CreateProcessW)"`

---

## Phase 2 — Windows compile fixes (production code)

### Task 4: shared RFC3339 formatter + portable time calls

**Files:**
- Create: `include/lci/mcp/time_format.h`
- Modify: `src/mcp/handlers_index.cpp:62-94` (delete local `format_rfc3339_nano_local`, include new header)
- Modify: `src/mcp/handlers_context.cpp:23-54` (delete the documented duplicate — resolves the loop-fix note from DART-2PPeRKfyrceR — include new header)
- Modify: `src/git/frequency_analyzer.cpp:420-422`

- [ ] **Step 1:** Create the shared header (body is the existing handlers_index implementation verbatim, with the two `_r` calls swapped to `portable::`):

```cpp
// include/lci/mcp/time_format.h
#pragma once

#include <lci/core/portable.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace lci {
namespace mcp {

/// Formats a system_clock time_point as RFC3339Nano with local timezone offset
/// (matches Go's time.Time JSON marshal: "2026-05-13T17:12:53.528955893-05:00").
/// Zero-alloc on hot path: writes into a fixed 48-byte stack buffer then a
/// single std::string move out.
inline std::string format_rfc3339_nano_local(
    std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto secs = time_point_cast<seconds>(tp);
    auto ns = duration_cast<nanoseconds>(tp - secs).count();
    std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
    portable::localtime_local(t, tm);

    // Compute local zone offset from UTC.
    std::tm utm{};
    portable::gmtime_utc(t, utm);
    // Difference in seconds: treat both tms as if UTC for difftime.
    std::time_t lt = std::mktime(&tm);
    std::time_t ut = std::mktime(&utm);
    long offset = static_cast<long>(lt - ut);
    char sign = offset < 0 ? '-' : '+';
    long abs_off = offset < 0 ? -offset : offset;
    int oh = static_cast<int>(abs_off / 3600);
    int om = static_cast<int>((abs_off % 3600) / 60);

    char buf[48];
    int n = std::snprintf(buf, sizeof(buf),
                          "%04d-%02d-%02dT%02d:%02d:%02d.%09ld%c%02d:%02d",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec,
                          static_cast<long>(ns), sign, oh, om);
    if (n <= 0) return std::string{};
    return std::string(buf, static_cast<size_t>(n));
}

}  // namespace mcp
}  // namespace lci
```

- [ ] **Step 2:** In `handlers_index.cpp` and `handlers_context.cpp`: delete each file's local `format_rfc3339_nano_local` (including the duplication comment block in handlers_context), add `#include <lci/mcp/time_format.h>`. Call sites keep working unqualified if the handlers are inside `namespace lci::mcp` (they are — verify; otherwise qualify as `mcp::format_rfc3339_nano_local`).
- [ ] **Step 3:** In `frequency_analyzer.cpp`, add `#include <lci/core/portable.h>` and replace line 422:

```cpp
// before
    gmtime_r(&t, &tm_buf);
// after
    portable::gmtime_utc(t, tm_buf);
```

- [ ] **Step 4:** Build + run `./build/release/tests/lci_tests --gtest_filter='*McpHandlers*:*Frequency*:*GitAnalysis*'` → PASS (existing handler tests cover the timestamp format), then full suite green.
- [ ] **Step 5: Commit** `git commit -am "refactor(mcp,git): shared RFC3339 formatter on portable time (MSVC fix + dedup)"`

### Task 5: `Provider::run_git` → subprocess (delete shell_quote)

**Files:** Modify: `src/git/provider.cpp:42-80`

- [ ] **Step 1:** Replace the anonymous namespace block at lines 42-58 (the whole `shell_quote` + its comment) and `run_git` at 60-80 with:

```cpp
bool Provider::run_git(const std::vector<std::string>& args,
                       std::string& out) const {
    // No shell: args go to git verbatim (subprocess::run_capture), so
    // `--format=%H|%an|%at` needs no quoting and cannot be injected.
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.emplace_back("git");
    argv.insert(argv.end(), args.begin(), args.end());
    return subprocess::run_capture(argv, repo_root_, out);
}
```

Add `#include <lci/core/subprocess.h>` to the includes; remove now-unused `<cstdio>` and `<array>` if nothing else in the file uses them.

- [ ] **Step 2:** Build + run `./build/release/tests/lci_tests --gtest_filter='*Git*'` → PASS (git_test runs against real git; the `%an|%ae` pipe-format cases from the shell-injection fix must stay green). Full suite green.
- [ ] **Step 3: Commit** `git commit -am "refactor(git): run_git via shell-free subprocess; delete shell_quote"`

### Task 6: updater → subprocess + portable

**Files:** Modify: `src/update/updater.cpp` (lines 53-83, 472, and every `run_capture`/`run_cmd` call site)

- [ ] **Step 1:** Enumerate call sites: `grep -n "run_capture\|run_cmd(" src/update/updater.cpp`. Known sites: `have_tool` (line 80-83), checksum commands (~line 144: `sha256sum` / `shasum -a 256` / `certutil` Windows branch), curl download (~line 483), tar extract (run_cmd). Each currently builds a quoted command string.
- [ ] **Step 2:** Change signatures to argv form and delegate:

```cpp
// replaces lines 53-66
// Run a command, capturing stdout. Returns false if the process could not be
// started or exited non-zero.
bool run_capture(const std::vector<std::string>& argv, std::string& out) {
    return lci::subprocess::run_capture(argv, "", out);
}

// replaces lines 68-78
// Run a command for its exit code (stdout/stderr inherit the terminal).
int run_cmd(const std::vector<std::string>& argv) {
    return lci::subprocess::run_status(argv);
}

bool have_tool(const std::string& name) {
    std::string out;
    return run_capture({name, "--version"}, out);
}
```

- [ ] **Step 3:** Convert each call site from string to argv. The download site (lines 483-485) becomes:

```cpp
    std::cout << "Downloading " << asset->name << " (" << latest << ")...\n";
    if (run_cmd({"curl", "-fsSL", "--proto", "=https", "-o", tarball.string(),
                 asset->url}) != 0) {
```

The tar/checksum sites follow the same mechanical pattern — keep the exact tool names and flags currently in each branch, split on the spaces that today separate args, and pass former-quoted paths as single argv elements (drop the `\"` wrapping). `WIFEXITED`/`WEXITSTATUS` and the `#if defined(_WIN32)` in old `run_cmd` disappear (subprocess handles it).

- [ ] **Step 4:** Replace line 472:

```cpp
// before
    fs::path work = fs::temp_directory_path(ec) /
                    ("lci-update-" + std::to_string(::getpid()));
// after
    fs::path work = fs::temp_directory_path(ec) /
                    ("lci-update-" + std::to_string(portable::process_id()));
```

Add `#include <lci/core/portable.h>` and `#include <lci/core/subprocess.h>`; remove `<cstdio>`/popen leftovers.

- [ ] **Step 5:** Build + `./build/release/tests/lci_tests --gtest_filter='*Updater*'` → PASS; full suite green.
- [ ] **Step 6: Commit** `git commit -am "refactor(update): argv subprocess + portable pid (MSVC popen/getpid fix)"`

### Task 7: `is_mcp_mode` portability (Windows + macOS)

**Files:** Modify: `src/cli/cli_core.cpp:58-100`

- [ ] **Step 1:** Restructure the function. The Linux `/proc/<ppid>/comm` parent check stays Linux-only; arg0 goes portable; stdin-pipe detection gets a Windows branch:

```cpp
bool is_mcp_mode() {
    if (const char* env = std::getenv("LCI_MCP_MODE")) {
        std::string val(env);
        if (val == "1" || val == "true") return true;
    }

#if defined(_WIN32)
    // stdin is a pipe/file (not a console) => almost certainly an MCP client.
    DWORD type = GetFileType(GetStdHandle(STD_INPUT_HANDLE));
    if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) return true;
#else
    struct stat st {};
    if (fstat(STDIN_FILENO, &st) == 0 && !S_ISCHR(st.st_mode)) {
        return true;
    }
#endif

    {
#if defined(__GLIBC__)
        std::string arg0 =
            fs::path(program_invocation_name).filename().string();
#elif defined(__APPLE__)
        std::string arg0 = getprogname();
#elif defined(_WIN32)
        std::string arg0;
        try {
            arg0 = portable::executable_path().filename().string();
        } catch (const std::runtime_error&) {
            // No exe name available; fall through to remaining heuristics.
        }
#endif
        for (auto& c : arg0) c = static_cast<char>(std::tolower(c));
        if (arg0.find("mcp") != std::string::npos ||
            arg0.find("server") != std::string::npos) {
            return true;
        }
    }

#if defined(__linux__)
    {
        pid_t ppid = getppid();
        if (ppid > 1) {
            std::string comm_path =
                "/proc/" + std::to_string(ppid) + "/comm";
            std::ifstream f(comm_path);
            if (f) {
                std::string parent_name;
                std::getline(f, parent_name);
                for (auto& c : parent_name)
                    c = static_cast<char>(std::tolower(c));
                static constexpr std::string_view kMcpClients[] = {
                    "mcp-tui", "mcp-client", "claude", "cursor", "vscode"};
                for (auto client : kMcpClients) {
                    if (parent_name.find(client) != std::string::npos)
                        return true;
                }
            }
        }
    }
#endif
```

(Keep whatever follows line 100 — the function's tail — unchanged.) Header block at lines 9-12 becomes:

```cpp
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <lci/core/portable.h>
#include <cctype>
```

- [ ] **Step 2:** Build + `./build/release/tests/lci_tests --gtest_filter='*Cli*'` → PASS; full suite green.
- [ ] **Step 3: Commit** `git commit -am "fix(cli): portable is_mcp_mode (Win32 pipe check, getprogname on macOS)"`

### Task 8: server spawn via `spawn_detached` + `executable_path`

**Files:** Modify: `src/cli/server.cpp:59-100` (ensure_server_running) and the daemon block at lines 143-194 (run_server)

- [ ] **Step 1:** Replace the `#ifndef _WIN32` fork/execl block in `ensure_server_running` (lines 59-100, including the `#else` Windows stub) with platform-neutral code:

```cpp
    std::filesystem::path exe;
    try {
        exe = portable::executable_path();
    } catch (const std::runtime_error& e) {
        error = std::string("failed to get executable path: ") + e.what();
        return nullptr;
    }

    std::vector<std::string> argv{exe.string()};
    if (!cfg.project.root.empty() && cfg.project.root != ".") {
        argv.push_back("--root");
        argv.push_back(cfg.project.root);
    }
    argv.push_back("server");

    if (!subprocess::spawn_detached(argv)) {
        error = "failed to spawn background server process";
        return nullptr;
    }
```

- [ ] **Step 2:** Apply the same transformation to the double-fork daemon block at lines 143-194 (it builds an argv `raw` vector for `execv` already — reuse that argument list, swap the fork/setsid/execv machinery for one `subprocess::spawn_detached(argv)` call, exe from `portable::executable_path()`). Delete the `/proc/self/exe` reads at lines 62 and 149.
- [ ] **Step 3:** `SIGTERM`/`SIGINT` handlers at lines 212-213: wrap in `#ifndef _WIN32` and add a Windows branch using only `std::signal(SIGINT, …)` + `std::signal(SIGTERM, …)` — MSVC's CRT does define both SIGINT and SIGTERM, so first try leaving the two lines unguarded; only add guards if MSVC rejects them.
- [ ] **Step 4:** Add includes `<lci/core/portable.h>`, `<lci/core/subprocess.h>`; drop now-unused `<unistd.h>` parts (the `::unlink` at line 49 stays POSIX-guarded — socket files don't exist on Windows).
- [ ] **Step 5:** Build; run the server-lifecycle integration check locally (real corpus, karpathy #5):

```bash
./build/release/tests/lci_tests --gtest_filter='*Server*:*Client*'
ctest --test-dir build/release -R lci_integration_suite --output-on-failure
```

Expected: PASS — the background-start path is exercised by integration tests. Full unit suite green.
- [ ] **Step 6: Commit** `git commit -am "feat(cli): cross-platform background server spawn (spawn_detached)"`

---

## Phase 3 — Windows test-tree fixes + CI green

### Task 9: test sources portability + parity gating

**Files:**
- Modify: `tests/cli_test.cpp` (line 3 unistd include; `/tmp` literals at 39-43 and per `grep -n '"/tmp' tests/cli_test.cpp`)
- Modify: `tests/file_content_store_test.cpp:308,327,340` (and any other `getpid()`/`/tmp` hits in that file)
- Modify: `tests/server_test.cpp:217-228` (unguarded `sockaddr_un`)
- Modify: `tests/lib/spec_diff/tests/assert_matches_test.cpp:23` (mkstemp)
- Modify: `tests/integration/spec_runner.cpp:44-51` (ExecutablePath)
- Modify: `tests/CMakeLists.txt` (gate integration/real-project/parity on `NOT WIN32`)

- [ ] **Step 1:** `cli_test.cpp`: delete `#include <unistd.h>` (line 3). Replace the `/tmp` root-override test (lines 37-44):

```cpp
TEST(CliConfigTest, RootOverrideApplied) {
    GlobalFlags flags;
    auto tmp = std::filesystem::temp_directory_path();
    flags.root = tmp.string();
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(cfg.project.root,
              std::filesystem::absolute(tmp).string());
}
```

Sweep the rest of the file: `grep -n '"/tmp' tests/cli_test.cpp` (lines ~150, 167, 202, 218, 237) — replace each `"/tmp/lci_test_…"` construction with `(std::filesystem::temp_directory_path() / ("lci_test_…")).string()` keeping the same unique-suffix logic.

- [ ] **Step 2:** `file_content_store_test.cpp`: add `#include <lci/core/portable.h>`; replace each of the three temp paths, e.g.:

```cpp
    std::string path =
        (std::filesystem::temp_directory_path() /
         ("lci_mmap_test_" + std::to_string(lci::portable::process_id()) +
          ".txt"))
            .string();
```

(same pattern for `lci_mmap_empty_` and `lci_mmap_move_`).

- [ ] **Step 3:** `server_test.cpp`: wrap the `sockaddr_un` test at lines ~217-228 in `#ifndef _WIN32` / `#endif` — it asserts `sun_path` size limits which don't exist on the Windows TCP transport (the Win32 socket-path behavior is covered by the existing guarded tests at 171/196).
- [ ] **Step 4:** `assert_matches_test.cpp`: replace the `mkstemp` fixture with:

```cpp
#include <fstream>

namespace {
std::string write_temp_golden(const std::string& content) {
    static int counter = 0;
    auto p = std::filesystem::temp_directory_path() /
             ("spec_diff_golden_" + std::to_string(::testing::UnitTest::GetInstance()
                                                       ->random_seed()) +
              "_" + std::to_string(counter++) + ".txt");
    std::ofstream out(p, std::ios::binary);
    out << content;
    return p.string();
}
}  // namespace
```

and adapt the single call site (line ~23) to use it; delete the `mkstemp`/`close`/`unistd.h` usage.

- [ ] **Step 5:** `spec_runner.cpp:44-51`:

```cpp
#include <lci/core/portable.h>

fs::path ExecutablePath() {
    return lci::portable::executable_path();
}
```

- [ ] **Step 6:** `tests/CMakeLists.txt`: wrap the integration-test target (lines 114-144), real-project target (149-182), and `add_subdirectory(parity)` (line 262) in one guard each:

```cmake
# The integration suites and parity runner spawn children via fork/exec
# (tests/parity/runner/modes/*.cpp, child_guard.h) and have no Windows
# implementation yet. Gated, not stubbed: Windows port of the runner is
# tracked in Dart (task created in this plan's final phase, tag loop-fix).
# The enforced Windows green bar — like the Linux release gate — is the
# unit suite (lci_tests) plus spec_diff unit tests.
if(NOT WIN32)
    add_executable(lci_integration_tests …)   # existing block unchanged
    …
endif()
```

Also update the `foreach(_t IN ITEMS …)` warning-flags loop (line 241): it already checks `if(TARGET ${_t})`, so it needs no change. `add_subdirectory(lib/spec_diff)` stays unconditional (its unit tests must run on Windows).

- [ ] **Step 7:** Full Linux verification:

```bash
cmake --build build/release --parallel
./build/release/tests/lci_tests
ctest --test-dir build/release -R 'lci_integration_suite|spec_diff' --output-on-failure
```

Expected: all green — these edits must be invisible on Linux.
- [ ] **Step 8: Commit** `git commit -am "test: portable temp paths/pids; gate fork-exec parity runner off Windows"`

### Task 10: Windows CI leg — Release config + unit test gate

**Files:** Modify: `.github/workflows/ci.yml:119-155` (windows job)

- [ ] **Step 1:** The job uses the default Visual Studio generator (multi-config): `cmake --build build/release --parallel` without `--config` builds **Debug**. Fix the build step and add the test gate:

```yaml
      - name: Build
        run: cmake --build build/release --parallel --config Release

      - name: Test (unit suite)
        run: |
          build/release/tests/Release/lci_tests.exe
          build/release/tests/lib/spec_diff/Release/spec_diff_unit_tests.exe
```

(If the spec_diff binary lands at a different sub-path, locate it with `Get-ChildItem -Recurse -Filter spec_diff_unit_tests.exe build/release` in a debug run and fix the path — the target name is `spec_diff_unit_tests` per the vcxproj seen in the failure log.)

- [ ] **Step 2:** Commit and push the branch; open a PR (windows job runs on `pull_request`):

```bash
git push -u origin windows-mac-port
gh pr create --title "feat: Windows + macOS compilation and release legs" --draft --body "Plan: docs/superpowers/plans/2026-06-09-windows-macos-release.md"
gh run watch  # or: gh pr checks --watch
```

Expected: `windows msvc / release` **green** (build + 1700-ish unit tests). Linux legs stay green.
- [ ] **Step 3:** Iterate on any residual MSVC errors here — this is the checkpoint where unknown-unknowns surface (e.g. additional `unistd.h` includes inside `lci_tests` sources the failure log truncated). Fix root cause, never `#ifdef`-out a test body (karpathy: own all failures). Commit each fix separately.
- [ ] **Step 4: Commit** `git commit -am "ci(windows): build Release config, gate on unit suite"`

### Task 11: Windows release leg

**Files:** Modify: `.github/workflows/release.yml`

- [ ] **Step 1:** Add the job after `build-linux` (note: TGZ, not ZIP — `install.ps1:39` and `select_asset` match `*win64*.tar.gz`, D3):

```yaml
  build-windows:
    runs-on: [self-hosted, windows, lci-windows]
    name: build / lci-windows-amd64
    steps:
      - uses: actions/checkout@v4

      - name: Clean stale vcpkg
        run: if (Test-Path "${{ github.workspace }}/vcpkg") { Remove-Item -Recurse -Force "${{ github.workspace }}/vcpkg" }

      - name: Set up vcpkg
        uses: lukka/run-vcpkg@v11
        with:
          vcpkgGitCommitId: "f3e10653cc27d62a37a3763cd84b38bca07c6075"

      - name: Configure
        run: |
          cmake --preset release `
            -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

      - name: Build
        run: cmake --build build/release --parallel --config Release

      - name: Test (unit suite)
        run: build/release/tests/Release/lci_tests.exe

      - name: Package
        run: |
          cd build/release
          cpack -G TGZ -C Release

      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: lci-windows-amd64
          path: build/release/lci-*.tar.gz
          retention-days: 2

      - name: Reclaim build space
        if: always()
        run: if (Test-Path build/release) { Remove-Item -Recurse -Force build/release }
```

- [ ] **Step 2:** Update the publish job: `needs: [build-linux, build-windows]`. The SHA256SUMS `find` and the `files:` globs already cover `*.tar.gz` — no change needed there. Update the lines 92-97 NOTE comment: Windows is back; macOS still pending (until Phase 5).
- [ ] **Step 3:** Verify the expected artifact name locally: `grep -n "win64\|windows" install.ps1 src/update/updater.cpp` — cpack on Windows names the file `lci-<version>-win64.tar.gz`, which matches both matchers. Record this in the commit message.
- [ ] **Step 4: Commit** `git commit -am "ci(release): Windows build/test/package leg (lci-*-win64.tar.gz)"`

---

## Phase 4 — macOS compile fixes

### Task 12: `AtomicSharedPtr<T>` (libc++ has no `atomic<shared_ptr>`)

**Files:**
- Create: `include/lci/core/atomic_shared_ptr.h`
- Modify (member decl swap + include, `.load()/.store()` call sites unchanged):
  - `include/lci/core/trigram.h:180`
  - `include/lci/core/reference_tracker.h:189`
  - `include/lci/core/file_content_store.h:181`
  - `include/lci/indexing/master_index.h:217`
  - `include/lci/indexing/deleted_file_tracker.h:59`
  - plus every remaining hit of `grep -rn "std::atomic<std::shared_ptr" include/ src/` (the postings snapshot member lives in `include/lci/core/postings.h` — `src/core/postings.cpp:15-47` uses it)

- [ ] **Step 1: Create the wrapper**

```cpp
// include/lci/core/atomic_shared_ptr.h
#pragma once

#include <atomic>
#include <memory>
#include <version>

namespace lci {

/// std::atomic<std::shared_ptr<T>> is C++20, but libc++ (macOS/AppleClang)
/// has not implemented it. Where the native type exists (libstdc++, MSVC) we
/// use it; on libc++ we fall back to the atomic_load/store free functions.
/// Both give acquire/release publication of immutable snapshots — the RCU
/// read path stays a single atomic load either way. (Neither implementation
/// is formally lock-free for shared_ptr — all three stdlibs use a lock pool
/// for the control-block update — identical to what the code shipped with.)
#if defined(__cpp_lib_atomic_shared_ptr)

template <typename T>
class AtomicSharedPtr {
  public:
    std::shared_ptr<T> load(
        std::memory_order order = std::memory_order_seq_cst) const {
        return ptr_.load(order);
    }
    void store(std::shared_ptr<T> desired,
               std::memory_order order = std::memory_order_seq_cst) {
        ptr_.store(std::move(desired), order);
    }

  private:
    std::atomic<std::shared_ptr<T>> ptr_;
};

#else  // libc++ fallback

template <typename T>
class AtomicSharedPtr {
  public:
    std::shared_ptr<T> load(
        std::memory_order order = std::memory_order_seq_cst) const {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        return std::atomic_load_explicit(&ptr_, order);
#pragma clang diagnostic pop
    }
    void store(std::shared_ptr<T> desired,
               std::memory_order order = std::memory_order_seq_cst) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        std::atomic_store_explicit(&ptr_, std::move(desired), order);
#pragma clang diagnostic pop
    }

  private:
    std::shared_ptr<T> ptr_;
};

#endif

}  // namespace lci
```

- [ ] **Step 2:** Swap every declaration, e.g. `trigram.h:180`:

```cpp
// before
    std::atomic<std::shared_ptr<const Snapshot>> snapshot_;
// after
    AtomicSharedPtr<const Snapshot> snapshot_;
```

Add `#include <lci/core/atomic_shared_ptr.h>` to each header. Verify zero remaining hits: `grep -rn "std::atomic<std::shared_ptr" include/ src/` → empty.

- [ ] **Step 3:** Build + run the RCU-sensitive suites:

```bash
cmake --build build/release --parallel
./build/release/tests/lci_tests --gtest_filter='*Trigram*:*Postings*:*FileContentStore*:*MasterIndex*:*ReferenceTracker*:*DeletedFile*'
./build/release/tests/lci_tests
```

→ all green.
- [ ] **Step 4: TSan floor (trigram/postings RCU epic is tsan-clean — keep it that way):**

```bash
cmake --preset tsan && cmake --build build/tsan --parallel --target lci_tests
./build/tsan/tests/lci_tests --gtest_filter='*Trigram*:*Postings*:*FileContentStore*'
```

→ no race reports.
- [ ] **Step 5: Commit** `git commit -am "fix(core): AtomicSharedPtr wrapper — libc++ lacks atomic<shared_ptr> (macOS)"`

### Task 13: FP `from_chars` call sites → `portable::parse_double`

**Files:** Modify: `src/config/config.cpp:143-147`, `src/core/semantic_annotator.cpp:321-328,351-358`

- [ ] **Step 1:** `config.cpp` (the KDL number lexer):

```cpp
// before (143-147)
        std::string text(src_.substr(start, pos_ - start));
        double val = 0;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), val);
        if (ec != std::errc{}) val = 0;
        return {TokenKind::Number, std::move(text), val, false};
// after
        std::string text(src_.substr(start, pos_ - start));
        double val = 0;
        if (!portable::parse_double(text, val)) val = 0;
        return {TokenKind::Number, std::move(text), val, false};
```

- [ ] **Step 2:** `semantic_annotator.cpp` — the two `double weight` sites (321-328 and 351-358) become:

```cpp
    if (RE2::PartialMatch(sp, *patterns_.loop_weight, &cap)) {
        double weight = 0;
        if (portable::parse_double(cap, weight)) {
            ann.loop_weight = weight;
            ann.has_memory_hints = true;
        }
    }
```

```cpp
    if (RE2::PartialMatch(sp, *patterns_.propagation_weight, &cap)) {
        double weight = 0;
        if (portable::parse_double(cap, weight)) {
            ann.propagation_weight = std::clamp(weight, 0.0, 1.0);
            ann.has_memory_hints = true;
        }
    }
```

(The `int bounded` site at 331-339 keeps integer `std::from_chars` — supported everywhere.) Add `#include <lci/core/portable.h>` to both files.

- [ ] **Step 3:** Build + `./build/release/tests/lci_tests --gtest_filter='*Config*:*Semantic*'` → PASS; full suite green.
- [ ] **Step 4: Commit** `git commit -am "fix(config,core): parse_double for libc++ (no FP from_chars on macOS)"`

### Task 14: macOS CI leg green + unit gate

**Files:** Modify: `.github/workflows/ci.yml` (`on:` block at lines 3-11, macos job at 161-216)

- [ ] **Step 1:** The macos job has `if: github.event_name == 'push'` — it never runs on PRs (10× billing guard). Add an on-demand trigger so the branch can be validated without merging:

```yaml
on:
  push:
    branches: [main]
    paths-ignore:
      - "**/*.md"
      - "docs/**"
      - ".gitignore"
  pull_request:
    branches: [main]
  workflow_dispatch:
```

and change the macos job condition to:

```yaml
    if: github.event_name == 'push' || github.event_name == 'workflow_dispatch'
```

- [ ] **Step 2:** Add the unit-test gate after the Build step of the macos job:

```yaml
      - name: Test (unit suite)
        run: ./build/release/tests/lci_tests
```

- [ ] **Step 3:** Push, then dispatch: `gh workflow run ci.yml --ref windows-mac-port && gh run watch`. Expected: `macos appleclang / release` green. The `__compressed_pair`/`make_shared<FileContentStore>` errors should disappear with Task 12 (they cascade from the failed `atomic<shared_ptr<FileContentSnapshot>>` member instantiation). **If they persist:** that's a real independent bug — pull the fresh log (`gh run view <id> --log-failed`), find the first error in the chain, and fix root cause before proceeding; do not merge with a red mac leg.
- [ ] **Step 4:** Watch for `program_invocation_name` (Task 7 fixed it with `getprogname()`) and any further AppleClang strictness; fix in place.
- [ ] **Step 5: Commit** `git commit -am "ci(macos): unit-suite gate + workflow_dispatch for branch validation"`

---

## Phase 5 — macOS release leg

### Task 15: release.yml build-macos + publish wiring

**Files:** Modify: `.github/workflows/release.yml`

- [ ] **Step 1:** Add after build-windows (mirrors the ci.yml macos job; arm64-only per D4, asset `lci-<ver>-Darwin.tar.gz` matches `install.sh:31` and `select_asset`):

```yaml
  build-macos:
    runs-on: macos-latest
    name: build / lci-darwin-arm64
    env:
      VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"
    steps:
      - uses: actions/checkout@v4

      - name: Export GitHub Actions cache variables
        uses: actions/github-script@v7
        with:
          script: |
            core.exportVariable('ACTIONS_CACHE_URL', process.env.ACTIONS_CACHE_URL || '');
            core.exportVariable('ACTIONS_RUNTIME_TOKEN', process.env.ACTIONS_RUNTIME_TOKEN || '');

      - name: Set up vcpkg
        uses: lukka/run-vcpkg@v11
        with:
          vcpkgGitCommitId: "f3e10653cc27d62a37a3763cd84b38bca07c6075"

      - name: Install tools
        run: brew install ninja

      - name: Configure
        run: |
          cmake --preset release \
            -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

      - name: Build
        run: cmake --build build/release --parallel

      - name: Test (unit suite)
        run: ./build/release/tests/lci_tests

      - name: Package
        run: |
          cd build/release
          cpack -G TGZ

      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: lci-darwin-arm64
          path: build/release/lci-*.tar.gz
          retention-days: 2
```

- [ ] **Step 2:** `needs: [build-linux, build-windows, build-macos]` on the publish job. Delete the lines 92-97 "Windows and macOS build legs were removed" NOTE comment entirely (both legs are back). SHA256SUMS/`files:` globs already cover everything (all three platforms ship `.tar.gz`).
- [ ] **Step 3: Commit** `git commit -am "ci(release): macOS arm64 leg; all three platforms publish"`

---

## Phase 6 — Wrap-up

### Task 16: docs, follow-ups, merge

- [ ] **Step 1:** Update `CHANGELOG.md` (if present; else skip per repo convention) and `README.md` platform support matrix: Linux x86_64 (tar.gz/deb/rpm), Windows x86_64 (tar.gz), macOS arm64 (tar.gz). Note D4's x86_64-mac gap.
- [ ] **Step 2:** File Dart follow-up tasks (use dart-query MCP, tag `loop-fix`):
  1. "Port parity runner (tests/parity/runner/modes/{cli,mcp,http}.cpp, child_guard.h) to Windows via lci::subprocess extension (async pipe read, kill, timeout)" — references the `if(NOT WIN32)` gate in tests/CMakeLists.txt.
  2. "macOS universal binary or x86_64 leg: vcpkg universal triplet for abseil/re2/efsw + LCI_UNIVERSAL_BINARY preset; or arch-check in install.sh Darwin branch."
- [ ] **Step 3:** Final verification sweep (karpathy #1, 10/10 stability not applicable here — these are deterministic compile gates, but run the unit suite twice):

```bash
./build/release/tests/lci_tests && ./build/release/tests/lci_tests
ctest --test-dir build/release -R lci_integration_suite --output-on-failure
gh pr checks   # linux x5 + windows green; macos green via dispatch run
```

- [ ] **Step 4:** Mark PR ready, merge to main (squash vs merge per repo habit — history shows plain commits to main; rebase-merge). Push of main triggers ci.yml including the macos leg — confirm green end-to-end.
- [ ] **Step 5:** Release dry-run happens at the next `v*` tag (user's call — version bump to 0.7.0 in `CMakeLists.txt:project()` belongs in the tag commit). Verify after tagging: three artifact sets + SHA256SUMS on the GitHub release; `install.ps1` on the Windows runner and `install.sh` on mac/Linux fetch and verify checksums.

---

## Self-review notes

- **Spec coverage:** Windows compile errors (10 sites) → Tasks 2,4,5,6,9; Windows runtime gaps (server spawn stub, MCP detection) → Tasks 7,8; Windows CI/release → Tasks 10,11. macOS compile errors (3 classes) → Tasks 12,13 (+14 verifies the cascade); macOS CI/release → Tasks 14,15. Windows-first ordering preserved: Phases 2-3 complete before any mac work.
- **Known risk:** the MSVC failure log shows errors only up to where the build aborted; Task 10 Step 3 is the explicit checkpoint for residuals (likely candidates: more `unistd.h` includes in test files, `ssize_t` uses). Same for AppleClang in Task 14 Step 4.
- **Type consistency:** `subprocess::run_capture(argv, cwd, out)` signature used identically in Tasks 3,5,6; `portable::process_id()` returns `int` everywhere; `AtomicSharedPtr::load/store` keep the exact call-site spelling of `std::atomic<shared_ptr>`.
