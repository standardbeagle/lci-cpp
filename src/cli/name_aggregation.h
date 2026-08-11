#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lci::cli {

/// Aggregates a caller/callee name list into "name xN" entries, production
/// names before test_* names, most frequent first, capped at max_shown.
/// Hot symbols on real corpora have hundreds of same-named callers (every
/// estimator's `fit`); raw dumping made inspect output unreadable.
inline std::string format_aggregated_names(
    const std::vector<std::string>& names, size_t max_shown = 25) {
    // Counts in first-seen order. The views borrow `names`, which outlives
    // every use below, so tallying costs no string copies. `seen` keeps the
    // tally O(n): a linear scan of `counts` per name was quadratic on exactly
    // the input this function exists for -- hot symbols whose caller lists run
    // to hundreds of repeats of the same name.
    std::vector<std::pair<std::string_view, int>> counts;  // first-seen order
    std::unordered_map<std::string_view, size_t> seen;     // name -> index
    counts.reserve(names.size());
    seen.reserve(names.size());
    for (const auto& n : names) {
        auto [it, inserted] = seen.emplace(std::string_view(n), counts.size());
        if (inserted) {
            counts.emplace_back(std::string_view(n), 1);
        } else {
            ++counts[it->second].second;
        }
    }

    auto is_test_name = [](std::string_view n) {
        return n.rfind("test_", 0) == 0;
    };
    std::stable_sort(counts.begin(), counts.end(),
                     [&](const auto& a, const auto& b) {
                         bool ta = is_test_name(a.first);
                         bool tb = is_test_name(b.first);
                         if (ta != tb) return !ta;
                         return a.second > b.second;
                     });

    std::string out;
    size_t shown = std::min(max_shown, counts.size());
    for (size_t i = 0; i < shown; ++i) {
        if (i > 0) out += ", ";
        out += counts[i].first;
        if (counts[i].second > 1) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), " x%d", counts[i].second);
            out += buf;
        }
    }
    if (counts.size() > shown) {
        // ", … +" is 7 bytes (the ellipsis is 3-byte UTF-8) plus " more" and a
        // 20-digit size_t: 33 including the NUL, so 32 could truncate.
        char buf[40];
        std::snprintf(buf, sizeof(buf), ", … +%zu more",
                      counts.size() - shown);
        out += buf;
    }
    if (counts.size() != names.size() || counts.size() > shown) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "  (%zu unique / %zu total)",
                      counts.size(), names.size());
        out += buf;
    }
    return out;
}

}  // namespace lci::cli
