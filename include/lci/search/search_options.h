#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lci/language_map.h>
#include <lci/types.h>

namespace lci {

// -- Search constants ---------------------------------------------------------

/// Default lines of context for search results.
inline constexpr int kDefaultContextLines = 50;

/// Approximate tokens per line of context.
inline constexpr int kTokensPerContextLine = 20;

/// Maximum matches per file before truncation.
inline constexpr int kMaxMatchesPerFile = 100;

/// Largest context window a caller may request (MCP `output=ctx:N`). N was
/// previously passed through unbounded, so a single row could carry an entire
/// file and one response could exceed any sane token budget. Requests above
/// this are clamped; the ceiling matches kDefaultContextLines, the widest
/// window the engine produces on its own.
inline constexpr int kMaxRequestedContextLines = kDefaultContextLines;

/// Longest accepted search pattern, in bytes. Longer patterns are rejected
/// with an explicit SearchStats::error rather than silently returning no
/// matches (Karpathy rule 6: fail fast, surface signal).
inline constexpr std::size_t kMaxSearchPatternBytes = 1000;

/// Scoring constants for search ranking.
inline constexpr double kCodeFileBoost = 50.0;
inline constexpr double kDocFilePenalty = -20.0;
inline constexpr double kConfigFileBoost = 10.0;
/// Flat offset applied to EVERY SearchEngine::score_result score. It is named
/// for a symbol-awareness dimension the search path never had: nothing on the
/// scoring path resolves a match to a symbol, so this was applied
/// unconditionally from the start and the `has_symbol` parameter that gated it
/// was dead. The parameter is gone; the constant stays because it is baked
/// into emitted scores (the MCP handler's refs-enrichment cut is a raw score
/// of 50, and a doc-file match lands exactly on it).
inline constexpr double kNonSymbolPenalty = -30.0;
inline constexpr double kRequireSymbolPenalty = -1000.0;
inline constexpr double kWordBoundaryBonus = 50.0;
inline constexpr double kLineStartBonus = 25.0;
inline constexpr double kExactCaseBonus = 20.0;
inline constexpr double kBaseMatchScore = 100.0;
inline constexpr double kAdditionalPatternCoverageBoost = 0.15;
inline constexpr double kPatternCoverageBoostCap = 0.5;
// A compound identifier must outrank a line-start keyword plus a separate
// exact term. With current match-quality bounds, +100% is the smallest round
// multiplier that preserves that ordering across code-file positions.
inline constexpr double kIdentifierCoverageBoost = 1.0;

// -- File classification ------------------------------------------------------

/// Classification of files for ranking purposes.
enum class FileCategory : uint8_t {
    Code = 0,
    Documentation,
    Config,
    Test,
    Unknown,
};

/// Returns the file extension (including the leading dot) of a path, taken from
/// the final path element only. Mirrors Go's filepath.Ext: a dot in a parent
/// directory name never leaks into the extension. Empty if the basename has no
/// dot.
std::string_view file_extension(std::string_view path);

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

    // -- Rich-search parity (Go cmd/lci/mcp.SearchParams) ---------------------
    // Mirrors Go internal/mcp/handlers.go:1167+ buildSearchOptions output so
    // C++ MCP `search` handler can carry the same filters down to the engine.
    // All defaults preserve current behavior.

    /// Treat pattern as RE2 regex (Karpathy rule: no std::regex on hot path).
    bool use_regex{false};

    /// Enable semantic word-split expansion for multi-word patterns.
    /// Default true to match Go's SearchParams.Semantic default (handler
    /// always sets it on for MCP callers; CLI keeps it off via flags).
    bool semantic{true};

    /// Filter results to enclosing symbol of one of these types
    /// (function, class, method, …). Empty = no filter.
    std::vector<std::string> symbol_types;

