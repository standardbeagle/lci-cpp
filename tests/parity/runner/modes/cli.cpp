#include "runner/modes/cli.h"

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

std::string substitute(const std::string& s, const std::string& corpus_path) {
    std::string out = s;
    auto replace = [&](const std::string& token, const std::string& with) {
        size_t pos = 0;
        while ((pos = out.find(token, pos)) != std::string::npos) {
            out.replace(pos, token.size(), with);
            pos += with.size();
        }
    };
    replace("${CORPUS}", corpus_path);
    return out;
}

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
            setenv(k.c_str(), v.c_str(), 1);
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

    cap.stdout_data = read_all(out_pipe[0]);
    cap.stderr_data = read_all(err_pipe[0]);
    close(out_pipe[0]);
    close(err_pipe[0]);

    if (WIFEXITED(status))   cap.exit_code = WEXITSTATUS(status);
    else                     cap.exit_code = -1;
    return cap;
}

} // namespace lci::parity
