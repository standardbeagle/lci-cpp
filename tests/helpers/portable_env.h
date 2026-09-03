#pragma once

// MSVC ships no POSIX setenv/unsetenv; tests need only the narrow
// overwrite/remove semantics, which _putenv_s provides (empty value
// removes). Members of the unnamed namespace are found by ::setenv
// qualified lookup too.
#ifdef _WIN32
#include <cstdlib>
namespace {
inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
}  // namespace
#endif
