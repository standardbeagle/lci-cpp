#pragma once

#include <string>
#include <vector>

#include <lci/indexing/master_index.h>

namespace lci {

/// One duplicated function instance.
struct CloneMember {
    std::string path;  ///< repo-relative
    int line{};
    int end_line{};
    std::string name;
};

/// A set of functions sharing the same (or nearly the same) body.
struct CloneClass {
    std::vector<CloneMember> members;  ///< sorted by (path, line)
    int lines{};             ///< normalized body lines per instance
    int duplicated_lines{};  ///< lines * (members - 1)
    bool exact{};            ///< exact (normalized-identical) vs structural
    double similarity{};     ///< 1.0 exact; min pairwise Jaccard otherwise
};

struct CloneReport {
    int functions_scanned{};
    int clone_classes{};
    int duplicated_lines{};   ///< sum over classes
    int function_lines{};     ///< normalized lines across scanned functions
    double duplication_pct{}; ///< duplicated_lines / function_lines
    std::vector<CloneClass> classes;  ///< ranked duplicated_lines desc
};

/// Corpus-wide duplicate-code detection over the indexed functions.
///
/// Exact clones: functions whose comment/whitespace-normalized bodies are
/// identical, grouped by content hash (one pass, whole corpus). Structural
/// clones: near-identical token sets (Jaccard >= threshold) among the
/// largest remaining functions — capped so the pairwise stage stays
/// bounded on 500k+ symbol corpora; the cap is reported, never silent.
/// The change-scoped variant of these detectors lives in git::Analyzer
/// (git_analyze focus=duplicates); this is the repo-wide report.
class CloneDetector {
  public:
    struct Options {
        int min_lines = 6;              ///< normalized lines to qualify
        double structural_threshold = 0.90;
        int structural_top_n = 1500;    ///< largest functions entering the
                                        ///< pairwise structural stage
        int max_classes = 50;           ///< classes kept in the report
    };

    /// `allowed_attrs` indexes PathAttrId -> analyzable (the analysis
    /// scope gate, same as ErrorHandlingAnalyzer); empty = allow all.
    CloneReport analyze(MasterIndex& index, std::string_view project_root,
                        const std::vector<bool>& allowed_attrs,
                        const Options& opts) const;
};

}  // namespace lci
