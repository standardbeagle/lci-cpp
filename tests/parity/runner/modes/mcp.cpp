#include "runner/modes/mcp.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace lci::parity {

namespace {

const char* kInitializeFrame =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
    "\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"parity_runner\",\"version\":\"1.0\"}}}\n";

ssize_t write_all(int fd, const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = ::write(fd, data.data() + total, data.size() - total);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

std::string read_one_line(int fd, std::chrono::steady_clock::time_point deadline) {
    std::string out;
    char c;
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::read(fd, &c, 1);
        if (n == 1) {
            if (c == '\n') return out;
            out.push_back(c);
        } else if (n == 0) {
            return out;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return out;
        }
    }
    return out;
}

} // namespace

CapturedOutput run_mcp(const std::string& binary_path,
                       const Descriptor&  d,
                       const std::string& corpus_path,
                       int timeout_seconds) {
    int out_pipe[2], in_pipe[2], err_pipe[2];
    if (pipe(out_pipe) || pipe(in_pipe) || pipe(err_pipe)) {
        throw std::runtime_error(std::string("pipe failed: ") + strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        for (const auto& [k, v] : d.invocation.env) setenv(k.c_str(), v.c_str(), 1);
        if (!corpus_path.empty()) chdir(corpus_path.c_str());

        std::vector<std::string> args = d.invocation.args;
        std::vector<char*> argv;
        std::string prog = binary_path;
        argv.push_back(prog.data());
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(prog.c_str(), argv.data());
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_seconds);

    write_all(in_pipe[1], kInitializeFrame);
    std::string init_resp = read_one_line(out_pipe[0], deadline);
    (void)init_resp; // we don't validate beyond non-empty

    // Send the descriptor's stdin payload (the tool-call JSON-RPC).
    if (!d.invocation.stdin_data.empty()) {
        std::string body = d.invocation.stdin_data;
        if (body.back() != '\n') body.push_back('\n');
        write_all(in_pipe[1], body);
    }
    std::string call_resp = read_one_line(out_pipe[0], deadline);

    close(in_pipe[1]);
    ::kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);

    CapturedOutput cap;
    cap.stdout_data = call_resp;
    cap.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
    cap.timed_out = std::chrono::steady_clock::now() > deadline;

    char buf[4096];
    while (true) {
        ssize_t n = ::read(err_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        cap.stderr_data.append(buf, static_cast<size_t>(n));
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    return cap;
}

} // namespace lci::parity
