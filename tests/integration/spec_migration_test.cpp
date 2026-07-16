#include "integration/spec_runner.h"

#include <cctype>
#include <string>
#include <string_view>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace lci::integration {
namespace {

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
