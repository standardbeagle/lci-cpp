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

/// Sets an environment variable, overwriting any existing value. POSIX setenv
/// / MSVC _putenv_s. Returns true on success.
bool set_env(const char* name, const char* value);

/// Removes an environment variable (no-op if absent). POSIX unsetenv / MSVC
/// _putenv_s with an empty value. Returns true on success.
bool unset_env(const char* name);

}  // namespace portable
}  // namespace lci
