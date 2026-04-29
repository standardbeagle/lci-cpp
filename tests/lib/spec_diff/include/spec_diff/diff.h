// tests/lib/spec_diff/include/spec_diff/diff.h
//
// Tier-aware structural diff over canonicalized JSON. Pure: takes two
// already-canonicalized JSON values and a tier-aware DiffOptions, returns
// a structured DiffResult with per-field reasons + a unified diff for
// human inspection.
#pragma once

#include "spec_diff/tiers.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace spec_diff {

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
DiffResult diff(const nlohmann::json& expected,
                const nlohmann::json& actual,
                const DiffOptions& opts);

// Legacy alias retained because both the parity runner and its unit
// tests historically called this `compare`. Forwards to `diff`.
inline DiffResult compare(const nlohmann::json& expected,
                          const nlohmann::json& actual,
                          const DiffOptions& opts) {
    return diff(expected, actual, opts);
}

} // namespace spec_diff
