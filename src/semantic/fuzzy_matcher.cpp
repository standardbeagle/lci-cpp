#include <lci/semantic/fuzzy_matcher.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace lci {

FuzzyMatcher::FuzzyMatcher(bool enabled, double threshold,
                           std::string_view algorithm)
    : enabled_(enabled),
      threshold_(threshold >= 0.0 && threshold <= 1.0 ? threshold : 0.80),
      algorithm_(algorithm.empty() ? "jaro-winkler" : std::string(algorithm)) {}

bool FuzzyMatcher::match(std::string_view a, std::string_view b) const {
    if (!enabled_) return a == b;
    return similarity(a, b) >= threshold_;
}

double FuzzyMatcher::similarity(std::string_view a, std::string_view b) const {
    if (!enabled_) return a == b ? 1.0 : 0.0;

    if (algorithm_ == "jaro-winkler") return jaro_winkler(a, b);
    if (algorithm_ == "levenshtein") return levenshtein_similarity(a, b);
    if (algorithm_ == "cosine") return cosine_similarity(a, b);
    return jaro_winkler(a, b);
}

double FuzzyMatcher::jaro_winkler(std::string_view a, std::string_view b) const {
    if (a == b) return 1.0;
    if (a.empty() || b.empty()) return 0.0;

    auto len_a = static_cast<int>(a.size());
    auto len_b = static_cast<int>(b.size());

    int match_distance = std::max(len_a, len_b) / 2 - 1;
    if (match_distance < 0) match_distance = 0;

    std::vector<bool> a_matches(static_cast<size_t>(len_a), false);
    std::vector<bool> b_matches(static_cast<size_t>(len_b), false);

    int matches = 0;
    int transpositions = 0;

    // Find matching characters.
    for (int i = 0; i < len_a; ++i) {
        int start = std::max(0, i - match_distance);
        int end = std::min(i + match_distance + 1, len_b);

        for (int j = start; j < end; ++j) {
            if (b_matches[static_cast<size_t>(j)] || a[static_cast<size_t>(i)] != b[static_cast<size_t>(j)])
                continue;
            a_matches[static_cast<size_t>(i)] = true;
            b_matches[static_cast<size_t>(j)] = true;
            ++matches;
            break;
        }
    }

    if (matches == 0) return 0.0;

    // Count transpositions.
    int k = 0;
    for (int i = 0; i < len_a; ++i) {
        if (!a_matches[static_cast<size_t>(i)]) continue;
        while (!b_matches[static_cast<size_t>(k)]) ++k;
        if (a[static_cast<size_t>(i)] != b[static_cast<size_t>(k)]) ++transpositions;
        ++k;
    }

    double jaro = (static_cast<double>(matches) / len_a +
                   static_cast<double>(matches) / len_b +
                   static_cast<double>(matches - transpositions / 2) / matches) / 3.0;

    // Winkler modification: boost for common prefix (up to 4 chars).
    int prefix = 0;
    int max_prefix = std::min(4, std::min(len_a, len_b));
    for (int i = 0; i < max_prefix; ++i) {
        if (a[static_cast<size_t>(i)] == b[static_cast<size_t>(i)]) {
            ++prefix;
        } else {
            break;
        }
    }

    return jaro + static_cast<double>(prefix) * 0.1 * (1.0 - jaro);
}

double FuzzyMatcher::levenshtein_similarity(std::string_view a,
                                             std::string_view b) const {
    if (a == b) return 1.0;
    if (a.empty() || b.empty()) return 0.0;

    auto len_a = a.size();
    auto len_b = b.size();

    // Use single-row DP for space efficiency.
    std::vector<int> prev(len_b + 1);
    std::vector<int> curr(len_b + 1);

    for (size_t j = 0; j <= len_b; ++j)
        prev[j] = static_cast<int>(j);

    for (size_t i = 1; i <= len_a; ++i) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= len_b; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }

    int distance = prev[len_b];
    auto max_len = static_cast<double>(std::max(len_a, len_b));
    return 1.0 - static_cast<double>(distance) / max_len;
}

double FuzzyMatcher::cosine_similarity(std::string_view a,
                                        std::string_view b) const {
    if (a == b) return 1.0;
    if (a.empty() || b.empty()) return 0.0;

    // Build bigram sets.
    auto get_bigrams = [](std::string_view s) -> std::unordered_set<std::string> {
        std::unordered_set<std::string> bigrams;
        if (s.size() < 2) {
            bigrams.insert(std::string(s));
            return bigrams;
        }
        for (size_t i = 0; i + 1 < s.size(); ++i) {
            bigrams.insert(std::string(s.substr(i, 2)));
        }
        return bigrams;
    };

    auto bigrams_a = get_bigrams(a);
    auto bigrams_b = get_bigrams(b);

    if (bigrams_a.empty() || bigrams_b.empty()) return 0.0;

    double intersection = 0.0;
    for (const auto& bg : bigrams_a) {
        if (bigrams_b.count(bg)) ++intersection;
    }

    double mag_a = std::sqrt(static_cast<double>(bigrams_a.size()));
    double mag_b = std::sqrt(static_cast<double>(bigrams_b.size()));

    if (mag_a == 0.0 || mag_b == 0.0) return 0.0;

    return intersection / (mag_a * mag_b);
}

std::vector<FuzzyMatch> FuzzyMatcher::find_matches(
    std::string_view target,
    const std::vector<std::string>& candidates) const {

    std::vector<FuzzyMatch> matches;
    for (const auto& candidate : candidates) {
        double sim = similarity(target, candidate);
        if (sim >= threshold_) {
            matches.push_back({candidate, sim});
        }
    }

    // Sort by similarity descending.
    std::sort(matches.begin(), matches.end(),
              [](const FuzzyMatch& a, const FuzzyMatch& b) {
                  return a.similarity > b.similarity;
              });

    return matches;
}

}  // namespace lci
