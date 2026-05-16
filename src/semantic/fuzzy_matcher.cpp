#include <lci/semantic/fuzzy_matcher.h>

#include <algorithm>
#include <rapidfuzz/fuzz.hpp>
#include <rapidfuzz/distance/JaroWinkler.hpp>

// Thin wrapper over rapidfuzz-cpp (header-only, SIMD-accelerated).
// Public API returns normalized [0.0, 1.0] for stability with semantic_scorer,
// git/analyzer, and existing FuzzyMatcher* GTests. rapidfuzz fuzz::* returns
// [0, 100]; JaroWinkler::normalized_similarity returns [0, 1]. Scale at boundary.

namespace lci {

FuzzyMatcher::FuzzyMatcher(bool enabled, double threshold, std::string_view algorithm)
    : enabled_(enabled),
      threshold_(threshold >= 0.0 && threshold <= 1.0 ? threshold : 0.80),
      algorithm_(algorithm.empty() ? "jaro-winkler" : std::string(algorithm)) {}

bool FuzzyMatcher::match(std::string_view a, std::string_view b) const {
    if (!enabled_) return a == b;
    return similarity(a, b) >= threshold_;
}

double FuzzyMatcher::similarity(std::string_view a, std::string_view b) const {
    if (!enabled_) return a == b ? 1.0 : 0.0;
    if (a == b) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    if (algorithm_ == "levenshtein") return levenshtein_similarity(a, b);
    if (algorithm_ == "cosine") return cosine_similarity(a, b);
    return jaro_winkler(a, b);  // default: jaro-winkler / jaro_winkler
}

double FuzzyMatcher::jaro_winkler(std::string_view a, std::string_view b) const {
    return rapidfuzz::jaro_winkler_similarity(a, b);
}

double FuzzyMatcher::levenshtein_similarity(std::string_view a, std::string_view b) const {
    return rapidfuzz::fuzz::ratio(a, b) / 100.0;
}

double FuzzyMatcher::cosine_similarity(std::string_view a, std::string_view b) const {
    // token_set_ratio is rapidfuzz's set-based scorer; analogous to prior bigram cosine for short symbols.
    return rapidfuzz::fuzz::token_set_ratio(a, b) / 100.0;
}

std::vector<FuzzyMatch> FuzzyMatcher::find_matches(
    std::string_view target, const std::vector<std::string>& candidates) const {
    std::vector<FuzzyMatch> matches;
    matches.reserve(candidates.size());
    for (const auto& c : candidates) {
        double sim = similarity(target, c);
        if (sim >= threshold_) matches.push_back({c, sim});
    }
    std::sort(matches.begin(), matches.end(),
              [](const FuzzyMatch& x, const FuzzyMatch& y) { return x.similarity > y.similarity; });
    return matches;
}

}  // namespace lci
