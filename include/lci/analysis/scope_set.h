#pragma once

// ScopeSet: one selection abstraction for "which folders / files / elements
// does this operation apply to", populated from independent sources — git
// diff hunks, glob patterns, path regexes, path attributes, or the results
// of a code-model query — and consumed by analyses that must scope their
// findings (first consumer: git-analyze, whose findings previously covered
// every symbol in a touched file instead of the change itself).
//
// Deep module contract: a ScopeSet answers only three questions —
// contains_file / contains_lines / contains_symbol — plus set algebra.
// Everything about where the selection came from stays in the populators.
//
// Representation: path -> sorted, merged, 1-based inclusive line ranges.
// An entry with NO ranges means "the whole file". `all()` matches
// everything (the identity for intersect, absorbing for unite). Paths are
// compared verbatim: populate and query with the same normalization
// (repo-relative, '/'-separated) — mixing absolute and relative paths is
// the self-match bug class git-analyze already hit once.

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace lci {

struct EnhancedSymbol;

struct LineRange {
    int start{};  ///< 1-based, inclusive
    int end{};    ///< 1-based, inclusive; >= start
    bool operator==(const LineRange& o) const {
        return start == o.start && end == o.end;
    }
};

class ScopeSet {
  public:
    /// Matches every file, line, and symbol.
    static ScopeSet all();
    /// Matches nothing (the default-constructed state).
    static ScopeSet none();

    bool is_all() const { return all_; }
    /// True when the scope can match nothing (not-all and no files).
    bool empty() const { return !all_ && files_.empty(); }
    size_t file_count() const { return files_.size(); }

    /// Adds a whole file. Overrides any narrower line ranges for the path.
    void add_file(std::string path);
    /// Adds a line range within a file. Ranges are merged on insert; a
    /// whole-file entry for the path stays whole-file.
    void add_lines(std::string path, LineRange range);

    bool contains_file(std::string_view path) const;
    /// True when [start,end] overlaps the scope's ranges for `path`
    /// (whole-file entries overlap everything in the file).
    bool contains_lines(std::string_view path, int start, int end) const;
    /// Span-overlap test for a symbol located in `path`.
    bool contains_symbol(std::string_view path, const EnhancedSymbol& sym) const;

    /// Set algebra. Intersecting two line-scoped entries keeps range
    /// overlaps; whole-file is the identity within a shared path.
    ScopeSet unite(const ScopeSet& other) const;
    ScopeSet intersect(const ScopeSet& other) const;

    /// Path -> ranges view (empty vector = whole file). Iteration order is
    /// hash order: sort before any user-visible emission.
    const absl::flat_hash_map<std::string, std::vector<LineRange>>& files()
        const {
        return files_;
    }

  private:
    bool all_{false};
    absl::flat_hash_map<std::string, std::vector<LineRange>> files_;
};

// ---------------------------------------------------------------------------
// Populators. Each returns a ScopeSet; composition is the caller's algebra.
// ---------------------------------------------------------------------------

/// Whole files by explicit path list.
ScopeSet scope_from_paths(const std::vector<std::string>& paths);

/// Whole files whose path matches ANY glob ('*' spans '/', '?' one char),
/// chosen from `candidate_paths`.
ScopeSet scope_from_globs(const std::vector<std::string>& globs,
                          const std::vector<std::string>& candidate_paths);

/// Whole files whose path matches an RE2 pattern (partial match), chosen
/// from `candidate_paths`. On a bad pattern returns none() and sets error.
ScopeSet scope_from_regex(const std::string& pattern,
                          const std::vector<std::string>& candidate_paths,
                          std::string& error);

/// Element scope from a code-model query result: each (path, symbol) adds
/// the symbol's line span. Callers run the query (index, communities,
/// fan-in, information score, ...) — the scope only records the answer.
ScopeSet scope_from_symbols(
    const std::vector<std::pair<std::string, const EnhancedSymbol*>>& symbols);

/// Parses unified-diff text (git diff -U0 form) into NEW-side line ranges
/// per file: "+++ b/<path>" headers and "@@ -a,b +c,d @@" hunks. Pure-file
/// deletions contribute a whole-file-absent entry (no ranges added).
/// Exposed for tests; git callers use Provider::get_changed_line_ranges.
ScopeSet scope_from_unified_diff(std::string_view diff_text);

}  // namespace lci
