#pragma once

#include <gtest/gtest.h>

#include "spec_diff/assert_matches.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lci::integration {

struct SpecCase {
    enum class ActualSource {
        Stdout,
        OutputFile,
    };

    // Path of the descriptor file relative to the tests/ source directory.
    std::string descriptor_rel_path;
    // Path of the golden file relative to the tests/ source directory.
    std::string golden_rel_path;
    ActualSource actual_source = ActualSource::Stdout;
    std::optional<spec_diff::SpecDescriptor::Parse> parse_override;
};

void ExpectSpecMatches(const SpecCase& spec_case);

// Load a SpecCase from an integration .spec.json file. The spec JSON is the
// same shape as a parity descriptor with three additional optional top-level
// fields that drive the integration harness (parity descriptors ignore them):
//
//   "golden":         path to the golden file (required for integration
//                     specs); may be absolute, or relative to the spec
//                     file's directory, or relative to the
//                     tests/integration/ directory.
//   "actual_source":  "stdout" (default) or "output_file" — selects whether
//                     the comparison reads from captured stdout or from the
//                     artifact written to an --output argument.
//   "parse_override": "json" or "text" — overrides the parse style used by
//                     the diff engine when it differs from the descriptor's
//                     "parse" field (e.g. exit-only descriptors that still
//                     need text comparison against a golden).
//
// `spec_path` is an absolute path to the .spec.json file. Throws
// std::runtime_error if the file cannot be read or required fields are
// missing.
SpecCase LoadIntegrationSpec(const std::filesystem::path& spec_path);

// Walk `root_dir` recursively for files matching `*.spec.json`, load each
// one via LoadIntegrationSpec, and return the resulting cases sorted by
// descriptor path for stable test ordering. `root_dir` must exist;
// otherwise throws std::runtime_error.
std::vector<SpecCase> DiscoverIntegrationSpecs(
    const std::filesystem::path& root_dir);

// Same as DiscoverIntegrationSpecs but resolves the search root from the
// LCI_TESTS_SOURCE_DIR compile definition (set by the build system). This
// is the form used by GoogleTest parametrized fixtures, which need a
// callable that runs at static-init time. `subdir` is appended under
// `tests/integration/` (e.g. "cli" → tests/integration/cli/).
std::vector<SpecCase> DiscoverIntegrationSpecsFromTestsDir(
    const std::string& subdir);

}  // namespace lci::integration
