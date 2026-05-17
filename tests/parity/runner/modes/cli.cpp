#include "runner/modes/cli.h"
#include "runner/modes/child_guard.h"
#include "runner/modes/subst.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace lci::parity {

namespace {

std::string read_all(int fd) {
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

} // namespace

CapturedOutput run_cli(const std::string& binary_path,
                       const Invocation&  inv,
                       const std::string& corpus_path,
                       int                timeout_seconds) {
    int out_pipe[2], err_pipe[2], in_pipe[2];
    if (pipe(out_pipe) || pipe(err_pipe) || pipe(in_pipe)) {
        throw std::runtime_error(std::string("pipe failed: ") + strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        ::close(in_pipe[0]);  ::close(in_pipe[1]);
        throw std::runtime_error(std::string("fork failed: ") + strerror(errno));
    }

    if (pid == 0) {
        // child
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        for (const auto& [k, v] : inv.env) {
            std::string subst_v = substitute(v, corpus_path);
            setenv(k.c_str(), subst_v.c_str(), 1);
        }
        if (!inv.cwd.empty()) {
            std::string cwd = substitute(inv.cwd, corpus_path);
            if (chdir(cwd.c_str()) != 0) {
                _exit(127);
            }
        }
        std::vector<std::string> sub_args;
        sub_args.reserve(inv.args.size());
        for (const auto& a : inv.args) sub_args.push_back(substitute(a, corpus_path));

        std::vector<char*> argv;
        argv.reserve(sub_args.size() + 2);
        std::string prog = binary_path;
        argv.push_back(prog.data());
        for (auto& a : sub_args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(prog.c_str(), argv.data());
        _exit(127);
    }

    // parent
    // RAII guard: if std::thread construction below throws (e.g. EAGAIN),
    // or any future addition between fork and waitpid throws, the child
    // is SIGTERM-then-SIGKILL reaped instead of orphaned. Karpathy rule 4
    // (determinism — race-free is faster than race-with-retry) + the
    // task evidence: parity_verify catches 3 orphans / 5 sockets after
    // the multi-lang corpus suite. Released explicitly once the existing
    // wait_for / SIGKILL loop has reaped the child.
    ChildProcessGuard guard(pid);

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (!inv.stdin_data.empty()) {
        ssize_t total = 0;
        while (total < (ssize_t)inv.stdin_data.size()) {
            ssize_t n = ::write(in_pipe[1],
                                inv.stdin_data.data() + total,
                                inv.stdin_data.size() - total);
            if (n < 0) { if (errno == EINTR) continue; break; }
            total += n;
        }
    }
    close(in_pipe[1]);

    // Drain stdout/stderr concurrently to avoid deadlock when child output
    // exceeds the pipe buffer (default 64 KiB on Linux).
    std::string stdout_buf, stderr_buf;
    std::thread out_reader([&]{ stdout_buf = read_all(out_pipe[0]); });
    std::thread err_reader([&]{ stderr_buf = read_all(err_pipe[0]); });

    CapturedOutput cap;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_seconds);
    int status = 0;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            cap.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Joining the readers will block until EOF on each pipe; child exit (or
    // SIGKILL on timeout) closes the write ends, so this always terminates.
    out_reader.join();
    err_reader.join();
    close(out_pipe[0]);
    close(err_pipe[0]);

    // Child reaped above; release guard so dtor is a no-op.
    guard.release();

    cap.stdout_data = std::move(stdout_buf);
    cap.stderr_data = std::move(stderr_buf);

    if (WIFEXITED(status))   cap.exit_code = WEXITSTATUS(status);
    else                     cap.exit_code = -1;
    return cap;
}

} // namespace lci::parity
