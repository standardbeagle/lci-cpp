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
