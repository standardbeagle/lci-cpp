#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <functional>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/semantic/synonym_table.h>
#include <lci/symbol.h>

namespace lci {

/// A symbol whose name uses vocabulary an agent is unlikely to search for:
/// either a leading verb / token that is in no known synonym group and not a
/// common word (genuinely unknown jargon), or a corpus-rare obscure token.
struct VocabularyOutlier {
    std::string object_id;
    std::string name;
    std::string location;            ///< basename:line
    int fan_in{};                    ///< incoming references (importance)
    std::string odd_term;            ///< the offending token
    /// "misspelling" | "convention-mismatch" | "unknown-verb" | "obscure-token"
    std::string reason;
    /// Correction (misspelling) or common synonyms, if the term maps.
    std::vector<std::string> suggested;
};

/// For one standard concept (synonym group), the member terms that actually
/// appear as symbol verbs in this codebase, with counts. Tells an agent which
/// word THIS repo uses for a standard operation (search "explode", not "split").
struct AliasUsage {
    std::string canonical;  ///< representative term of the group
    std::vector<std::pair<std::string, int>> terms;  ///< member -> count
};

/// A name defined at many distinct sites: a search on it returns them all,
/// so the name no longer identifies anything (findability defect).
struct AmbiguousName {
    std::string name;
    int definition_count{};
};

/// A name whose tokens fail to narrow the corpus: searching every token of
/// the name still leaves ~`expected_matches` candidate symbols.
struct VagueName {
    std::string name;
    double bits{};             ///< total information: sum of -log2(selectivity)
    double expected_matches{}; ///< corpus_size * product(token selectivity)
    int definitions{};         ///< how many symbols carry exactly this name
};

/// Corpus-relative name information. Each token's selectivity is the
/// fraction of function-like symbols whose name contains it; its information
/// is -log2(selectivity) bits. Token selectivities MULTIPLY (bits add): a
/// name is the intersection of its tokens' candidate sets. A name is vague
/// when the whole name still leaves an expected result set >=
/// ci_thresholds::kAmbiguousNameDefs — the same bar ambiguous_names uses for
/// exact-name collisions, generalized to token combinations. No English
/// judgment is involved: "process" is vague only in a repo where many
/// symbols contain "process".
///
/// The obscurity axis is separate and dictionary-based: tokens that are not
/// English-like, not common programming words, not synonyms, and not
/// corpus-frequent. Obscure tokens are highly selective but unguessable —
/// the opposite failure from vagueness.
struct NameInformation {
    double median_bits{};      ///< median name information across the corpus
    int total_symbols{};
    std::vector<VagueName> vague_names;  ///< lowest-information names, capped
    // Obscurity (dictionary legibility) axis:
    int nonword_tokens{};
    int total_tokens{};
    std::vector<std::pair<std::string, int>> top_nonwords;
};

/// A variable whose name shares no vocabulary with its initializer: the
/// name says nothing about what the value is ("tmp = load_config()").
struct FidelityMismatch {
    std::string var_name;
    std::string source_name;  ///< initializer callee
    std::string location;     ///< path:line
    int use_count{};
};

struct NameFidelity {
    int checked{};     ///< variables with a recognizable initializer call
    int mismatched{};  ///< total mismatches found (list below is capped)
    std::vector<FidelityMismatch> mismatches;
};

/// One spelling of a split concept: a distinct raw name (representative
/// site) whose canonicalized tokens equal the group's.
struct SynonymSplitMember {
    std::string name;      ///< raw spelling (highest fan-in site)
    std::string location;  ///< basename:line of that site
    int fan_in{};          ///< summed incoming refs across sites spelled this way
};

/// Two or more symbols naming the same concept with different words: their
/// names are identical after mapping every token to its synonym-group
/// primary, but the token sequences differ (fetchUser vs loadUser). A
/// search for one spelling misses the others — the "same word for the same
/// thing" rule, checked corpus-wide.
struct SynonymSplit {
    std::string canonical;  ///< primary-token form, tokens joined by '_'
    std::vector<SynonymSplitMember> members;  ///< distinct spellings, by fan-in
    int total_fan_in{};
};

struct NamingReport {
    std::vector<VocabularyOutlier> outliers;
    std::vector<AliasUsage> aliases_in_use;
    std::vector<AmbiguousName> ambiguous_names;
    NameInformation information;
    NameFidelity fidelity;
    std::vector<SynonymSplit> synonym_splits;
};

/// Detects low-discoverability naming to cut wasted semantic searches.
///
/// Outliers combine two signals: (1) a leading verb that is in no SynonymTable
/// group AND not a common word, and (2) a token that is corpus-rare and not in
/// the standard vocabulary. Outliers are ranked by fan-in (incoming refs) so
/// only important, hard-to-find symbols surface. `aliases_in_use` reports,
/// per synonym group present in the codebase, which member terms are used.
class NamingAnalyzer {
  public:
    NamingAnalyzer() = default;

    /// `content_of`, when provided, enables the name-vs-source fidelity
    /// pass: it must return the indexed content for a file id ("" when
    /// unavailable). Without it the fidelity report stays empty.
    NamingReport analyze(
        const std::vector<FileSymbolData>& files, const SynonymTable& synonyms,
        std::string_view project_root,
        const std::function<std::string_view(FileID)>& content_of = {}) const;

    /// Parses the initializer callee out of one declaration line, starting
    /// the scan after `name_col` (1-based column of the variable name).
    /// Recognizes `= callee(`, `:= pkg.Callee(`, `= obj.method(`,
    /// `= make<T>(`; returns "" when the RHS is not a call. Exposed for
    /// tests.
    static std::string_view initializer_callee(std::string_view line,
                                               int name_col);

    /// True if `word` (lowercased) is a common programming/English word that
    /// should never be treated as obscure jargon.
    static bool is_common_word(std::string_view word);
};

}  // namespace lci
