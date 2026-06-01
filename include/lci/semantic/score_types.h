#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lci {

// -- Match types --------------------------------------------------------------

/// Classification of semantic match found during scoring.
enum class MatchType : uint8_t {
    Exact = 0,
    Substring,
    Phrase,
    Annotation,
    Fuzzy,
    Stemming,
    Abbreviation,
    Synonym,
    NameSplit,
    None,
};

/// Returns the string name for a MatchType value.
constexpr std::string_view to_string(MatchType mt) {
    switch (mt) {
        case MatchType::Exact: return "exact";
        case MatchType::Substring: return "substring";
        case MatchType::Phrase: return "phrase";
        case MatchType::Annotation: return "annotation";
        case MatchType::Fuzzy: return "fuzzy";
        case MatchType::Stemming: return "stemming";
        case MatchType::Abbreviation: return "abbreviation";
        case MatchType::Synonym: return "synonym";
        case MatchType::NameSplit: return "name_split";
        case MatchType::None: return "no_match";
    }
    return "unknown";
}

// -- Semantic score -----------------------------------------------------------

/// Scoring result for a single symbol match.
struct SemanticScore {
    double score{};           ///< Final combined score (0.0-1.0).
    MatchType query_match{MatchType::None};
    double confidence{};      ///< Confidence in the match (0.0-1.0).
    std::string justification;
    std::unordered_map<std::string, std::string> match_details;

    /// Checks if the score is within acceptable bounds.
    bool is_valid() const {
        return score >= 0.0 && score <= 1.0 &&
               confidence >= 0.0 && confidence <= 1.0;
    }
};

/// A symbol with its computed semantic score.
struct ScoredSymbol {
    std::string symbol;       ///< The symbol name being scored.
    SemanticScore score;
    int rank{};               ///< 1-based rank (1 = highest score).
};

/// Complete result of a semantic search.
struct SemanticSearchResult {
    std::string query;
    std::vector<ScoredSymbol> symbols;
    int candidates_considered{};
    int results_returned{};
    int64_t execution_time_ns{};
};

// -- Score configuration ------------------------------------------------------

/// Configuration for each scoring layer.
struct ScoreLayers {
    double exact_weight{1.0};
    double substring_weight{0.9};
    double phrase_weight{0.88};
    double annotation_weight{0.85};
    double fuzzy_weight{0.70};
    double stemming_weight{0.55};
    double name_split_weight{0.40};
    double abbreviation_weight{0.25};
    // Synonym match (login<->signin) is a strong semantic equivalence: ranked
    // above stemming (0.55) and below fuzzy (0.70). Fixed, not KDL-configurable.
    double synonym_weight{0.6};

    double fuzzy_threshold{0.7};
    int stem_min_length{3};

    int max_results{10};
    double min_score{0.2};
};

/// Default scoring configuration matching Go's DefaultScoreLayers.
inline constexpr ScoreLayers kDefaultScoreLayers{};

// -- LRU cache ----------------------------------------------------------------

/// Thread-safe LRU cache for normalized query data.
/// Stores pre-processed query information to avoid repeated computation.
struct NormalizedQuery {
    std::string original;
    std::vector<std::string> words;
    std::vector<std::string> stems;
};

class LRUCache {
  public:
    explicit LRUCache(int max_size);

    /// Retrieves a cached query, moving it to front. Returns nullptr if absent.
    const NormalizedQuery* get(const std::string& key);

    /// Inserts or updates a cache entry, evicting oldest if at capacity.
    void set(const std::string& key, NormalizedQuery value);

    /// Removes all entries.
    void clear();

    /// Returns current number of cached entries.
    int size() const;

  private:
    struct Entry {
        std::string key;
        NormalizedQuery value;
    };

    int max_size_;
    mutable std::mutex mu_;
    std::list<Entry> order_;
    std::unordered_map<std::string, std::list<Entry>::iterator> items_;
};

// -- Match detector interface -------------------------------------------------

/// Interface for match detection at a specific scoring layer.
class MatchDetector {
  public:
    virtual ~MatchDetector() = default;

    struct DetectResult {
        bool matched{};
        double score{};
        std::string justification;
        std::unordered_map<std::string, std::string> details;
    };

    /// Detects if query matches target. queryLower/targetLower are pre-normalized.
    virtual DetectResult detect(
        std::string_view query,
        std::string_view target_name,
        std::string_view query_lower,
        std::string_view target_lower,
        const ScoreLayers& config) const = 0;
};

}  // namespace lci
