#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lci/search/search_options.h>

namespace lci {

// -- Index type enumeration ---------------------------------------------------

/// Types of indexes available for search routing.
enum class IndexType : uint8_t {
    Trigram = 0,
    Symbol,
    Reference,
    CallGraph,
    Postings,
    Location,
    Content,
};

/// Returns the string name for an IndexType value.
constexpr std::string_view to_string(IndexType it) {
    switch (it) {
        case IndexType::Trigram: return "trigram";
        case IndexType::Symbol: return "symbol";
        case IndexType::Reference: return "reference";
        case IndexType::CallGraph: return "call_graph";
        case IndexType::Postings: return "postings";
        case IndexType::Location: return "location";
        case IndexType::Content: return "content";
    }
    return "unknown";
}

// -- Analysis result ----------------------------------------------------------

/// Result of requirements analysis with routing decisions.
struct AnalysisResult {
    std::vector<IndexType> required_indexes;
    std::vector<IndexType> optional_indexes;
    double confidence{0.5};
    int64_t estimated_cost{100};
    std::vector<std::string> reasoning;
    std::vector<std::string> optimization_hints;
};

// -- Analyzer configuration ---------------------------------------------------

/// Configuration for the requirements analyzer.
struct AnalyzerConfig {
    bool enable_pattern_analysis{true};
    bool enable_semantic_analysis{true};
    bool enable_context_analysis{true};
    int max_pattern_complexity{10};
    std::vector<IndexType> default_indexes{
        IndexType::Trigram, IndexType::Symbol};
};

// -- Requirements analyzer ----------------------------------------------------

/// Analyzes search queries and options to determine which indexes
/// should be consulted and in what order. Produces routing decisions
/// with confidence scores and cost estimates.
class RequirementsAnalyzer {
  public:
    RequirementsAnalyzer();
    explicit RequirementsAnalyzer(AnalyzerConfig config);

    /// Analyzes search requirements based on pattern and options.
    AnalysisResult analyze(std::string_view pattern,
                           const SearchOptions& options) const;

    /// Returns true if the given index type is in the result.
    static bool should_use_index(const AnalysisResult& result,
                                 IndexType index_type);

    /// Returns estimated search time in milliseconds.
    static int64_t estimated_search_time(const AnalysisResult& result);

  private:
    AnalyzerConfig config_;

    void analyze_pattern(AnalysisResult& result,
                         std::string_view pattern) const;
    void analyze_search_options(AnalysisResult& result,
                                const SearchOptions& options) const;
    void analyze_semantic_requirements(AnalysisResult& result,
                                       std::string_view pattern) const;
    void analyze_context_requirements(AnalysisResult& result,
                                      const SearchOptions& options) const;
    void calculate_metrics(AnalysisResult& result,
                           std::string_view pattern,
                           const SearchOptions& options) const;
    void generate_optimization_hints(AnalysisResult& result,
                                      std::string_view pattern,
                                      const SearchOptions& options) const;

    int calculate_pattern_complexity(std::string_view pattern) const;
    bool is_symbol_pattern(std::string_view pattern) const;
    bool is_file_path_pattern(std::string_view pattern) const;
    bool is_content_pattern(std::string_view pattern) const;
    bool has_semantic_annotations(std::string_view pattern) const;
    bool has_relationship_patterns(std::string_view pattern) const;
    bool has_architectural_patterns(std::string_view pattern) const;

    static void add_required(AnalysisResult& result, IndexType type,
                             std::string_view reason);
    static void add_optional(AnalysisResult& result, IndexType type,
                             std::string_view reason);
    static bool contains_index(const std::vector<IndexType>& indexes,
                               IndexType type);
    static int64_t get_index_cost(IndexType type);
};

}  // namespace lci
