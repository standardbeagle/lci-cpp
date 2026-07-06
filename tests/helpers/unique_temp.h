#pragma once

// Process-and-instance-unique temp paths for tests.
//
// gtest_discover_tests registers every TEST case as its own ctest entry, so
// `ctest -j<N>` runs many cases *as separate processes of the same binary,
// concurrently*. Any fixture that derived its scratch directory from a fixed
// string, or from `hash(thread::id) ^ counter` (both identical across two
// freshly-started processes: same main-thread id, counter starts at 0), handed
// two concurrent workers the SAME path. They then indexed and remove_all()'d
// each other's files — a reader mmaps a source file, the other process's
// TearDown truncates it, and the reader takes SIGBUS ("Bus error"); or the
// dir vanishes mid-index and cases fail/time out. Mixing in the pid makes the
// path unique per process; the atomic counter keeps it unique per instance
// within a process. Deterministic per (pid, instance) — no RNG.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

namespace lci {
namespace test {

// A "<pid>_<n>" token, unique across processes (pid) and across calls within a
// process (monotonic atomic counter).
inline std::string unique_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    auto pid = static_cast<std::uint64_t>(
#ifdef _WIN32
        _getpid()
#else
        ::getpid()
#endif
    );
    return std::to_string(pid) + "_" +
           std::to_string(counter.fetch_add(1));
}

// <temp>/<prefix><pid>_<n> — collision-free under parallel ctest.
inline std::filesystem::path unique_temp_dir(std::string_view prefix) {
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + unique_suffix());
}

}  // namespace test
}  // namespace lci
