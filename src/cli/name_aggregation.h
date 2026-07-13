#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace lci::cli {

/// Aggregates a caller/callee name list into "name xN" entries, production
/// names before test_* names, most frequent first, capped at max_shown.
/// Hot symbols on real corpora have hundreds of same-named callers (every
/// estimator's `fit`); raw dumping made inspect output unreadable.
inline std::string format_aggregated_names(
    const std::vector<std::string>& names, size_t max_shown = 25) {
    std::vector<std::pair<std::string, int>> counts;  // first-seen order
    for (const auto& n : names) {
        auto it = std::find_if(counts.begin(), counts.end(),
                               [&](const auto& p) { return p.first == n; });
        if (it == counts.end()) {
            counts.emplace_back(n, 1);
        } else {
            ++it->second;
        }
    }

    auto is_test_name = [](const std::string& n) {
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
        char buf[32];
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
