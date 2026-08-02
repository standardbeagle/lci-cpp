#include "integration/spec_runner.h"
#include "runner/modes/http.h"

#include <cctype>
#include <string>
#include <string_view>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// glibc exposes argv[0] before main() runs; declared in <errno.h> only under
// _GNU_SOURCE, so declare it directly for this Linux-only test binary.
extern "C" char* program_invocation_name;

namespace lci::integration {
namespace {

constexpr const char* kListenerHelperArgv0 =
    "lci_integration_tests-listener-helper";

int RunDetachedListenerHelperIfRequested() {
    // Gate on the argv[0] marker passed by SpawnDetachedUnixListener, not on
    // environment alone: leaked env vars from an aborted run would otherwise
    // pause() the whole test binary before main().
    const char* invocation = program_invocation_name;
    if (!invocation ||
        std::string_view(invocation) != kListenerHelperArgv0) {
        return 0;
    }
    const char* pathname = ::getenv("LCI_TEST_LISTENER_PATH");
    const char* ready_fd_text = ::getenv("LCI_TEST_LISTENER_READY_FD");
    if (!pathname || !ready_fd_text) return 0;
    const int ready_fd = std::atoi(ready_fd_text);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (fd < 0 || std::strlen(pathname) >= sizeof(address.sun_path)) ::_exit(2);
    std::memcpy(address.sun_path, pathname, std::strlen(pathname) + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(fd, 4) != 0) {
        ::_exit(3);
    }
    const char marker = 'R';
    if (::write(ready_fd, &marker, 1) != 1) ::_exit(4);
    ::close(ready_fd);
    for (;;) ::pause();
}

[[maybe_unused]] const int kDetachedListenerHelper =
    RunDetachedListenerHelperIfRequested();

TEST(SpecRunnerProcessOwnership, ListenerHelperIgnoresLeakedEnvWithoutArgvMarker) {
    // Leaked listener env vars from an aborted run must NOT pause() a normal
    // invocation of this binary before main(): the helper hook requires the
    // explicit argv[0] marker, not environment alone.
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::setenv("LCI_TEST_LISTENER_PATH", "/tmp/lci-leaked-env-listener.sock",
                 1);
        ::setenv("LCI_TEST_LISTENER_READY_FD", "1", 1);
        const int null_fd = ::open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDOUT_FILENO);
            ::dup2(null_fd, STDERR_FILENO);
        }
        ::execl("/proc/self/exe", "lci_integration_tests",
                "--gtest_list_tests", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    pid_t reaped = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
        reaped = ::waitpid(child, &status, WNOHANG);
        if (reaped != 0) break;
        ::usleep(50000);
    } while (std::chrono::steady_clock::now() < deadline);
    if (reaped == 0) {
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        FAIL() << "test binary hung before main() on leaked listener env vars";
    }
    ASSERT_EQ(reaped, child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SpecRunnerProcessOwnership, RequiresExactInheritedToken) {
    const std::string token = "runner-42";
    std::string owned("PATH=/bin", 9);
    owned += '\0';
    owned += "LCI_SPEC_RUNNER_PROCESS_OWNER=runner-42";
    owned += '\0';
    std::string ancestor("PATH=/bin", 9);
    ancestor += '\0';
    ancestor += "HOME=/tmp";
    ancestor += '\0';
    std::string other_runner =
        "LCI_SPEC_RUNNER_PROCESS_OWNER=runner-420";
    other_runner += '\0';

    EXPECT_TRUE(ProcessEnvironmentHasOwnershipTokenForTest(owned, token));
    EXPECT_FALSE(ProcessEnvironmentHasOwnershipTokenForTest(ancestor, token));
    EXPECT_FALSE(
        ProcessEnvironmentHasOwnershipTokenForTest(other_runner, token));
}

TEST(SpecRunnerProcessOwnership, CleansOwnedChildAndPreservesUnrelatedChild) {
    if (!PidfdCleanupSupportedForTest()) {
        GTEST_SKIP() << "pidfd_open/pidfd_send_signal are unsupported by this "
                        "Linux kernel";
    }

    constexpr const char* token = "real-child-lifecycle";
    auto spawn_sleep = [](const char* ownership_token) {
        const pid_t pid = ::fork();
        EXPECT_GE(pid, 0);
        if (pid == 0) {
            if (ownership_token) {
                ::setenv("LCI_SPEC_RUNNER_PROCESS_OWNER", ownership_token, 1);
            } else {
                ::unsetenv("LCI_SPEC_RUNNER_PROCESS_OWNER");
            }
            ::execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        return pid;
    };

    const pid_t owned = spawn_sleep(token);
    const pid_t unrelated = spawn_sleep(nullptr);
    ASSERT_GT(owned, 0);
    ASSERT_GT(unrelated, 0);
    ::usleep(100000);

    CleanupOwnedProcessesForTest(token);

    int status = 0;
    EXPECT_EQ(::waitpid(owned, &status, 0), owned);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(::kill(unrelated, 0), 0) << std::strerror(errno);

    ::kill(unrelated, SIGKILL);
    EXPECT_EQ(::waitpid(unrelated, nullptr, 0), unrelated);
}

TEST(SpecRunnerProcessOwnership, ForceKillsTermResistantOwnedChild) {
    if (!PidfdCleanupSupportedForTest()) {
        GTEST_SKIP() << "pidfd_open/pidfd_send_signal are unsupported by this "
                        "Linux kernel";
    }

    constexpr const char* token = "term-resistant-child";
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::setenv("LCI_SPEC_RUNNER_PROCESS_OWNER", token, 1);
        ::signal(SIGTERM, SIG_IGN);
        ::execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::usleep(100000);

    CleanupOwnedProcessesForTest(token);

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

pid_t SpawnDetachedUnixListener(const std::filesystem::path& path,
                                const char* ownership_token) {
    int ready[2];
    EXPECT_EQ(::pipe(ready), 0);
    const pid_t child = ::fork();
    EXPECT_GE(child, 0);
    if (child == 0) {
        ::close(ready[0]);
        if (ownership_token) {
            ::setenv("LCI_SPEC_RUNNER_PROCESS_OWNER", ownership_token, 1);
        } else {
            ::unsetenv("LCI_SPEC_RUNNER_PROCESS_OWNER");
        }
        (void)::setsid();
        const std::string pathname = path.string();
        const std::string ready_fd = std::to_string(ready[1]);
        ::setenv("LCI_TEST_LISTENER_PATH", pathname.c_str(), 1);
        ::setenv("LCI_TEST_LISTENER_READY_FD", ready_fd.c_str(), 1);
        ::execl("/proc/self/exe", kListenerHelperArgv0,
                static_cast<char*>(nullptr));
        ::_exit(5);
    }
    ::close(ready[1]);
    // A helper that never signals readiness (bad exec, early _exit) must fail
    // the test, not hang the whole binary on a blocking read.
    pollfd readiness{ready[0], POLLIN, 0};
    int poll_result = -1;
    do {
        poll_result = ::poll(&readiness, 1, /*timeout_ms=*/10000);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result <= 0 || (readiness.revents & POLLIN) == 0) {
        ADD_FAILURE() << "detached listener helper never became ready";
        ::close(ready[0]);
        ::kill(-child, SIGKILL);
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        return -1;
    }
    char marker = 0;
    EXPECT_EQ(::read(ready[0], &marker, 1), 1);
    EXPECT_EQ(marker, 'R');
    ::close(ready[0]);
    return child;
}

// Guarantees the detached helper (its own session/pgid after setsid) and the
// bound socket never outlive the test, even when a mid-test ASSERT bails out
// before the explicit cleanup calls run.
class DetachedListenerGuard {
  public:
    DetachedListenerGuard(pid_t pid, std::filesystem::path socket_path)
        : pid_(pid), socket_path_(std::move(socket_path)) {}
    DetachedListenerGuard(const DetachedListenerGuard&) = delete;
    DetachedListenerGuard& operator=(const DetachedListenerGuard&) = delete;

    ~DetachedListenerGuard() {
        if (pid_ > 0) {
            ::kill(-pid_, SIGKILL);
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
        std::error_code ec;
        std::filesystem::remove(socket_path_, ec);
    }

    // The test observed the helper's exit itself; never signal or reap a
    // possibly recycled pid afterwards.
    void MarkReaped() { pid_ = -1; }

  private:
    pid_t pid_;
    std::filesystem::path socket_path_;
};

bool CanConnectUnixSocket(const std::filesystem::path& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string pathname = path.string();
    if (pathname.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return false;
    }
    std::memcpy(address.sun_path, pathname.c_str(), pathname.size() + 1);
    const bool connected =
        ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
        0;
    ::close(fd);
    return connected;
}

TEST(SpecRunnerProcessOwnership,
     RemovesSocketAfterDetachedOwnedListenerIsConfirmedDead) {
    if (!PidfdCleanupSupportedForTest()) {
        GTEST_SKIP() << "pidfd cleanup unsupported";
    }
    constexpr const char* token = "detached-owned-listener";
    const auto corpus = std::filesystem::temp_directory_path() /
                        ("lci-owned-corpus-" + std::to_string(::getpid()) +
                         "-" + std::to_string(std::chrono::steady_clock::now()
                                                   .time_since_epoch()
                                                   .count()));
    std::filesystem::create_directories(corpus);
    const auto candidates =
        lci::parity::candidate_socket_paths_for_test(corpus.string());
    ASSERT_FALSE(candidates.empty());
    const auto socket_path = candidates.front();
    const pid_t listener = SpawnDetachedUnixListener(socket_path, token);
    DetachedListenerGuard guard(listener, socket_path);
    ASSERT_GT(listener, 0);
    ASSERT_TRUE(CanConnectUnixSocket(socket_path));

    CleanupOwnedProcessesAndSocketsForTest(token, corpus.string());

    int status = 0;
    ASSERT_EQ(::waitpid(listener, &status, 0), listener);
    guard.MarkReaped();
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_FALSE(std::filesystem::exists(socket_path));
    std::error_code ec;
    std::filesystem::remove_all(corpus, ec);
}

TEST(SpecRunnerProcessOwnership,
     PreservesUnrelatedLiveListenerOnCandidatePath) {
    const auto corpus = std::filesystem::temp_directory_path() /
                        ("lci-unrelated-corpus-" + std::to_string(::getpid()) +
                         "-" + std::to_string(std::chrono::steady_clock::now()
                                                   .time_since_epoch()
                                                   .count()));
    std::filesystem::create_directories(corpus);
    const auto candidates =
        lci::parity::candidate_socket_paths_for_test(corpus.string());
    ASSERT_FALSE(candidates.empty());
    const auto socket_path = candidates.front();
    const pid_t listener = SpawnDetachedUnixListener(socket_path, nullptr);
    DetachedListenerGuard guard(listener, socket_path);
    ASSERT_GT(listener, 0);
    ASSERT_TRUE(CanConnectUnixSocket(socket_path));

    CleanupOwnedProcessesAndSocketsForTest("some-other-owner",
                                           corpus.string());

    EXPECT_EQ(::kill(listener, 0), 0) << std::strerror(errno);
    EXPECT_TRUE(std::filesystem::exists(socket_path));
    EXPECT_TRUE(CanConnectUnixSocket(socket_path));

    if (::kill(listener, SIGKILL) == 0) {
        EXPECT_EQ(::waitpid(listener, nullptr, 0), listener);
    }
    guard.MarkReaped();
    std::error_code ec;
    std::filesystem::remove(socket_path, ec);
    std::filesystem::remove_all(corpus, ec);
}

// ---------------------------------------------------------------------------
// Migration anchors moved to the directory-walking pattern across migrations
// 3/8 - 7/8: cli/* (3/8), http/* (4/8), index/* (5/8), probes/* (6/8),
// mcp/* (7/8 — see IntegrationMcpSpec block below). The last anchor
// (SpecMigrationTest.McpInfoBasic) was retired when mcp/* was mass-migrated
// to tests/integration/mcp/<tool>/basic.spec.json + matching golden.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Parametrized integration suite: walks tests/integration/cli/ recursively
// for *.spec.json files and runs ExpectSpecMatches on each. Replaces the
// per-descriptor TEST blocks; adding a new cli case is as simple as dropping
// a new <name>.spec.json + <name>.{txt,json} golden into the tree.
// ---------------------------------------------------------------------------

class IntegrationCliSpec : public ::testing::TestWithParam<SpecCase> {};

TEST_P(IntegrationCliSpec, MatchesGolden) {
    ExpectSpecMatches(GetParam());
}

// Convert a spec's descriptor_rel_path into a gtest-safe instance name.
// Example: "integration/cli/symbols/inspect-missing-json.spec.json" →
//          "cli_symbols_inspect_missing_json".
std::string SpecCaseInstanceName(const SpecCase& spec_case) {
    std::string stem = spec_case.descriptor_rel_path;
    constexpr std::string_view prefix = "integration/";
    if (stem.rfind(prefix, 0) == 0) {
        stem.erase(0, prefix.size());
    }
    constexpr std::string_view suffix = ".spec.json";
    if (stem.size() > suffix.size()
        && stem.compare(stem.size() - suffix.size(), suffix.size(), suffix)
            == 0) {
        stem.erase(stem.size() - suffix.size());
    }
    for (char& c : stem) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            c = '_';
        }
    }
    return stem;
}

INSTANTIATE_TEST_SUITE_P(
    All,
    IntegrationCliSpec,
    ::testing::ValuesIn(DiscoverIntegrationSpecsFromTestsDir("cli")),
    [](const ::testing::TestParamInfo<SpecCase>& param_info) {
        return SpecCaseInstanceName(param_info.param);
    });

// ---------------------------------------------------------------------------
// Parametrized integration suite for http/*: walks tests/integration/http/
// recursively for *.spec.json files. Same pattern as IntegrationCliSpec —
// adding a new http endpoint to the harness is just dropping a new
// <name>.spec.json + goldens/http/<name>.json into the tree.
// ---------------------------------------------------------------------------

class IntegrationHttpSpec : public ::testing::TestWithParam<SpecCase> {};

TEST_P(IntegrationHttpSpec, MatchesGolden) {
    ExpectSpecMatches(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    All,
    IntegrationHttpSpec,
    ::testing::ValuesIn(DiscoverIntegrationSpecsFromTestsDir("http")),
    [](const ::testing::TestParamInfo<SpecCase>& param_info) {
        return SpecCaseInstanceName(param_info.param);
    });

// ---------------------------------------------------------------------------
// Parametrized integration suite for index/*: walks tests/integration/index/
// recursively for *.spec.json files. Same pattern as IntegrationCliSpec /
// IntegrationHttpSpec — adding a new index case is just dropping a new
// <name>.spec.json + goldens/index/<name>.json into the tree. Two of the
// three parity index descriptors (lci-cpp-repo, lci-go-repo) are
// intentionally not migrated because their corpora are live git checkouts;
// see tests/integration/index/KNOWN_DIVERGENCE.md for rationale.
// ---------------------------------------------------------------------------

class IntegrationIndexSpec : public ::testing::TestWithParam<SpecCase> {};

TEST_P(IntegrationIndexSpec, MatchesGolden) {
    ExpectSpecMatches(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    All,
    IntegrationIndexSpec,
    ::testing::ValuesIn(DiscoverIntegrationSpecsFromTestsDir("index")),
    [](const ::testing::TestParamInfo<SpecCase>& param_info) {
        return SpecCaseInstanceName(param_info.param);
    });

// ---------------------------------------------------------------------------
// Parametrized integration suite for probes/*: walks
// tests/integration/probes/ recursively for *.spec.json files. Same pattern
// as IntegrationCliSpec / IntegrationHttpSpec / IntegrationIndexSpec —
// adding a new probe is just dropping a new <name>.spec.json + matching
// golden into the tree. Replaces the explicit SpecMigrationTest.ProbesGraph
// anchor that lived here through migrations 3/8 - 5/8.
// ---------------------------------------------------------------------------

class IntegrationProbesSpec : public ::testing::TestWithParam<SpecCase> {};

TEST_P(IntegrationProbesSpec, MatchesGolden) {
    ExpectSpecMatches(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    All,
    IntegrationProbesSpec,
    ::testing::ValuesIn(DiscoverIntegrationSpecsFromTestsDir("probes")),
    [](const ::testing::TestParamInfo<SpecCase>& param_info) {
        return SpecCaseInstanceName(param_info.param);
    });

// ---------------------------------------------------------------------------
// Parametrized integration suite for mcp/*: walks tests/integration/mcp/
// recursively for *.spec.json files. Same pattern as the cli/http/index/
// probes blocks above — adding a new MCP tool case is just dropping a new
// <tool>/<name>.spec.json + goldens/mcp/<tool>/<name>.json into the tree.
//
// Replaces the explicit SpecMigrationTest.McpInfoBasic anchor that lived
// here through migrations 3/8 - 6/8. Each spec drives the C++ MCP server
// over stdio (newline-delimited framing) with a deterministic
// initialize → notifications/initialized → tools/call sequence and pins
// result.content[].type + result.content[].text against a captured golden.
//
// The parity oracle suite (`ctest -L parity -R parity\.mcp`) remains the
// authoritative cross-port check between the Go reference binary and this
// C++ port; the integration suite intentionally pins ONLY the C++ side
// against itself, so it stays green even when parity is red and provides
// a regression signal independent of the Go reference's release cadence.
// ---------------------------------------------------------------------------

class IntegrationMcpSpec : public ::testing::TestWithParam<SpecCase> {};

TEST_P(IntegrationMcpSpec, MatchesGolden) {
    ExpectSpecMatches(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    All,
    IntegrationMcpSpec,
    ::testing::ValuesIn(DiscoverIntegrationSpecsFromTestsDir("mcp")),
    [](const ::testing::TestParamInfo<SpecCase>& param_info) {
        return SpecCaseInstanceName(param_info.param);
    });

}  // namespace
}  // namespace lci::integration
