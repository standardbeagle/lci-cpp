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

#include <cctype>
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
    if (n == 0) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    if (n == MAX_PATH) {
        throw std::runtime_error("executable path exceeds MAX_PATH");
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
// libc++ (macOS/AppleClang) defines __cpp_lib_to_chars for the INTEGER
// from_chars overloads but leaves the floating-point overloads deleted (as of
// LLVM 18 / Xcode 16.4 — confirmed by CI), so the feature macro cannot be
// trusted there. Use std::from_chars only on non-libc++ stdlibs (libstdc++,
// MSVC STL) that actually implement the float overload; everywhere on libc++
// fall back to strtod.
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && \
    !defined(_LIBCPP_VERSION)
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(),
                                     out);
    return ec == std::errc{};
#else
    // libc++ has no floating-point from_chars. strtod is locale-sensitive,
    // but lci never calls setlocale, so LC_NUMERIC stays "C".
    char buf[64];
    if (text.empty() || text.size() >= sizeof(buf)) return false;
    // from_chars does not skip leading whitespace; strtod does. Reject a
    // leading space so both branches agree.
    unsigned char first = static_cast<unsigned char>(text.front());
    if (std::isspace(first)) return false;
    std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
    char* end = nullptr;
    double v = std::strtod(buf, &end);
    if (end == buf) return false;
    out = v;
    return true;
#endif
}

bool set_env(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value) == 0;
#else
    return ::setenv(name, value, 1) == 0;
#endif
}

bool unset_env(const char* name) {
#if defined(_WIN32)
    // An empty value removes the variable from the MSVC CRT environment.
    return _putenv_s(name, "") == 0;
#else
    return ::unsetenv(name) == 0;
#endif
}

}  // namespace portable
}  // namespace lci
