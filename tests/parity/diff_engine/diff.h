#pragma once

#include "diff_engine/field_tier.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace lci::parity {

struct DiffOptions {
    TierMap     tiers;
    double      score_abs    = 0.01;    // ranked tolerance
    long long   timed_max_ms = 60000;   // timed range
    std::string id_pattern;             // regex; empty = type-only check
};

struct DiffResult {
    bool passed = true;
    std::vector<std::string> reasons;   // human-readable per-field reasons
    std::string unified_diff;           // unified diff of canonicalized JSON
};

// Compare two canonicalized JSON values per the tier map and tolerances.
// Inputs MUST already be canonicalized (sort_keys, num-normalize, etc.).
DiffResult compare(const nlohmann::json& go,
                   const nlohmann::json& cpp,
                   const DiffOptions& opts);

} // namespace lci::parity
