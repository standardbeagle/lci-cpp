#include "integration/spec_runner.h"

#include <cctype>
#include <string>
#include <string_view>

namespace lci::integration {
namespace {

// ---------------------------------------------------------------------------
// One-off explicit tests for migration anchors that still live next to their
// parity descriptors (http/index/probes/mcp). These will be moved to the
// directory-walking pattern in the matching migration tasks (4/8 - 8/8).
// ---------------------------------------------------------------------------

TEST(SpecMigrationTest, HttpListSymbols) {
    ExpectSpecMatches({
        .descriptor_rel_path = "parity/descriptors/http/list-symbols.parity.json",
        .golden_rel_path = "integration/goldens/http/list-symbols.json",
        .actual_source = SpecCase::ActualSource::Stdout,
        .parse_override = std::nullopt,
    });
}

TEST(SpecMigrationTest, IndexSyntheticMultilang) {
    ExpectSpecMatches({
        .descriptor_rel_path = "parity/descriptors/index/synthetic-multilang.parity.json",
        .golden_rel_path = "integration/goldens/index/synthetic-multilang.json",
        .actual_source = SpecCase::ActualSource::Stdout,
        .parse_override = std::nullopt,
    });
}

TEST(SpecMigrationTest, ProbesGraph) {
    ExpectSpecMatches({
        .descriptor_rel_path = "parity/descriptors/probes/graph.parity.json",
        .golden_rel_path = "integration/goldens/probes/graph.dot",
        .actual_source = SpecCase::ActualSource::OutputFile,
        .parse_override = spec_diff::SpecDescriptor::Parse::Text,
    });
}

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

}  // namespace
}  // namespace lci::integration
