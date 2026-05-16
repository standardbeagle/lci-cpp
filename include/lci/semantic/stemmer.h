#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lci {

/// Porter2 (Snowball) stemmer for word normalization.
/// Enables finding similar words in different forms
/// (authenticate, authentication, authenticating -> authent).
///
/// Backed by libstemmer (Snowball upstream). Three-way byte-equivalence
/// with Go surgebase/porter2 is established by the 29,417-word voc.txt /
/// output.txt fixture (the canonical Snowball English fixture, also used
/// verbatim by surgebase as its acceptance test) — see
/// tests/data/porter2_fixture/ and PorterFixtureTest in semantic_test.cpp.
///
/// Hot-path discipline (Karpathy): the underlying sb_stemmer is created
/// once per thread (thread_local) and reused across calls; stem() writes
/// into a thread-local std::string buffer to avoid per-token allocation.
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

/// Stem a single word via libstemmer Porter2 (English UTF-8). Exposed for
/// tests and parity tooling — production callers go through lci::Stemmer.
std::string porter2_stem(std::string_view word);

}  // namespace lci
