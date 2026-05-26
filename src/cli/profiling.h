// CPU/memory profiling wiring for `lci`.
//
// Backed by gperftools (libprofiler / libtcmalloc) when CMake detects them
// (LCI_HAVE_GPERFTOOLS). When unavailable, `start_cpu_profile()` and
// `start_memory_profile()` return a clear error naming what's missing — no
// silent no-op (Karpathy rule 6: surface signal, no fallback lies).
//
// Usage: call `start_cpu_profile(path, err)` once before the work to be
// profiled; the returned RAII guard stops profiling and writes the output
// when destructed. Same for `start_memory_profile()`. Both no-op cleanly
// when `path` is empty so callers can wire flag → call with no branching.

#pragma once

#include <string>

namespace lci {
namespace cli {

/// RAII guard. Constructed inactive when `path` is empty; otherwise either
/// starts profiling (and stops + writes on destruction) or sets `error` and
/// stays inactive. Move-only.
class ProfilerGuard {
public:
    ProfilerGuard() = default;
    ProfilerGuard(const ProfilerGuard&) = delete;
    ProfilerGuard& operator=(const ProfilerGuard&) = delete;
    ProfilerGuard(ProfilerGuard&& other) noexcept
        : kind_(other.kind_), path_(std::move(other.path_)),
          active_(other.active_) {
        other.kind_ = Kind::Inactive;
        other.active_ = false;
    }
    ProfilerGuard& operator=(ProfilerGuard&& other) noexcept {
        if (this != &other) {
            stop();
            kind_ = other.kind_;
            path_ = std::move(other.path_);
            active_ = other.active_;
            other.kind_ = Kind::Inactive;
            other.active_ = false;
        }
        return *this;
    }
    ~ProfilerGuard() { stop(); }

    /// True if a profile was successfully started and will be flushed on
    /// destruction. False on inactive (no path) or when start failed.
    bool active() const { return active_; }

    enum class Kind { Inactive, Cpu, Memory };

    // Internal constructor; use `start_cpu_profile` / `start_memory_profile`.
    ProfilerGuard(Kind kind, std::string path, bool active)
        : kind_(kind), path_(std::move(path)), active_(active) {}

private:
    void stop();

    Kind kind_ = Kind::Inactive;
    std::string path_;
    bool active_ = false;
};

/// Starts CPU profiling. `path` empty → returns an inactive guard, no error.
/// Non-empty path + gperftools available → starts CPU profiler, returns active
/// guard. Non-empty path + gperftools NOT available → sets `error` and returns
/// an inactive guard. Failure to open the output file likewise sets `error`.
ProfilerGuard start_cpu_profile(const std::string& path, std::string& error);

/// Starts memory (heap) profiling. Same contract as `start_cpu_profile`.
/// Heap profile is flushed and the profiler stopped on guard destruction.
ProfilerGuard start_memory_profile(const std::string& path, std::string& error);

}  // namespace cli
}  // namespace lci
