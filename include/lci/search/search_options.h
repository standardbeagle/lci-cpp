#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lci/types.h>

namespace lci {

// -- Search constants ---------------------------------------------------------

/// Default lines of context for search results.
inline constexpr int kDefaultContextLines = 50;

/// Approximate tokens per line of context.
inline constexpr int kTokensPerContextLine = 20;

/// Maximum matches per file before truncation.
inline constexpr int kMaxMatchesPerFile = 100;

/// Scoring constants for search ranking.
inline constexpr double kCodeFileBoost = 50.0;
inline constexpr double kDocFilePenalty = -20.0;
inline constexpr double kConfigFileBoost = 10.0;
inline constexpr double kNonSymbolPenalty = -30.0;
inline constexpr double kRequireSymbolPenalty = -1000.0;
inline constexpr double kWordBoundaryBonus = 50.0;
inline constexpr double kLineStartBonus = 25.0;
inline constexpr double kExactCaseBonus = 20.0;
inline constexpr double kBaseMatchScore = 100.0;

// -- File classification ------------------------------------------------------

/// Classification of files for ranking purposes.
enum class FileCategory : uint8_t {
    Code = 0,
    Documentation,
    Config,
    Test,
    Unknown,
};

/// Classifies a file by its path and extension.
FileCategory classify_file(std::string_view path);

/// Returns a ranking score adjustment for a file's extension.
double score_file_type(std::string_view path);

/// Returns true if the path appears to be a test file.
bool is_test_file(std::string_view path);

// -- Search options -----------------------------------------------------------

/// Stack-allocated search options controlling search behavior.
/// All fields have sane defaults so callers only set what they need.
///
/// This struct replaces the minimal SearchOptions that was previously
/// defined in master_index.h, adding full parity with the Go version.
struct SearchOptions {
    int max_results{100};
    int max_context_lines{0};
    bool case_insensitive{false};
    bool declaration_only{false};
    bool usage_only{false};
    bool full_function{false};
    bool ensure_complete_stmt{false};
    int max_function_lines{500};
    bool exclude_tests{false};
    bool exclude_comments{false};
    bool word_boundary{false};
    bool invert_match{false};
    bool merge_file_results{false};
    int max_count_per_file{0};
};

// -- Search result types ------------------------------------------------------

/// A single match within a file (byte offsets).
struct SearchMatch {
    int start{};
    int end{};
    bool exact{};
};

/// Context lines surrounding a search match.
struct SearchContext {
    std::vector<std::string> lines;
    int start_line{};
    int end_line{};
    std::string block_type;
    std::string block_name;
    bool is_complete{true};
};

/// A scored search result with file location and context.
struct SearchResult {
    FileID file_id{};
    std::string path;
    int line{};
    int column{};
    std::string match_text;
    double score{};
    SearchContext context;
};

// -- Search-specific pure helper functions ------------------------------------
// Note: count_lines() lives in <lci/core/line_scanner.h>.
// Note: compute_line_offsets() lives in <lci/core/file_content_store.h>.

/// Returns the 1-based line number for a byte offset in content.
int search_line_number(std::string_view content, int offset);

/// Returns the byte offset of the start of the line containing offset.
int search_line_start(std::string_view content, int offset);

/// Returns the byte offset of the end of the line containing offset.
int search_line_end(std::string_view content, int offset);

/// Returns true if the byte is a word character (alphanumeric or underscore).
bool is_word_character(char c);

/// Returns true if there is a word boundary at the given position.
bool is_word_boundary(std::string_view content, int pos);

/// Finds all literal occurrences of pattern in content. Returns start offsets.
std::vector<int> find_literal_occurrences(std::string_view content,
                                          std::string_view pattern);

/// Finds case-insensitive literal occurrences. Returns start offsets.
std::vector<int> find_literal_occurrences_ci(std::string_view content,
                                             std::string_view pattern);

/// Finds occurrences where pattern appears as a whole word.
std::vector<int> find_whole_word_occurrences(std::string_view content,
                                             std::string_view pattern);

/// Returns a pattern complexity score (higher = more specific).
int calculate_pattern_complexity(std::string_view pattern);

/// Returns a match quality score based on context.
double calculate_match_quality(std::string_view content,
                               int match_start, int match_end,
                               std::string_view pattern);

/// Binary search for line number given sorted line offsets (int version).
int search_binary_line_offset(const std::vector<int>& offsets, int offset);

}  // namespace lci
