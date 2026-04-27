#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lci {

/// Result of a fuzzy string match.
struct FuzzyMatch {
    std::string term;
    double similarity{};
};

/// Fuzzy string matching using Jaro-Winkler, Levenshtein, or cosine similarity.
class FuzzyMatcher {
  public:
    FuzzyMatcher(bool enabled, double threshold, std::string_view algorithm);

    bool is_enabled() const { return enabled_; }
    double threshold() const { return threshold_; }
    std::string_view algorithm() const { return algorithm_; }

    /// Checks if two strings are similar within the configured threshold.
    bool match(std::string_view a, std::string_view b) const;

    /// Returns the similarity score between two strings (0.0-1.0).
    double similarity(std::string_view a, std::string_view b) const;

    /// Finds all candidates similar to target, sorted by similarity descending.
    std::vector<FuzzyMatch> find_matches(
        std::string_view target,
        const std::vector<std::string>& candidates) const;

  private:
    double jaro_winkler(std::string_view a, std::string_view b) const;
    double levenshtein_similarity(std::string_view a, std::string_view b) const;
    double cosine_similarity(std::string_view a, std::string_view b) const;

    bool enabled_;
    double threshold_;
    std::string algorithm_;
};

}  // namespace lci
