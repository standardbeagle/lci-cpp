#include "integration/spec_runner.h"

#include <cctype>
#include <string>
#include <string_view>

namespace lci::integration {
namespace {

// ---------------------------------------------------------------------------
// One-off explicit tests for migration anchors that still live next to their
// parity descriptors (mcp/*). These will be moved to the directory-walking
// pattern in the matching migration tasks (7/8 - 8/8).
// cli/* moved in migration 3/8, http/* in migration 4/8, index/* in
// migration 5/8, probes/* in migration 6/8 (see IntegrationProbesSpec block
// below).
// ---------------------------------------------------------------------------

TEST(SpecMigrationTest, McpInfoBasic) {
    ExpectSpecMatches({
        .descriptor_rel_path = "parity/descriptors/mcp/info/basic.parity.json",
        .golden_rel_path = "integration/goldens/mcp/info/basic.json",
        .actual_source = SpecCase::ActualSource::Stdout,
        .parse_override = std::nullopt,
    });
}

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

}  // namespace
}  // namespace lci::integration
