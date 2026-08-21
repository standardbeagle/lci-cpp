#pragma once

#include <string_view>
#include <vector>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/side_effects.h>

namespace lci {

class MasterIndex;
class SideEffectAnalyzer;

/// Error-handling + resource-management scoring and rollup
/// (docs/plans/2026-08-17-error-handling-score-design.md).
///
/// Consumes the per-function SideEffectAnalyzer records (findings were
/// detected during the AST warmup pass), weights them by severity ×
/// confidence × normalized fan-in from the real call graph, and rolls
/// function → module → repo. Production-only: test/vendored/example paths
/// never score (a test's empty catch is fine). All output deterministically
/// sorted (severity desc, file, line).
class ErrorHandlingAnalyzer {
  public:
    struct Result {
        ErrorHandlingSummary errors;
        ResourceSummary resources;
    };

    /// `allowed_attrs` is indexed by PathAttrId: true means files with that
    /// attribute are scored. Empty means the default — every attribute that
    /// activates the Analysis capability, i.e. shipping code.
    static Result analyze(const SideEffectAnalyzer& analyzer,
                          const MasterIndex& indexer,
                          std::string_view project_root,
                          const std::vector<bool>& allowed_attrs = {});

    /// Per-finding score deduction: severity base × confidence ×
    /// (0.5 + 0.5 × normalized fan-in). Exposed for unit tests.
    static double finding_deduction(FindingSeverity severity, double confidence,
                                    double norm_fanin);

    /// Per-function score: 1.0 minus the summed deductions, floored at 0.
    static double function_score(const std::vector<EhFinding>& findings,
                                 double norm_fanin);
};

}  // namespace lci
