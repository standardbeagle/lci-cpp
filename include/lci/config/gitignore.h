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
    bool absolute{};
    PatternType type{};
    std::string prefix;
    std::string suffix;
};

/// Parses .gitignore files and matches paths against their patterns.
/// Supports negation (!), directory-only (/), and wildcard (* / **) patterns.
class GitignoreParser {
  public:
    GitignoreParser() = default;

    /// Loads patterns from a .gitignore file in the given directory.
    /// Returns false on read error (missing file is not an error).
    bool load_gitignore(const std::string& root_path);

    /// Adds a single pattern line (for programmatic use and testing).
    void add_pattern(std::string_view line);

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

    GitignorePattern parse_pattern(std::string_view line) const;
    PatternType analyze_pattern(std::string_view pattern,
                                std::string& prefix_out,
                                std::string& suffix_out) const;
    bool matches_pattern(const GitignorePattern& pat,
                         std::string_view path, bool is_dir) const;
    bool fast_match(const GitignorePattern& pat,
                    std::string_view path) const;
    bool match_glob(std::string_view pattern,
                    std::string_view text) const;
};

}  // namespace lci
