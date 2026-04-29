// tests/lib/spec_diff/src/tiers.cpp
#include "spec_diff/tiers.h"

#include <algorithm>
#include <cctype>

namespace spec_diff {

namespace {
bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

FieldTier classify_path(const TierMap& m, const std::string& path) {
    // Order: explicit ignore -> ids -> timed -> ranked -> stable -> default Stable.
    if (contains(m.ignore, path)) return FieldTier::Ignore;
    if (contains(m.ids, path))    return FieldTier::Id;
    if (contains(m.timed, path))  return FieldTier::Timed;
    if (contains(m.ranked, path)) return FieldTier::Ranked;
    if (contains(m.stable, path)) return FieldTier::Stable;
    return FieldTier::Stable;
}

std::string normalize_indexes(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '[') {
            size_t j = i + 1;
            while (j < path.size() && std::isdigit(static_cast<unsigned char>(path[j]))) {
                ++j;
            }
            if (j < path.size() && path[j] == ']' && j > i + 1) {
                out += "[]";
                i = j + 1;
                continue;
            }
        }
        out.push_back(path[i]);
        ++i;
    }
    return out;
}

} // namespace spec_diff
