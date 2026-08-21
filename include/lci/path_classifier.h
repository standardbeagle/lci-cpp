#pragma once

// PathClassifier — the single authority for file attribute tagging.
//
// Every file gets exactly one attribute. The attribute SET is data, not a C++
// enum: lci ships a default ruleset (see kBuiltinAttributeRuleset in
// path_classifier.cpp, written in the same KDL a project writes) and a
// `.lci.kdl` extends it, overrides its patterns, or declares attributes lci
// has never heard of:
//
//   attributes {
//       test "src/legacy_tests/"        // extra pattern for a known attribute
//       production "vendor/mycompany/"  // un-tag a builtin match
//       internal-tooling rank=6 {       // an attribute of your own
//           activates "index" "search"
//           dir "scripts" "tools"
//           glob "*.tmpl"
//       }
//   }
//
// An attribute declares what it ACTIVATES — which of the four gates apply to
// its files. That is what "shipping code" means here: not a flag on the
// attribute, but the set of files whose attribute activates the gate a given
// tool reads. code_insight's analysis sections are the Analysis gate, so they
// default to production and to whatever else a project opts in.
//
// Pattern grammar (shared by shipped rules and config):
//   dir "<glob>"    matches a whole path SEGMENT ("vendor", "_*")
//   glob "<glob>"   matches the basename, or the whole relative path when it
//                   contains '/'
//   content "<id>"  named content heuristic: "minified" | "generated-header",
//                   applied only to files no pattern claimed
// Config shorthand (`test "src/legacy/"`) keeps its historical meaning: a
// trailing '/' is a directory prefix or interior segment, otherwise a glob.
//
// Precedence: config patterns first, in declaration order, first match wins.
// Then shipped patterns, lowest `rank` first — which is why a test file under
// vendor/ is vendored (rank 0) rather than a test (rank 2).
//
// Perf contract (karpathy #3): classification runs ONCE per file on the
// indexing write path; the result is stored in the file record as a
// PathAttrId. Read paths do a lock-free O(1) lookup and never re-run globs.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lci {

/// Index into a PathAttrRegistry. Not an enum: the set is open.
using PathAttrId = uint8_t;

/// The fallback attribute — the one a file gets when nothing matches. Always
/// id 0, always named "production" by the shipped ruleset.
inline constexpr PathAttrId kFallbackAttr = 0;

/// The gates an attribute can activate. Each one is a real branch in the
/// code; nothing here is aspirational.
enum class Capability : uint8_t {
    Index = 0,   ///< parsed and stored at all
    Search,      ///< eligible for search/grep results
    Refs,        ///< eligible as a reference-resolution target
    Analysis,    ///< counted by code_insight's analysis sections
};
inline constexpr int kCapabilityCount = 4;

/// Canonical capability name ("index", "search", ...), and its parse.
std::string_view capability_name(Capability cap);
bool parse_capability(std::string_view name, Capability& out);

/// One attribute definition: its identity, its precedence, what it turns on,
/// and the patterns that claim a file for it.
struct AttrDef {
    std::string name;
    int rank{50};
    uint8_t capabilities{};        ///< bitmask, bit == 1 << Capability
    std::vector<std::string> dirs;      ///< segment globs
    std::vector<std::string> globs;     ///< basename or whole-path globs
    std::vector<std::string> contents;  ///< named content heuristics

    bool activates(Capability cap) const {
        return (capabilities & (1u << static_cast<uint8_t>(cap))) != 0;
    }
};

/// One config pattern: attribute name -> pattern. The name is free text; an
/// unknown one declares a new attribute rather than failing, so a project is
/// never limited to the vocabulary this binary shipped with.
struct PathAttrRule {
    std::string attr;
    std::string pattern;
};

/// The attribute set in force: the shipped ruleset plus a project's
/// additions, resolved to stable ids.
class PathAttrRegistry {
  public:
    /// The shipped ruleset alone. Parsed once, shared.
    static const PathAttrRegistry& builtin();

    /// Shipped ruleset extended by a project's `attributes` block. On a
    /// malformed attribute definition the registry falls back to the builtin
    /// set and `error` says why.
    static PathAttrRegistry with_config(const std::vector<AttrDef>& config_attrs,
                                        const std::vector<PathAttrRule>& config_rules,
                                        std::string& error);

    int size() const { return static_cast<int>(defs_.size()); }
    const AttrDef& def(PathAttrId id) const { return defs_[id]; }
    std::string_view name(PathAttrId id) const { return defs_[id].name; }
    bool activates(PathAttrId id, Capability cap) const {
        return defs_[id].activates(cap);
    }
    /// Id for a name, or false when this registry has no such attribute.
    bool find(std::string_view name, PathAttrId& out) const;
    /// Every attribute that activates `cap`, in id order.
    std::vector<PathAttrId> with_capability(Capability cap) const;

    /// Config patterns, in declaration order — checked before shipped ones.
    const std::vector<std::pair<PathAttrId, std::string>>& config_rules() const {
        return config_rules_;
    }
    /// Attribute ids ordered by rank, lowest first (shipped precedence).
    const std::vector<PathAttrId>& by_rank() const { return by_rank_; }

  private:
    friend class PathClassifier;
    std::vector<AttrDef> defs_;
    std::vector<std::pair<PathAttrId, std::string>> config_rules_;
    std::vector<PathAttrId> by_rank_;

    void finalize();
};

/// Parses an `attributes { ... }` KDL block into definitions and shorthand
/// rules. Shared by the shipped ruleset and the config reader so both accept
/// exactly the same syntax.
bool parse_attributes_block(std::string_view kdl_source,
                            std::vector<AttrDef>& defs_out,
                            std::vector<PathAttrRule>& rules_out,
                            std::string& error);

class PathClassifier {
  public:
    /// Classifies against the shipped ruleset alone.
    PathClassifier();
    /// Classifies against `registry`, which must outlive the classifier.
    explicit PathClassifier(const PathAttrRegistry& registry);

    const PathAttrRegistry& registry() const { return *registry_; }

    /// Path-only classification. `rel_path` is repo-relative with '/'
    /// separators and no leading '/'.
    PathAttrId classify(std::string_view rel_path) const;

    /// Path + content classification. Content heuristics only promote a file
    /// nothing else claimed; an explicit config pattern always wins.
    PathAttrId classify(std::string_view rel_path,
                        std::string_view content) const;

  private:
    /// Shared path pass; `from_config` reports whether a config pattern (not
    /// a shipped one) decided the answer, which content heuristics must not
    /// override.
    PathAttrId classify_path(std::string_view rel_path,
                             bool& from_config) const;

    const PathAttrRegistry* registry_;
};

}  // namespace lci
