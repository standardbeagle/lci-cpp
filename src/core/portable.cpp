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

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include <array>
#include <cerrno>
#include <random>
#include <string>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <locale>
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

void prime_ctype_narrow_cache() {
    static const bool primed = [] {
        std::array<char, 256> every_char{};
        for (size_t i = 0; i < every_char.size(); ++i) {
            every_char[i] = static_cast<char>(i);
        }
        std::array<char, 256> narrowed{};
        // The range overload is the one that fills the whole cache in one go.
        std::use_facet<std::ctype<char>>(std::locale())
            .narrow(every_char.data(), every_char.data() + every_char.size(),
                    '\0', narrowed.data());
        return true;
    }();
    (void)primed;
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

std::string write_file_atomically(const std::filesystem::path& path,
                                  std::string_view data, unsigned mode) {
    namespace fs = std::filesystem;
    const fs::path dir = path.has_parent_path() ? path.parent_path()
                                                : fs::path(".");
#if defined(_WIN32)
    (void)mode;
    // CREATE_NEW refuses any existing name, a planted reparse point included,
    // so a random suffix only has to make collisions rare, not impossible.
    std::random_device rd;
    HANDLE h = INVALID_HANDLE_VALUE;
    fs::path tmp;
    for (int attempt = 0; attempt < 16 && h == INVALID_HANDLE_VALUE;
         ++attempt) {
        tmp = dir / (L"." + path.filename().wstring() + L".tmp." +
                     std::to_wstring(rd()));
        h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                        nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) {
            return "cannot create temp file beside " + path.string() +
                   ": error " + std::to_string(GetLastError());
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        return "cannot create a unique temp file beside " + path.string();
    }
    size_t off = 0;
    bool ok = true;
    while (off < data.size()) {
        const size_t left = data.size() - off;
        DWORD chunk = static_cast<DWORD>(left < (1u << 30) ? left : (1u << 30));
        DWORD written = 0;
        if (!WriteFile(h, data.data() + off, chunk, &written, nullptr) ||
            written == 0) {
            ok = false;
            break;
        }
        off += written;
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return "failed to write temp file " + tmp.string();
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        DeleteFileW(tmp.c_str());
        return "failed to replace " + path.string() + ": error " +
               std::to_string(err);
    }
    return {};
#else
    // O_CREAT|O_EXCL never opens an existing name, a symlink included, and
    // open(2) masks `mode` with the umask. mkstemp would fix the mode at
    // 0600, and an fchmod after it would ignore the umask.
    std::random_device rd;
    std::string tmpl;
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
        tmpl = (dir / ("." + path.filename().string() + ".tmp." +
                       std::to_string(rd())))
                   .string();
        fd = ::open(tmpl.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    static_cast<mode_t>(mode));
        if (fd < 0 && errno != EEXIST) {
            return "cannot create temp file beside " + path.string() + ": " +
                   std::strerror(errno);
        }
    }
    if (fd < 0) {
        return "cannot create a unique temp file beside " + path.string();
    }
    auto fail = [&](const std::string& what) {
        int err = errno;
        ::close(fd);
        ::unlink(tmpl.c_str());
        return what + ": " + std::strerror(err);
    };
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail("failed to write " + tmpl);
        }
        off += static_cast<size_t>(n);
    }
    if (::close(fd) != 0) {
        int err = errno;
        ::unlink(tmpl.c_str());
        return "failed to close " + tmpl + ": " + std::strerror(err);
    }
    if (::rename(tmpl.c_str(), path.c_str()) != 0) {
        int err = errno;
        ::unlink(tmpl.c_str());
        return "failed to replace " + path.string() + ": " +
               std::strerror(err);
    }
    return {};
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
