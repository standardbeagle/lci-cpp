#include "runner/modes/http.h"
#include "runner/modes/child_guard.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    // RAII guard: any early return / exception between here and the
    // explicit release() at end-of-function reaps the child. Destructor
    // is SIGTERM → poll 2 s → SIGKILL → waitpid, noexcept. Karpathy
    // rule 4 + rule 6 — never leak; never let the failure path silently
    // succeed.
    ChildProcessGuard guard(srv);

    std::string sock_path;
    if (!wait_for_socket(candidates, sock_path, std::chrono::seconds(15))) {
        // guard's dtor will SIGTERM/SIGKILL+reap srv; stale socket cleanup
        // is handled by the next setup-fixture run and by kill_server's
        // post-kill remove. Mirror the latter here for an isolated cleanup.
        guard.release();
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

    // Wait up to 5 s for the index to be ready. Go guards most endpoints
    // behind index readiness and returns 503 until complete; C++ responds
    // immediately. Poll /status so both binaries are measured post-ready.
    // Multi-lang corpus indexes in <2 s on both binaries; 5 s gives 2.5×
    // headroom while bounding worst-case wall time when ready never fires
    // (test proceeds anyway on timeout).
    wait_for_ready(cli, std::chrono::seconds(5));

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

    // Happy path: release the guard and reap via kill_server which also
    // removes the live socket. If anything above threw, the guard's
    // destructor already reaped before unwind reached here.
    guard.release();
    kill_server(srv, sock_path);

    return cap;
}

std::vector<std::string> candidate_socket_paths_for_test(
    const std::string& abs_corpus) {
    return candidate_socket_paths(abs_corpus);
}

void shutdown_corpus_servers(const std::string& corpus_path) {
    std::error_code ec;
    std::string abs_corpus = fs::absolute(corpus_path, ec).string();
    if (ec) abs_corpus = corpus_path;

    auto candidates = candidate_socket_paths(abs_corpus);
    bool any_live = false;
    for (const auto& sock_path : candidates) {
        if (!fs::exists(sock_path)) continue;
        any_live = true;
        httplib::Client cli(sock_path);
        cli.set_address_family(AF_UNIX);
        cli.set_connection_timeout(2);
        cli.set_read_timeout(2);
        cli.set_default_headers({{"Host", "localhost"}});
        // POST /shutdown to the per-corpus daemon spawned by the CLI's
        // ensure_server_running() helper. Both Go and C++ servers honor
        // /shutdown; failure is non-fatal (server may already be dead).
        cli.Post("/shutdown", "", "application/json");
    }
    if (!any_live) return;

    // Poll briefly for the daemon to honor /shutdown and unlink its
    // socket. 5 s ceiling matches kill_server's grace.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_gone = true;
        for (const auto& sock_path : candidates) {
            if (fs::exists(sock_path)) { all_gone = false; break; }
        }
        if (all_gone) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Belt-and-suspenders: pgrep any surviving `lci --root <corpus>`
    // children (Go or C++) and SIGTERM/SIGKILL them. Scoped to this
    // corpus's absolute path so concurrent parity runs on different
    // corpora are untouched. Always runs — if /shutdown already
    // succeeded, pgrep returns nothing and this is a no-op.
    {
        // Match both C++ ("/lci ") and Go ("/lci-linux-amd64 ") binaries
        // rooted at this exact corpus path. The trailing `( |$)` anchors
        // the corpus path so subdirectories of the same corpus aren't
        // accidentally swept up.
        std::string cmd =
            "pgrep -f '(/lci|lci-linux-amd64) .*--root " + abs_corpus +
            "( |$)' 2>/dev/null";
        FILE* p = ::popen(cmd.c_str(), "r");
        if (p) {
            char line[64];
            std::vector<pid_t> pids;
            while (std::fgets(line, sizeof(line), p)) {
                long v = std::strtol(line, nullptr, 10);
                if (v > 0) pids.push_back(static_cast<pid_t>(v));
            }
            ::pclose(p);
            for (pid_t pid : pids) ::kill(pid, SIGTERM);
            if (!pids.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                for (pid_t pid : pids) ::kill(pid, SIGKILL);
                // SIGKILL doesn't reap. These daemons were spawned via
                // setsid so we're not their parent; init reaps them
                // asynchronously. parity_verify also calls pgrep — it
                // can still see the dying PIDs for a few ms after SIGKILL
                // until /proc/<pid> disappears. Poll /proc with a short
                // ceiling so the next test (and parity_verify) see a
                // clean process table. Karpathy rule 4 — race-free.
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(3);
                while (std::chrono::steady_clock::now() < deadline) {
                    bool any_alive = false;
                    for (pid_t pid : pids) {
                        std::string proc_path =
                            "/proc/" + std::to_string(pid);
                        std::error_code ec;
                        if (fs::exists(proc_path, ec)) {
                            any_alive = true;
                            break;
                        }
                    }
                    if (!any_alive) break;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));
                }
            }
        }
    }
    // Final defensive socket-file cleanup: a cleanly-shutdown server
    // unlinks its socket itself, but a SIGKILL'd one (or one that
    // shut down between the poll and our pgrep) leaves the file
    // behind. orphan_cleanup.sh verify counts socket *files* as well
    // as procs, so the file has to go.
    for (const auto& sock_path : candidates) {
        std::error_code rm_ec;
        fs::remove(sock_path, rm_ec);
    }
}

} // namespace lci::parity
