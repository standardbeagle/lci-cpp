#include "runner/modes/http.h"

#include <httplib.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lci::parity {

namespace {

// Replicates the identical hash algorithm used by both Go and C++ lci binaries:
//   hash = hash * 31 + uint32(c)   for each char in abs_path
// producing /tmp/lci-server-<08hex>.sock.
std::string compute_socket_path(const std::string& abs_corpus) {
    uint32_t hash = 0;
    for (unsigned char c : abs_corpus) {
        hash = hash * 31u + static_cast<uint32_t>(c);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "lci-server-%08x.sock", hash);
    return (fs::temp_directory_path() / buf).string();
}

pid_t spawn_server(const std::string& binary_path,
                   const std::string& corpus_path) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // child: redirect stdout/stderr to /dev/null so the server
        // doesn't pollute the test output.
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        // Pass the corpus as --root so both binaries compute the
        // same deterministic socket path.
        ::execlp(binary_path.c_str(), binary_path.c_str(),
                 "--root", corpus_path.c_str(),
                 "server",
                 static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return pid;
}

bool wait_for_socket(const std::string& sock_path,
                     std::chrono::milliseconds budget) {
    auto end = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < end) {
        if (fs::exists(sock_path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void kill_server(pid_t pid, const std::string& sock_path) {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    // Give it up to 5 s to exit gracefully.
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        int status = 0;
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    // Remove stale socket after forced kill.
    std::error_code ec;
    fs::remove(sock_path, ec);
}

} // namespace

CapturedOutput run_http(const std::string& binary_path,
                        const Descriptor&  d,
                        const std::string& corpus_path) {
    CapturedOutput cap;

    // Resolve the corpus to an absolute path so the hash matches what
    // the server computes internally via filepath.Abs / fs::absolute.
    std::error_code ec;
    std::string abs_corpus = fs::absolute(corpus_path, ec).string();
    if (ec) abs_corpus = corpus_path;

    std::string sock_path = compute_socket_path(abs_corpus);

    // Remove any stale socket from a previous crashed run.
    fs::remove(sock_path, ec);

    pid_t srv = spawn_server(binary_path, abs_corpus);
    if (srv < 0) {
        cap.stderr_data = "run_http: fork failed: " + std::string(std::strerror(errno));
        return cap;
    }

    if (!wait_for_socket(sock_path, std::chrono::seconds(15))) {
        kill_server(srv, sock_path);
        cap.timed_out = true;
        cap.stderr_data = "run_http: server did not create socket within 15 s";
        return cap;
    }

    // Connect via Unix domain socket.
    httplib::Client cli(sock_path);
    cli.set_address_family(AF_UNIX);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    // invocation.args[0] is the URL path (e.g. "/status", "/search").
    // invocation.stdin_data is the POST body (empty → GET).
    const std::string& url_path = d.invocation.args.empty()
                                  ? std::string("/")
                                  : d.invocation.args[0];

    if (!d.invocation.stdin_data.empty()) {
        auto res = cli.Post(url_path, d.invocation.stdin_data, "application/json");
        if (res) {
            cap.stdout_data = res->body;
            cap.exit_code   = res->status;
        } else {
            cap.stderr_data = "run_http: POST failed: " +
                              httplib::to_string(res.error());
        }
    } else {
        auto res = cli.Get(url_path);
        if (res) {
            cap.stdout_data = res->body;
            cap.exit_code   = res->status;
        } else {
            cap.stderr_data = "run_http: GET failed: " +
                              httplib::to_string(res.error());
        }
    }

    // Graceful shutdown via the HTTP API.
    cli.Post("/shutdown", "", "application/json");

    // Wait for the server process to exit (it should exit after /shutdown).
    kill_server(srv, sock_path);

    return cap;
}

} // namespace lci::parity
