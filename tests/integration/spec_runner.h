#pragma once

#include <gtest/gtest.h>

#include "spec_diff/assert_matches.h"

#include <optional>
#include <string>

namespace lci::integration {

struct SpecCase {
    enum class ActualSource {
        Stdout,
        OutputFile,
    };

    std::string descriptor_rel_path;
    std::string golden_rel_path;
    ActualSource actual_source = ActualSource::Stdout;
    std::optional<spec_diff::SpecDescriptor::Parse> parse_override;
};

void ExpectSpecMatches(const SpecCase& spec_case);

}  // namespace lci::integration
