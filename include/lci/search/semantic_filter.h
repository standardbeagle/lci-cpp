#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lci/core/file_content_store.h>
#include <lci/search/search_options.h>
#include <lci/types.h>

namespace lci {

// -- Semantic match -----------------------------------------------------------

/// A search match with line-level information for semantic filtering.
struct SemanticMatch {
    int line{};        ///< 1-based line number.
    int start{};       ///< Byte offset of match start.
    int end{};         ///< Byte offset of match end.
};

// -- Symbol location map ------------------------------------------------------

/// Pre-computed line-to-symbol mapping for zero-allocation lookups.
/// Built once per file, then consulted per-match without allocation.
struct SymbolLineEntry {
    int line{};
    std::string_view name;
    SymbolType type{};
    bool is_exported{};
    bool is_mutable{};
    VariableType variable_type{};
};

// -- Semantic filter ----------------------------------------------------------

/// Performs symbol-based filtering on search matches using pre-computed
/// line-to-symbol maps. Designed for zero-allocation hot-path lookups.
class SemanticFilter {
  public:
    explicit SemanticFilter(const FileContentStore& store);

    /// Filters matches based on semantic options (symbol types,
    /// declaration/usage, exported, comments).
    /// Returns only matches that pass all active filters.
    std::vector<SemanticMatch> apply_filter(
        FileID file_id,
        std::string_view content,
        const std::vector<SymbolLineEntry>& symbol_map,
        const std::vector<SemanticMatch>& matches,
        std::string_view pattern,
        const SearchOptions& options) const;

    /// Returns true if a line is a comment.
    static bool is_comment_line(std::string_view line);

    /// Finds the 1-based line number for a byte offset in content.
    static int line_for_offset(std::string_view content, int offset);

  private:
    const FileContentStore& store_;

    bool passes_filter(
        std::string_view content,
        const std::vector<SymbolLineEntry>& symbol_map,
        const SemanticMatch& match,
        std::string_view pattern,
        const SearchOptions& options) const;

    const SymbolLineEntry* find_symbol_at_line(
        const std::vector<SymbolLineEntry>& symbol_map,
        int line,
        std::string_view pattern) const;

    static bool is_exported_symbol(const SymbolLineEntry& entry);

    static bool contains_case_insensitive(
        const std::vector<std::string>& haystack,
        std::string_view needle);
};

}  // namespace lci
