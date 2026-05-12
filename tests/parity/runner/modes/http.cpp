#include "runner/modes/http.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace lci::parity {

namespace {

// Replicates the identical socket-path scheme the C++ lci binary computes
// in src/server/server.cpp: `lci-<uid>-<8hex>.sock` where the hash is the
// 32-bit polynomial hash (h = h*31 + c) of the absolute project root.
// The Go binary follows the same format so both servers bind the same
// socket when given the same --root.
uint32_t hash_corpus(const std::string& abs_corpus) {
    uint32_t hash = 0;
    for (unsigned char c : abs_corpus) {
        hash = hash * 31u + static_cast<uint32_t>(c);
    }
    return hash;
}

// SUNSET-WHEN-GO-UPGRADES (Dart task BNXsh3tUpMSW):
//   The C++ binary (post-namespacing commit) uses lci-<uid>-<hash>.sock.
//   The older Go reference binary uses lci-server-<hash>.sock. We poll
//   both candidates so HTTP parity works across the version mismatch.
//   When the Go reference is updated to the new uid-namespaced format,
//   drop the second candidate, restore a single compute_socket_path()
//   helper, and delete this comment. Behaviour locked by
//   tests/parity/unit_tests/socket_path_test.cpp.
std::vector<std::string> candidate_socket_paths(const std::string& abs_corpus) {
    uint32_t hash = hash_corpus(abs_corpus);
    const auto uid = static_cast<uint32_t>(::getuid());
    char buf_new[64];
    std::snprintf(buf_new, sizeof(buf_new), "lci-%u-%08x.sock", uid, hash);
    char buf_old[64];
    std::snprintf(buf_old, sizeof(buf_old), "lci-server-%08x.sock", hash);
    return {
        (fs::temp_directory_path() / buf_new).string(),
        (fs::temp_directory_path() / buf_old).string(),
    };
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

bool wait_for_socket(const std::vector<std::string>& candidates,
                     std::string& found_path,
                     std::chrono::milliseconds budget) {
    auto end = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < end) {
        for (const auto& p : candidates) {
            if (fs::exists(p)) {
                found_path = p;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// Poll /status until ready:true or budget expires.
// Go's server guards most endpoints behind index readiness, so we must
// wait before sending the real request.
bool wait_for_ready(httplib::Client& cli, std::chrono::milliseconds budget) {
    auto end = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < end) {
        auto res = cli.Get("/status");
        if (res && res->status == 200) {
            try {
                auto j = nlohmann::json::parse(res->body);
                if (j.contains("ready") && j["ready"].get<bool>()) return true;
            } catch (...) {}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

    auto candidates = candidate_socket_paths(abs_corpus);

    // Remove any stale sockets from a previous crashed run.
    for (const auto& p : candidates) {
        fs::remove(p, ec);
    }

    pid_t srv = spawn_server(binary_path, abs_corpus);
    if (srv < 0) {
        cap.stderr_data = "run_http: fork failed: " + std::string(std::strerror(errno));
        return cap;
    }

    std::string sock_path;
    if (!wait_for_socket(candidates, sock_path, std::chrono::seconds(15))) {
        kill_server(srv, candidates.front());
        cap.timed_out = true;
        cap.stderr_data = "run_http: server did not create socket within 15 s";
        return cap;
    }

    // Connect via Unix domain socket.
    httplib::Client cli(sock_path);
    cli.set_address_family(AF_UNIX);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);
    // Go's net/http rejects requests whose Host header contains the socket
    // path (which is what httplib sends by default for Unix sockets).
    // Override with a valid Host so both Go and C++ servers accept the request.
    cli.set_default_headers({{"Host", "localhost"}});

    // Wait up to 10 s for the index to be ready. Go guards most endpoints
    // behind index readiness and returns 503 until complete; C++ responds
    // immediately. Poll /status so both binaries are measured post-ready.
    wait_for_ready(cli, std::chrono::seconds(10));

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

std::vector<std::string> candidate_socket_paths_for_test(
    const std::string& abs_corpus) {
    return candidate_socket_paths(abs_corpus);
}

} // namespace lci::parity