    /// Multi-pattern OR list (already split from CSV).
    /// When non-empty, engine runs each pattern, OR-merges results, and
    /// applies word-coverage scoring (boost per additional matched pattern).
    std::vector<std::string> pattern_list;

    /// Regex pattern (RE2 syntax) that file paths must match. Built by the
    /// handler from `languages[]` → file-extension alternation.
    /// e.g. \.(go|ts|tsx)$
    std::string include_pattern;

    /// Regex pattern that excludes file paths.
    std::string exclude_pattern;

    /// Include-only glob filters built from the MCP `filter` param
    /// (comma-separated languages/extensions/globs, e.g. "go,*.md,src/**").
    /// A file survives when ANY glob matches its root-relative path.
    /// Empty = no filtering.
    std::vector<std::string> filter_globs;

    /// Root-relative path scope (the MCP `path` param). Non-glob values are
    /// directory-prefix matches ("src/http" matches src/http/**); values
    /// containing * or ? are matched with FileScanner::match_glob semantics
    /// against the root-relative path. Empty = no scoping.
    std::string path_scope;

    /// Trailing CLI path arguments (`lci grep pattern <path>...`, ripgrep
    /// `rg pattern [path...]` convention). Each entry is a root-relative file
    /// (exact match) or directory prefix ("sklearn/utils" matches
    /// sklearn/utils/**). A candidate survives when it matches ANY entry.
    /// Distinct from the single `path_scope` above (the MCP `path` param):
    /// this list carries the multi-path CLI positional. Empty = no scoping.
    std::vector<std::string> path_scopes;

    /// Always populate `object_id` enrichment in handler output. Read by the
    /// MCP handler, not the engine — engine never strips it.
    bool include_object_ids{true};
};

/// Aggregate outcome of a search beyond the (possibly truncated) result rows.
/// Filled by SearchEngine::search when the caller passes a non-null stats
/// pointer; lets handlers report the TRUE universe size instead of the old
/// total==max cap-saturation, plus a directory histogram for narrowing.
struct SearchStats {
    /// Matches found across all candidates before the output cap. Exact when
    /// hit_collection_cap is false; a lower bound otherwise.
    int total_found{0};

    /// True when even the over-collection cap (max_results*8, <=2000) was
    /// reached — total_found is then "at least this many".
    bool hit_collection_cap{false};

    /// Root-relative top-level directory -> match count over the full
    /// pre-truncation set, sorted by count desc then name. "." holds
    /// root-level files.
    std::vector<std::pair<std::string, int>> dir_counts;

    /// Non-empty when the query was rejected before any file was scanned
    /// (empty or over-long pattern). Distinguishes "invalid query" from
    /// "valid query, no matches" -- both return an empty result vector, so
    /// without this the caller cannot tell them apart. Callers that pass a
    /// SearchStats must surface it; callers that pass nullptr opt out.
    std::string error;
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

    /// True when this row was produced by a SYNONYM-expanded pattern rather
    /// than the pattern the caller typed. SearchCoordinator::rank orders
    /// original-pattern rows ahead of synonym rows outright, ahead of score:
    /// an expansion is a guess, and a well-placed guess must not outrank a
    /// poorly-placed hit on the actual query. Ordering on provenance directly
    /// rather than via a score penalty keeps the guarantee exact instead of
    /// making it depend on the scoring constants staying in a particular
    /// numeric relationship. Always false on the single-pattern path.
    /// Declared last so the existing aggregate initializations stay valid.
    bool from_synonym{false};

