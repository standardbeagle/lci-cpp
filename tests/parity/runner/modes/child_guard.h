// tests/parity/runner/modes/child_guard.h
//
// RAII guard for a forked child pid in parity-runner mode handlers.
//
// Why this exists (karpathy rule 4 — determinism, rule 6 — fail fast):
// Every parity mode (cli / mcp / http) forks an lci server / CLI child,
// drives it over pipes or a Unix socket, then is expected to reap it
// before returning. Prior code reaped on the happy path only — any
// exception thrown from httplib, nlohmann::json::parse, std::thread
// construction, or a read/write helper between fork() and the final
// kill+waitpid orphaned the child. Orphans pinned inotify watches and
// stale sockets, surfacing as parity_verify failure (test #2036) and
// flaky IntegrationMcpSpec.MatchesGolden (same socket-name pool).
//
// Contract:
//   * Constructed with a pid_t (or pid<=0 sentinel = inactive).
//   * release() — caller has reaped, guard becomes inactive.
//   * Destructor of an active guard: SIGTERM, waitpid up to 2 s,
//     SIGKILL, blocking waitpid. Exception-neutral: never throws.
//
// Used by cli.cpp, mcp.cpp, http.cpp. Stays in this header so all three
// share one definition; header-only keeps the file count within budget
// (2-3 files for the fix).

#pragma once

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

namespace lci::parity {

class ChildProcessGuard {
public:
    ChildProcessGuard() = default;
    explicit ChildProcessGuard(pid_t pid) noexcept : pid_(pid) {}

    ChildProcessGuard(const ChildProcessGuard&)            = delete;
    ChildProcessGuard& operator=(const ChildProcessGuard&) = delete;

    ChildProcessGuard(ChildProcessGuard&& other) noexcept : pid_(other.pid_) {
        other.pid_ = -1;
    }
    ChildProcessGuard& operator=(ChildProcessGuard&& other) noexcept {
        if (this != &other) {
            reap_now();
            pid_       = other.pid_;
            other.pid_ = -1;
        }
        return *this;
    }

    ~ChildProcessGuard() { reap_now(); }

    // Caller has finished and already reaped (or wants to reap manually).
    // Marks the guard inactive without touching the pid.
    pid_t release() noexcept {
        pid_t p = pid_;
        pid_    = -1;
        return p;
    }

    pid_t pid() const noexcept { return pid_; }
    bool  active() const noexcept { return pid_ > 0; }

private:
    // SIGTERM, poll up to 2 s, SIGKILL, blocking waitpid. Never throws.
    void reap_now() noexcept {
        if (pid_ <= 0) return;
        // Best-effort: child may already be reaped or never have existed.
        // ESRCH on kill is a no-op; waitpid handles the rest.
        ::kill(pid_, SIGTERM);
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < end) {
            int       status = 0;
            const pid_t w    = ::waitpid(pid_, &status, WNOHANG);
            if (w == pid_ || w == -1) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ::kill(pid_, SIGKILL);
        // Blocking reap after SIGKILL — kernel will deliver promptly.
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }

    pid_t pid_ = -1;
};

} // namespace lci::parity
