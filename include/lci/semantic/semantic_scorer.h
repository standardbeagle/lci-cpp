#pragma once

#include <lci/semantic/fuzzy_matcher.h>
#include <lci/semantic/name_splitter.h>
#include <lci/semantic/score_types.h>
#include <lci/semantic/stemmer.h>
#include <lci/semantic/synonym_table.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lci {

// -- Concrete match detectors -------------------------------------------------

class ExactMatchDetector final : public MatchDetector {
  public:
    DetectResult detect(std::string_view query, std::string_view target_name,
                        std::string_view query_lower, std::string_view target_lower,
                        const ScoreLayers& config) const override;
};

class SubstringMatchDetector final : public MatchDetector {
  public:
    DetectResult detect(std::string_view query, std::string_view target_name,
                        std::string_view query_lower, std::string_view target_lower,
                        const ScoreLayers& config) const override;
};

class FuzzyMatchDetector final : public MatchDetector {
  public:
    explicit FuzzyMatchDetector(const FuzzyMatcher& matcher);
    DetectResult detect(std::string_view query, std::string_view target_name,
                        std::string_view query_lower, std::string_view target_lower,
                        const ScoreLayers& config) const override;
  private:
    const FuzzyMatcher& matcher_;
};

class StemmingMatchDetector final : public MatchDetector {
  public:
    StemmingMatchDetector(const NameSplitter& splitter, const Stemmer& stemmer);
    DetectResult detect(std::string_view query, std::string_view target_name,
                        std::string_view query_lower, std::string_view target_lower,
                        const ScoreLayers& config) const override;
  private:
    const NameSplitter& splitter_;
    const Stemmer& stemmer_;
};

class NameSplitMatchDetector final : public MatchDetector {
  public:
    explicit NameSplitMatchDetector(const NameSplitter& splitter);
    DetectResult detect(std::string_view query, std::string_view target_name,
                        std::string_view query_lower, std::string_view target_lower,
                        const ScoreLayers& config) const override;
  private:
    const NameSplitter& splitter_;
};

class AbbreviationMatchDetector final : public MatchDetector {
  public:
    explicit AbbreviationMatchDetector(const NameSplitter& splitter);
    DetectResult detect(std::string_view query, std::string_view target_name,
                        std::string_view query_lower, std::string_view target_lower,
                        const ScoreLayers& config) const override;
  private:
    const NameSplitter& splitter_;
};

/// Matches when a query word and a target word belong to the same synonym
/// group (login<->signin, delete/remove/erase). The query-word x target-word
/// matrix makes the relation bidirectional for free.
class SynonymMatchDetector final : public MatchDetector {
  public:
    SynonymMatchDetector(const NameSplitter& splitter, const SynonymTable& table);
    DetectResult detect(std::string_view query, std::string_view target_name,
                        std::string_view query_lower, std::string_view target_lower,
                        const ScoreLayers& config) const override;
  private:
    const NameSplitter& splitter_;
    const SynonymTable& table_;
};

class PhraseMatchDetector final : public MatchDetector {
  public:
    PhraseMatchDetector(const NameSplitter& splitter,
                        const FuzzyMatcher& fuzzer,
                        const Stemmer& stemmer);
    DetectResult detect(std::string_view query, std::string_view target_name,
                        std::string_view query_lower, std::string_view target_lower,
                        const ScoreLayers& config) const override;
  private:
    const NameSplitter& splitter_;
    const FuzzyMatcher& fuzzer_;
    const Stemmer& stemmer_;
};

// -- Vocabulary analysis (forward declarations) -------------------------------

/// A symbol extracted from a file for vocabulary analysis.
struct FileSymbol {
    std::string file_path;
    std::string name;
    std::string type;      ///< function, variable, type, etc.
    bool is_exported{};
};

/// Configuration for production code filtering.
struct ProjectConfig {
    std::string language;
    std::vector<std::string> source_dirs;
    std::vector<std::string> test_markers;
    std::vector<std::string> exclude_dirs;
};

/// Result of vocabulary domain analysis.
struct DomainResult {
    std::string name;
    int count{};
    double confidence{};
    std::vector<std::string> example_symbols;
    std::vector<std::string> matched_terms;
};

/// Statistics for a specific term.
struct TermResult {
    std::string term;
    int count{};
    std::vector<std::string> example_symbols;
    std::vector<std::string> domains;
};

/// Analysis scope statistics.
struct ScopeStatistics {
    int total_files{};
    int production_files{};
    int test_files_excluded{};
    std::vector<std::string> source_directories;
    int total_symbols{};
    int total_functions{};
    int total_variables{};
    int total_types{};
};

/// Complete vocabulary analysis result.
struct VocabularyAnalysis {
    std::vector<DomainResult> domains_present;
    std::vector<std::string> domains_absent;
    std::vector<TermResult> unique_terms;
    std::vector<TermResult> common_terms;
    ScopeStatistics analysis_scope;
    int vocabulary_size{};
};

/// Filters symbols to only production code.
std::vector<FileSymbol> filter_production_symbols(
    const std::vector<FileSymbol>& symbols,
    const ProjectConfig& config);

// -- Semantic scorer ----------------------------------------------------------

/// Unified scoring across all semantic search layers.
/// Runs match detectors in priority order and returns the best match.
class SemanticScorer {
  public:
    SemanticScorer(std::shared_ptr<NameSplitter> splitter, Stemmer stemmer,
                   FuzzyMatcher fuzzer,
                   SynonymTable synonyms = SynonymTable::build_default());

    /// Updates the scoring configuration.
    void configure(const ScoreLayers& layers);

    /// Returns the current configuration.
    const ScoreLayers& config() const { return config_; }

    /// Scores a single symbol against a query.
    SemanticScore score_symbol(std::string_view query,
                               std::string_view symbol_name) const;

    /// Scores multiple symbols and returns them ranked by score.
    std::vector<ScoredSymbol> score_multiple(
        std::string_view query,
        const std::vector<std::string>& symbol_names) const;

    /// Performs a complete semantic search with timing.
    SemanticSearchResult search(std::string_view query,
                        const std::vector<std::string>& candidates) const;

    /// Clears the query normalization cache.
    void clear_cache();

  private:
    ScoreLayers config_;
    std::shared_ptr<NameSplitter> splitter_;
    Stemmer stemmer_;
    FuzzyMatcher fuzzer_;
    // Owned, immutable. Detectors hold a const& into this member; stable for
    // the scorer's lifetime (the scorer is neither moved nor copied).
    SynonymTable synonyms_;

    // Owned match detectors in priority order.
    std::vector<std::unique_ptr<MatchDetector>> detectors_;

    // Match type for each detector index.
    static MatchType index_to_match_type(int index);
    static double match_type_to_confidence(MatchType mt);

    mutable LRUCache query_cache_;
};

}  // namespace lci
