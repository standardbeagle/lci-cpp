#include "integration/spec_runner.h"

namespace lci::integration {
namespace {

TEST(SpecMigrationTest, CliConfigValidate) {
    ExpectSpecMatches({
        .descriptor_rel_path = "parity/descriptors/cli/config/validate.parity.json",
        .golden_rel_path = "integration/goldens/cli/config/validate.txt",
        .actual_source = SpecCase::ActualSource::Stdout,
        .parse_override = std::nullopt,
    });
}

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

}  // namespace
}  // namespace lci::integration
