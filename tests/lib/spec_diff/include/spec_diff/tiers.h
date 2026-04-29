// tests/lib/spec_diff/include/spec_diff/tiers.h
//
// Field-tier classification for structured (JSON) parity / golden diffs.
// Pure data; no dependency on subprocess runners, binary paths, or any
// parity-harness-specific concept.
#pragma once

#include <string>
#include <vector>

namespace spec_diff {

enum class FieldTier { Stable, Ranked, Timed, Id, Ignore };

struct TierMap {
    std::vector<std::string> stable;
    std::vector<std::string> ranked;
    std::vector<std::string> timed;
    std::vector<std::string> ids;
    std::vector<std::string> ignore;
    // Array paths whose elements should be sorted (by canonical JSON
    // dump) before tier comparison runs. Use this when the upstream
    // producer emits an array in non-deterministic order (e.g. hash-map
    // iteration) and the diff should verify content rather than position.
    std::vector<std::string> sort_arrays;
};

// Returns the tier for a given JSONPath-lite path. Default = Stable
// (fail-closed). Caller is responsible for normalizing array indexes
// (e.g. "results[3].file" -> "results[].file") before calling.
FieldTier classify_path(const TierMap& m, const std::string& path);

// Helper: rewrite "[N]" to "[]" for any integer N in the path.
std::string normalize_indexes(const std::string& path);

} // namespace spec_diff