    /// Distinct matched substrings that contributed to this ranked source
    /// line. Single-pattern searches leave this empty and callers use
    /// `match_text`; multi-pattern searches populate it in query order.
    std::vector<std::string> match_texts;
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

/// Returns true if `line` carries nothing but a comment, judged for `lang`.
///
/// THE per-line comment-classification rule. Backs SearchOptions::exclude_comments
/// and the CLI (`--exclude-comments`, src/cli/grep_filters.cpp
/// apply_exclude_comments), and tests/cli_test.cpp pins their line-for-line
/// agreement ON THE SINGLE-LINE PATH. A line is comment-only when its trimmed
/// form OPENS with `//` or `/*`, opens with `#` in a language where `#` is
/// unambiguously a comment (Python, Ruby; PHP excluding `#[` attributes), or is
/// exactly `*/`. A code line with a TRAILING comment — `int x = 1; /* note */` —
/// is NOT comment-only and is kept; so is a `"*/"` string literal.
///
/// Accepted residual OF THIS PER-LINE PREDICATE: a block-comment CONTINUATION
/// (" * prose") cannot be separated from a continued expression
/// ("* stats.confidence);") from one line alone, so it is kept. The MCP/HTTP
/// search path closes that residual by consulting enclosing block state — see
/// line_is_comment_only_with_spans; the CLI single-line grep path still uses
/// this predicate directly and keeps the residual.
bool line_is_comment_only(std::string_view line, LangId lang);

/// A half-open byte range [start,end) inside file content that is one C-style
/// block comment (`/* ... */`). Produced sorted and non-overlapping by
/// scan_block_comment_ranges, so a line's comment-membership is a monotonic
/// walk rather than a per-line re-scan.
struct CommentByteRange {
    int start{};
    int end{};
};

/// Returns the byte ranges of every CLOSED C-style block comment in `content`,
/// ascending and non-overlapping. Empty for languages with no `/* */` blocks
/// (Python, Ruby) and for `LangId::Unknown`. `//` line comments and string /
/// char literals are skipped while scanning, so a `/*` or `*/` inside them
/// never opens or closes a block; an UNTERMINATED `/*` is not recorded, so a
/// stray opener cannot swallow the rest of the file (fail toward keeping code).
/// One forward pass, no per-line allocation; the result is deterministic for a
/// given `content`.
std::vector<CommentByteRange> scan_block_comment_ranges(std::string_view content,
                                                        LangId lang);

/// Per-line comment classification that also knows a line sits INSIDE an open
/// block comment, which the per-line `line_is_comment_only` cannot decide.
/// Returns true when the line carries nothing but a comment: either
/// `line_is_comment_only` on its text, or its trimmed bytes fall wholly inside
/// one of `spans` (the continuation case). A line with code before the block
/// opener (`int x = 1; /* … */`) is NOT comment-only — its trimmed start lies
/// before the span.
///
/// `spans` must be sorted ascending. `span_pos` is a caller-held cursor into
/// it, advanced monotonically; callers MUST query lines in ascending
/// line_start order (both search filter loops do). No allocation per line.
bool line_is_comment_only_with_spans(std::string_view content,
                                     int line_start, int line_end,
                                     LangId lang,
                                     const std::vector<CommentByteRange>& spans,
                                     size_t& span_pos);

/// Returns true if there is a word boundary at the given position.
bool is_word_boundary(std::string_view content, int pos);

/// Returns a pattern complexity score (higher = more specific).
int calculate_pattern_complexity(std::string_view pattern);

/// Returns a match quality score based on context.
double calculate_match_quality(std::string_view content,
                               int match_start, int match_end,
                               std::string_view pattern);

/// Binary search for line number given sorted line offsets (int version).
int search_binary_line_offset(const std::vector<int>& offsets, int offset);

/// Binary search for the 1-based line number given the per-file line-start
/// byte offsets (uint32_t version, as stored by FileContentStore).
int search_binary_line_offset(const std::vector<uint32_t>& offsets, int offset);

/// Shared literal/regex content matcher backing both SearchEngine::find_matches
/// and MasterIndex::execute_search. Returns byte-offset matches; thread_local
/// RE2 cache + lowercase buffers keep it allocation-free across a candidate scan.
std::vector<SearchMatch> find_content_matches(std::string_view content,
                                              std::string_view pattern,
                                              const SearchOptions& options);

}  // namespace lci
