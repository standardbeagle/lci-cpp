#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    std::string reason;              ///< "unknown-verb" | "obscure-token"
    std::vector<std::string> suggested;  ///< common synonyms, if the term maps
};

/// For one standard concept (synonym group), the member terms that actually
/// appear as symbol verbs in this codebase, with counts. Tells an agent which
/// word THIS repo uses for a standard operation (search "explode", not "split").
struct AliasUsage {
    std::string canonical;  ///< representative term of the group
    std::vector<std::pair<std::string, int>> terms;  ///< member -> count
};

struct NamingReport {
    std::vector<VocabularyOutlier> outliers;
    std::vector<AliasUsage> aliases_in_use;
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

    NamingReport analyze(const std::vector<FileSymbolData>& files,
                         const SynonymTable& synonyms,
                         std::string_view project_root) const;

    /// True if `word` (lowercased) is a common programming/English word that
    /// should never be treated as obscure jargon.
    static bool is_common_word(std::string_view word);
};

}  // namespace lci
