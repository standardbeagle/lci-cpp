#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lci/core/file_content_store.h>
#include <lci/scope.h>
#include <lci/search/search_options.h>

namespace lci {

class MasterIndex;

// -- Rich-search helpers (pure, hot-path-safe) --------------------------------

/// Returns true when pattern contains regex syntax (Go looksLikeRegex parity).
bool looks_like_regex(std::string_view pattern);

/// Splits whitespace-separated tokens into out. Allocates only string slots.
void split_on_spaces(std::string_view input, std::vector<std::string>& out);

/// Expands a single pattern into [pattern, words…>2 chars] for multi-word
/// queries. Single-word patterns return one-element vector.
std::vector<std::string> expand_pattern_semantic(std::string_view pattern);

// -- Context extractor --------------------------------------------------------

/// Extracts context around search matches with block awareness.
class ContextExtractor {
  public:
    explicit ContextExtractor(const FileContentStore& store,
                              int default_context_lines = kDefaultContextLines);

    /// Extracts context around a match line with default options.
    SearchContext extract(FileID file_id,
                          const std::vector<BlockBoundary>& blocks,
                          int match_line,
                          int max_context_lines) const;

    /// Extracts block-aware context using block boundaries.
    SearchContext extract_block_context(
        FileID file_id,
        const std::vector<BlockBoundary>& blocks,
        int match_line) const;

    /// Extracts function context with padding around match.
    SearchContext extract_function_context(
        FileID file_id,
        const std::vector<BlockBoundary>& blocks,
        int match_line,
        int max_context_lines) const;

  private:
    const FileContentStore& store_;
    int default_context_lines_;

    /// Extracts simple line-range context.
    SearchContext extract_line_context(FileID file_id,
                                       int match_line,
                                       int num_lines) const;

    /// Splits content into lines.
    std::vector<std::string_view> split_lines(std::string_view content) const;

    /// Finds the start of a function including leading comments.
    int find_function_start(const std::vector<std::string_view>& lines,
                            int block_start) const;
};

// -- Search engine ------------------------------------------------------------

/// Orchestrates trigram + symbol search with ranking and scoring.
class SearchEngine {
  public:
    explicit SearchEngine(MasterIndex& index);

    /// Searches for a pattern across all indexed files.
    std::vector<SearchResult> search(const std::string& pattern,
                                     const SearchOptions& options) const;

    /// Multi-pattern search (OR-merge + dedup + per-file coverage boost).
    /// Mirrors Go's searchAndDeduplicate (handlers.go:1372). Each pattern is
    /// run with the same SearchOptions; results keyed by file+line+match.
    /// Score boost: +0.15 per additional matching pattern, cap +0.5.
    std::vector<SearchResult> search(const std::vector<std::string>& patterns,
                                     const SearchOptions& options) const;

  private:
    MasterIndex& index_;
    ContextExtractor context_extractor_;

    /// Finds literal matches in file content.
    std::vector<SearchMatch> find_matches(
        std::string_view content,
        std::string_view pattern,
        const SearchOptions& options) const;

    /// Scores a result for ranking.
    double score_result(const SearchResult& result,
                        std::string_view pattern,
                        bool has_symbol) const;

    /// Processes a single file for matches and produces results.
    void process_file(FileID file_id,
                      std::string_view pattern,
                      const SearchOptions& options,
                      int effective_cap,
                      std::vector<SearchResult>& results) const;
};

// -- Search coordinator -------------------------------------------------------

/// Deduplicates and merges results from multiple search methods.
class SearchCoordinator {
  public:
    /// Deduplicates results that refer to the same file+line.
    /// Keeps the result with the highest score.
    static std::vector<SearchResult> deduplicate(
        std::vector<SearchResult> results);

    /// Merges two result sets, deduplicating overlaps.
    static std::vector<SearchResult> merge(
        std::vector<SearchResult> a,
        std::vector<SearchResult> b);

    /// Sorts results by score (descending), then path, then line.
    static void rank(std::vector<SearchResult>& results);
};

}  // namespace lci
