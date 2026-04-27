#pragma once

#include <string>
#include <vector>

namespace lci::parity {

enum class FieldTier { Stable, Ranked, Timed, Id, Ignore };

struct TierMap {
    std::vector<std::string> stable;
    std::vector<std::string> ranked;
    std::vector<std::string> timed;
    std::vector<std::string> ids;
    std::vector<std::string> ignore;
};

// Returns the tier for a given JSONPath-lite path. Default = Stable
// (fail-closed). Caller is responsible for normalizing array indexes
// (e.g. "results[3].file" -> "results[].file") before calling.
FieldTier classify_path(const TierMap& m, const std::string& path);

// Helper: rewrite "[N]" to "[]" for any integer N in the path.
std::string normalize_indexes(const std::string& path);

} // namespace lci::parity
