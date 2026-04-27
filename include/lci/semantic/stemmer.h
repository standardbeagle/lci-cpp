#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lci {

/// Porter2 stemmer for word normalization.
/// Enables finding similar words in different forms
/// (authenticate, authentication, authenticating -> authent).
class Stemmer {
  public:
    Stemmer(bool enabled, std::string_view algorithm,
            int min_length, std::unordered_set<std::string> exclusions);

    /// Creates a disabled stemmer.
    static Stemmer disabled();

    bool is_enabled() const { return enabled_; }
    std::string_view algorithm() const { return algorithm_; }
    int min_length() const { return min_length_; }

    /// Returns the stem of a word, or the original if disabled/excluded/short.
    std::string stem(std::string_view word) const;

    /// Stems all words in a list.
    std::vector<std::string> stem_all(const std::vector<std::string>& words) const;

    /// Groups words by their stem.
    std::unordered_map<std::string, std::vector<std::string>>
    stem_and_group(const std::vector<std::string>& words) const;

    bool is_excluded(std::string_view word) const;

  private:
    bool enabled_;
    std::string algorithm_;
    int min_length_;
    std::unordered_set<std::string> exclusions_;
};

/// Porter2 stemming algorithm implementation.
/// Produces identical output to the Go surgebase/porter2 library.
std::string porter2_stem(std::string_view word);

}  // namespace lci
