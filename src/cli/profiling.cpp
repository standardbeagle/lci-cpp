#include "profiling.h"

#include <cstdio>
#include <fstream>

#if defined(LCI_HAVE_GPERFTOOLS)
#  include <gperftools/profiler.h>
#  include <gperftools/heap-profiler.h>
#endif

namespace lci {
namespace cli {

void ProfilerGuard::stop() {
    if (!active_) return;
#if defined(LCI_HAVE_GPERFTOOLS)
    switch (kind_) {
        case Kind::Cpu:
            ::ProfilerStop();
            break;
        case Kind::Memory:
            // Dump now; HeapProfilerStop frees state. The dump suffix gets
            // appended to `path_` by tcmalloc (e.g. `mem.prof.0001.heap`).
            ::HeapProfilerDump("lci shutdown");
            ::HeapProfilerStop();
            break;
        case Kind::Inactive:
            break;
    }
#endif
    active_ = false;
    kind_ = Kind::Inactive;
}

ProfilerGuard start_cpu_profile(const std::string& path, std::string& error) {
    if (path.empty()) return ProfilerGuard{};
#if defined(LCI_HAVE_GPERFTOOLS)
    // ProfilerStart writes to `path` on its own; verify the path is writable
    // up-front so we fail fast with a clear error instead of a silent miss.
    {
        std::ofstream probe(path);
        if (!probe) {
            error = "failed to open CPU profile output path: " + path;
            return ProfilerGuard{};
        }
    }
    if (::ProfilerStart(path.c_str()) == 0) {
        error = "ProfilerStart failed for path: " + path;
        return ProfilerGuard{};
    }
    std::fprintf(stderr, "[lci] CPU profiling enabled → %s\n", path.c_str());
    return ProfilerGuard(ProfilerGuard::Kind::Cpu, path, /*active=*/true);
#else
    (void)path;
    error = "--profile-cpu / --cpu-profile requires gperftools "
            "(libprofiler) at build time; this lci binary was built without "
            "it. Rebuild with gperftools installed (apt: libgoogle-perftools-dev, "
            "brew: gperftools, vcpkg: gperftools) and re-cmake.";
    return ProfilerGuard{};
#endif
}

ProfilerGuard start_memory_profile(const std::string& path, std::string& error) {
    if (path.empty()) return ProfilerGuard{};
#if defined(LCI_HAVE_GPERFTOOLS)
    {
        // tcmalloc writes `<prefix>.NNNN.heap` files; check the prefix dir
        // is writable by probing the parent. A simple touch of `path` works.
        std::ofstream probe(path);
        if (!probe) {
            error = "failed to open memory profile output path: " + path;
            return ProfilerGuard{};
        }
    }
    ::HeapProfilerStart(path.c_str());
    std::fprintf(stderr, "[lci] Memory profiling enabled → %s\n", path.c_str());
    return ProfilerGuard(ProfilerGuard::Kind::Memory, path, /*active=*/true);
#else
    (void)path;
    error = "--profile-memory / --mem-profile requires gperftools "
            "(libtcmalloc) at build time; this lci binary was built without "
            "it. Rebuild with gperftools installed (apt: libgoogle-perftools-dev, "
            "brew: gperftools, vcpkg: gperftools) and re-cmake.";
    return ProfilerGuard{};
#endif
}

}  // namespace cli
}  // namespace lci
