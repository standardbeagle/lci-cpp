#pragma once

// Cross-platform git invocation for tests that build a throwaway repo.
//
// The output redirect must be platform-correct: POSIX shells understand
// ">/dev/null", but cmd.exe (which std::system uses on Windows) treats
// "/dev/null" as the literal path "\dev\null". With no "\dev" directory the
// redirect fails, git returns non-zero, and `git("init")` looks like a git
// failure — silently breaking every git-backed fixture on Windows. "NUL" is
// the Windows null device.

#include <cstdlib>
#include <filesystem>
#include <string>

namespace lci {
namespace test {

#ifdef _WIN32
inline constexpr const char* kNullRedirect = " >NUL 2>&1";
#else
inline constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

// Runs `git -C <repo> <args>` with output discarded. Returns true on exit 0.
inline bool run_git(const std::filesystem::path& repo, const std::string& args) {
    std::string cmd =
        "git -C \"" + repo.string() + "\" " + args + kNullRedirect;
    return std::system(cmd.c_str()) == 0;
}

}  // namespace test
}  // namespace lci
