#pragma once

// PathClassifier — the single authority for file attribute tagging.
//
// Classifies every indexed file into exactly one attribute:
//   production | test | example | vendored | generated | docs
//
// Built-in per-ecosystem defaults (Go _*/testdata, PHP/JS/TS tests/,
// *.min.js, *_generated.*, ...) are extended/overridden by `.lci.kdl`:
//
//   attributes {
//       test "src/legacy_tests/"
//       vendored "*.iife.js"
//       production "vendor/mycompany/"   // un-tag a builtin match
//   }
//
// Config rules are checked first, in declaration order, first match wins;
// builtins apply only when no config rule matches. Patterns:
//   - trailing '/'  => directory prefix / path-segment match
//   - contains '/'  => glob against the repo-relative path
//   - otherwise     => glob against the basename
//
// Perf contract (karpathy #3): classification runs ONCE per file on the
// indexing write path; the result is stored in the file record. Read paths
// do a lock-free O(1) lookup and never re-run globs.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lci {

enum class PathAttr : uint8_t {
    Production = 0,
    Test,
    Example,
    Vendored,
    Generated,
    Docs,
};

inline constexpr int kPathAttrCount = 6;

/// One configured rule: pattern -> attribute.
struct PathAttrRule {
    PathAttr attr{};
    std::string pattern;
};

class PathClassifier {
  public:
    PathClassifier() = default;
    /// `config_rules` come from `.lci.kdl` and take precedence over builtins.
    explicit PathClassifier(std::vector<PathAttrRule> config_rules);

    /// Path-only classification. `rel_path` is repo-relative with '/'
    /// separators and no leading '/'.
    PathAttr classify(std::string_view rel_path) const;

    /// Path + content classification. Adds the content heuristics:
    ///   - minified single/long-line files  -> Vendored
    ///   - "Code generated"/"DO NOT EDIT"/"@generated" header -> Generated
    /// Content heuristics only promote a Production result; explicit config
    /// rules (including `production`) always win.
    PathAttr classify(std::string_view rel_path,
                      std::string_view content) const;

    /// Lowercase canonical name ("production", "test", ...).
    static std::string_view name(PathAttr attr);

    /// Parses a canonical name into an attribute. Returns false on unknown.
    static bool parse(std::string_view name, PathAttr& out);

  private:
    std::vector<PathAttrRule> config_rules_;
};

}  // namespace lci
