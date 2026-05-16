#include "runner/modes/mcp.h"
#include "runner/modes/subst.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace lci::parity {

namespace {

const char* kInitializeBody =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
    "\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"parity_runner\",\"version\":\"1.0\"}}}";

const char* kInitializedNotification =
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}";

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

// Encode a JSON-RPC frame in the chosen framing.
std::string encode_frame(const std::string& body, McpFraming f) {
    if (f == McpFraming::Ndjson) {
        return body.back() == '\n' ? body : body + "\n";
    }
    // Content-Length
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// Read one decoded frame from fd. Blocks (up to deadline) until a complete
// frame is available. Returns empty string on EOF/timeout.
std::string read_frame(int fd, McpFraming f,
                       std::chrono::steady_clock::time_point deadline) {
    if (f == McpFraming::Ndjson) {
        return read_one_line(fd, deadline);
    }
    // Content-Length: read headers until blank line, parse Content-Length, read body.
    std::string headers;
    while (std::chrono::steady_clock::now() < deadline) {
        char c;
        ssize_t n = ::read(fd, &c, 1);
        if (n == 1) {
            headers.push_back(c);
            if (headers.size() >= 4 &&
                headers.compare(headers.size() - 4, 4, "\r\n\r\n") == 0) break;
        } else if (n == 0) {
            return std::string();
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return std::string();
        }
    }
    // Parse Content-Length
    size_t len = 0;
    auto pos = headers.find("Content-Length:");
    if (pos == std::string::npos) return std::string();
    pos += sizeof("Content-Length:") - 1;
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
    while (pos < headers.size() && std::isdigit(static_cast<unsigned char>(headers[pos]))) {
        len = len * 10 + (headers[pos++] - '0');
    }
    std::string body;
    body.reserve(len);
    while (body.size() < len && std::chrono::steady_clock::now() < deadline) {
        char buf[4096];
        size_t want = std::min(sizeof(buf), len - body.size());
        ssize_t n = ::read(fd, buf, want);
        if (n > 0) body.append(buf, static_cast<size_t>(n));
        else if (n == 0) break;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else break;
    }
    return body;
}

} // namespace

CapturedOutput run_mcp(const std::string& binary_path,
                       const Descriptor&  d,
                       const std::string& corpus_path,
                       McpFraming         framing,
                       int                timeout_seconds) {
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

        for (const auto& [k, v] : d.invocation.env) {
            std::string subst_v = substitute(v, corpus_path);
            setenv(k.c_str(), subst_v.c_str(), 1);
        }
        if (!d.invocation.cwd.empty()) {
            std::string cwd = substitute(d.invocation.cwd, corpus_path);
            if (chdir(cwd.c_str()) != 0) _exit(127);
        } else if (!corpus_path.empty()) {
            if (chdir(corpus_path.c_str()) != 0) _exit(127);
        }

        std::vector<std::string> sub_args;
        sub_args.reserve(d.invocation.args.size());
        for (const auto& a : d.invocation.args)
            sub_args.push_back(substitute(a, corpus_path));

        std::vector<char*> argv;
        argv.reserve(sub_args.size() + 2);
        std::string prog = binary_path;
        argv.push_back(prog.data());
        for (auto& a : sub_args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(prog.c_str(), argv.data());
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_seconds);

    // MCP handshake
    write_all(in_pipe[1], encode_frame(kInitializeBody, framing));
    std::string init_resp = read_frame(out_pipe[0], framing, deadline);
    (void)init_resp; // we don't validate beyond non-empty

    write_all(in_pipe[1], encode_frame(kInitializedNotification, framing));

    // Send the descriptor's stdin payload (the tool-call JSON-RPC).
    if (!d.invocation.stdin_data.empty()) {
        write_all(in_pipe[1], encode_frame(d.invocation.stdin_data, framing));
    }

    // Drain stderr concurrently while reading the tool-call response.
    std::string stderr_buf;
    std::thread err_reader([&] {
        char buf[4096];
        while (true) {
            ssize_t n = ::read(err_pipe[0], buf, sizeof(buf));
            if (n > 0) stderr_buf.append(buf, static_cast<size_t>(n));
            else if (n == 0) break;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } else break;
        }
    });

    std::string call_resp = read_frame(out_pipe[0], framing, deadline);

    close(in_pipe[1]);
    ::kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);

    // Signal EOF to err_reader by closing read end after child has exited.
    close(err_pipe[0]);
    err_reader.join();
    close(out_pipe[0]);

    CapturedOutput cap;
    cap.stdout_data = call_resp;
    cap.stderr_data = std::move(stderr_buf);
    cap.exit_code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    cap.timed_out   = std::chrono::steady_clock::now() > deadline;
    return cap;
}

} // namespace lci::parity
