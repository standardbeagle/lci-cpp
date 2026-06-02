#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/error.h>

namespace lci {

/// A single synonym configuration operation parsed from a `.lci.kdl`
/// `synonyms` block (see config.cpp). Applied in document order by
/// SynonymTable::build_from_ops.
struct SynonymOp {
    enum class Kind { Group, Clear, ClearAll };
    Kind kind{Kind::Group};
    /// Group: the member words. Clear: the single word whose group to drop.
    /// ClearAll: empty.
    std::vector<std::string> words;
};

/// Immutable bidirectional synonym groups. Built once at config load, then
/// shared read-only by const reference (no read-path lock — Karpathy rule 3).
///
/// Groups are DISJOINT equivalence classes: every word belongs to at most one
/// group. All words are stored lowercased. Lookups are heterogeneous
/// (string_view) so the read path constructs no temporary std::string — callers
/// must pass an already-lowercased word (Karpathy rule 2).
class SynonymTable {
  public:
    SynonymTable() = default;

    /// Built-in curated dev-verb groups (design §2). Conflict-free by
    /// construction.
    static SynonymTable build_default();

    /// Applies KDL operations to the built-in baseline (or to empty when a
    /// leading clear-all op is present) and returns the frozen result.
    /// Returns an lci::Error on validation failure (fail-fast, Karpathy
    /// rule 6).
    static Result<SynonymTable> build_from_ops(std::span<const SynonymOp> ops);

    /// Members of `word`'s group, EXCLUDING `word` itself. Empty span if
    /// `word` is in no group. `word` must already be lowercased. No
    /// allocation: span into owning storage.
    std::span<const std::string> synonyms_of(std::string_view word) const;

    /// True if `a` and `b` are distinct words in the same group. Both must
    /// already be lowercased.
    bool in_same_group(std::string_view a, std::string_view b) const;

    /// The representative (first-listed) term of `word`'s group — the most
    /// recognizable spelling of the concept (e.g. primary_of("explode") ==
    /// "split"). Empty if `word` is in no group. `word` must be lowercased.
    std::string_view primary_of(std::string_view word) const;

    bool empty() const { return groups_.empty(); }
    std::size_t group_count() const { return groups_.size(); }

  private:
    /// Rebuilds word_to_group_ and others_ from groups_. Call after every
    /// mutation of groups_ during build; never on the read path.
    void reindex();

    std::vector<std::vector<std::string>> groups_;  ///< owning, lowercased
    absl::flat_hash_map<std::string, uint32_t> word_to_group_;  ///< word -> group index
    /// word -> its group members minus itself. Owning storage backing the
    /// no-alloc span returned by synonyms_of. Group sizes are tiny (~3-5) so
    /// the per-word duplication is negligible and bought once at build.
    absl::flat_hash_map<std::string, std::vector<std::string>> others_;
};

}  // namespace lci
