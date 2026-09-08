#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lci {

/// Classification of gitignore pattern for optimized matching.
enum class PatternType : uint8_t {
    Exact = 0,
    Prefix,
    Suffix,
    Wildcard,
};

/// A parsed gitignore pattern with optimization metadata.
struct GitignorePattern {
    std::string pattern;
    bool negate{};
    bool directory{};
    /// Anchored to the .gitignore's directory: set when the pattern had a
    /// leading `/` OR contains an interior `/` (git anchors both; only a
    /// slash-free pattern floats to any depth).
    bool absolute{};
    PatternType type{};
    std::string prefix;
    std::string suffix;
    /// Directory (relative to the match root, no trailing slash) of the
    /// .gitignore that declared this pattern; "" for the root file. The
    /// pattern applies only to paths under this prefix.
    std::string base;
    /// Longest wildcard-free run of the pattern (slashes trimmed; empty if
    /// under 3 chars). Any path the pattern can match — at full length or
    /// any suffix — contains it verbatim, so a find() miss skips both the
    /// glob matcher and the per-suffix retry loop. Wildcard patterns from
    /// a large .gitignore were ~18% of scan CPU on a 55k-file corpus.
    std::string literal;
};

/// Matches `text` against a gitignore-style glob `pattern`:
///   `?`      one non-`/` char
///   `*`      zero or more non-`/` chars
///   `**`     zero or more chars, crossing `/`
///   `[a-z]`  one non-`/` char from a class (`[!...]` / `[^...]` negates)
/// Exposed so other glob consumers (the git churn filter) share one dialect
/// instead of hand-rolling a second, weaker one.
bool glob_match(std::string_view pattern, std::string_view text);

/// Parses .gitignore files and matches paths against their patterns.
/// Supports negation (!), directory-only (/), and wildcard (* / **) patterns.
class GitignoreParser {
  public:
    GitignoreParser() = default;

    /// Loads patterns from .gitignore files: the one in `root_path` plus
    /// every nested .gitignore, each applying to its own subtree (deeper
    /// files override shallower ones, as in git). Ignored directories are
    /// not descended into. Returns false on read error (missing .gitignore
    /// files are not an error).
    bool load_gitignore(const std::string& root_path);

    /// Adds a single pattern line (for programmatic use and testing).
    /// `base` is the .gitignore's directory relative to the match root.
    void add_pattern(std::string_view line, std::string_view base = "");

    /// Returns true if the path should be ignored.
    ///
    /// **Path contract:** `path` must be **relative-to-project-root**.
    /// Internal `/` boundary semantics match gitignore standard:
    ///   - `*`  matches non-`/` only
    ///   - `**` matches across `/`
    ///   - directory patterns (trailing `/`) match the dir and contents
    bool should_ignore(std::string_view path, bool is_dir) const;

    /// Returns gitignore patterns as LCI exclusion glob patterns.
    std::vector<std::string> get_exclusion_patterns() const;

  private:
    std::vector<GitignorePattern> patterns_;

    bool load_dir(const std::string& dir_path, const std::string& base);
    GitignorePattern parse_pattern(std::string_view line,
                                   std::string_view base) const;
    PatternType analyze_pattern(std::string_view pattern,
                                std::string& prefix_out,
                                std::string& suffix_out) const;
    bool matches_pattern(const GitignorePattern& pat,
                         std::string_view path, bool is_dir) const;
    bool match_rel(const GitignorePattern& pat, std::string_view rel) const;
    bool fast_match(const GitignorePattern& pat,
                    std::string_view path) const;
    bool match_glob(std::string_view pattern,
                    std::string_view text) const;
};

}  // namespace lci
