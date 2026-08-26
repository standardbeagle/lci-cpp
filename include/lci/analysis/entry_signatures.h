#pragma once

#include <string>
#include <vector>

namespace lci {

struct InsightConfig;
class SemanticAnnotator;

namespace analysis {

/// Resolved entry-point pins for code_insight's ENTRY POINTS section, in
/// priority order (first match wins):
///   1. `.lci.kdl` insight { entry_points ... }        -> "annotated"
///   2. symbols labeled @lci:entry                     -> "annotated"
///   3. framework registry match on the project's own identity or
///      dependencies (go.mod / composer.json / package.json)
///                                                     -> "framework"
///   4. nothing                                        -> "heuristic"
/// Pinned symbols are seated first in the section; heuristic output carries
/// an explicit hint asking the author to annotate — a labeled guess, never a
/// confident one.
struct EntryPointHints {
    std::vector<std::string> pins;  // symbol names, deduplicated
    std::string confidence{"heuristic"};
};

EntryPointHints resolve_entry_hints(const InsightConfig& insight,
                                    const std::string& project_root,
                                    const SemanticAnnotator* annotator);

}  // namespace analysis
}  // namespace lci
