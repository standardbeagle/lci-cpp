#pragma once

#include <lci/analysis/codebase_intelligence_types.h>

namespace lci {

/// Manages token budget enforcement for codebase intelligence responses.
///
/// Prevents response overload by estimating token counts and progressively
/// truncating content to fit within budget limits.
///
/// Ported from Go: codebase_intelligence_token_budget.go
class TokenBudgetManager {
  public:
    TokenBudgetManager() = default;

    /// Calculates target token budget based on max_results.
    /// Returns tokens in the 4000-12000 range with 8000 base.
    static int calculate_target_budget(const int* max_results);

    /// Estimates total tokens in a response.
    static int estimate_response_tokens(
        const CodebaseIntelligenceResponse& response);

    /// Estimates tokens in a repository map.
    static int estimate_repository_map_tokens(const RepositoryMap& map);

    /// Estimates tokens in a dependency graph.
    static int estimate_dependency_graph_tokens(const DependencyGraph& graph);

    /// Estimates tokens in a health dashboard.
    static int estimate_health_dashboard_tokens(const HealthDashboard& health);

    /// Estimates tokens in entry points.
    static int estimate_entry_points_tokens(const EntryPointsList& points);

    /// Estimates tokens in semantic vocabulary.
    static int estimate_semantic_vocabulary_tokens(
        const SemanticVocabulary& vocab);

    /// Enforces token budget, returning a truncated copy if needed.
    /// If under budget, returns the response as-is via pointer.
    /// If over budget, modifies the response in-place.
    static void enforce_budget(CodebaseIntelligenceResponse& response,
                               const int* max_results);

    /// Truncates response to fit within target token count.
    static void truncate_to_budget(CodebaseIntelligenceResponse& response,
                                   int target_tokens);
};

}  // namespace lci
