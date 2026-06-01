#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/side_effects.h>
#include <lci/types.h>

namespace lci {

/// Configuration for side effect analysis behavior.
struct SideEffectAnalyzerConfig {
    bool trust_annotations{true};
    bool strict_mode{true};
    bool track_field_access{true};
    int max_accesses_per_function{1000};
};

/// Tracks state while analyzing a single function.
struct FunctionAnalysisContext {
    std::string name;
    std::string file;
    int start_line{};
    int end_line{};

    absl::flat_hash_map<std::string, int> parameters;
    std::string receiver_name;
    std::string receiver_type;

    absl::flat_hash_map<std::string, int> local_variables;
    int scope_depth{};
    std::vector<absl::flat_hash_map<std::string, int>> outer_scopes;

    std::vector<FieldAccess> accesses;
    int seq_num{};

    uint32_t side_effects{};

    std::vector<ExternalCallInfo> external_calls;
    std::vector<UnresolvedCallInfo> unresolved_calls;
    std::vector<ThrowSiteInfo> throw_sites;

    int defer_count{};
    int try_finally_count{};
    bool returns_error{};

    std::vector<std::string> impurity_reasons;
};

/// Two-phase conservative side-effect analyzer.
///
/// Phase 1: Per-function internal analysis (this class).
/// Phase 2: Transitive resolution via call graph (SideEffectPropagator).
///
/// Design: if we say it is pure, it IS pure.
class SideEffectAnalyzer {
  public:
    explicit SideEffectAnalyzer(std::string_view language,
                                const SideEffectAnalyzerConfig& config = {});

    // -- Function lifecycle ---------------------------------------------------

    void begin_function(std::string_view name, std::string_view file,
                        int start_line, int end_line);
    SideEffectInfo end_function();

    // -- Registration ---------------------------------------------------------

    void add_parameter(std::string_view name, int index);
    void set_receiver(std::string_view name, std::string_view receiver_type);
    void add_local_variable(std::string_view name, int line);
    void enter_scope();
    void exit_scope();

    // -- Recording effects ----------------------------------------------------

    void record_access(std::string_view identifier,
                       const std::vector<std::string>& field_path,
                       AccessType access_type, int line, int column);

    void record_function_call(std::string_view func_name,
                              std::string_view qualifier, bool is_method,
                              int line, int column);

    void record_dynamic_call(std::string_view description, int line, int column);
    void record_throw(std::string_view throw_type, int line, int column);
    void record_defer();
    void record_try_finally();
    void record_error_return();
    void record_channel_op(int line);

    // -- Results --------------------------------------------------------------

    const absl::flat_hash_map<std::string, SideEffectInfo>& results() const {
        return results_;
    }
    const SideEffectInfo* get_result(std::string_view file, int line) const;

    /// Walks every function/method symbol in the indexer and populates
    /// results_ with a conservative purity classification derived from
    /// callee-name heuristics. Functions whose outgoing refs target only
    /// internal symbols stay Pure; those that call a known I/O / network /
    /// database / throw / dynamic-eval symbol get marked accordingly.
    ///
    /// This is the C++ counterpart to Go's
    /// SideEffectAnalyzer.AnalyzeAll(symbolIndex). Replaces the per-file
    /// AST-walk path until the indexing pipeline pumps record_access /
    /// record_function_call live (tracked under sibling Dart tasks).
    void populate_from_index(const class MasterIndex& indexer);

    /// Phase 2: propagates side effects transitively upstream through the call
    /// graph. A function that (transitively) calls an impure function becomes
    /// impure even when its own local analysis showed no effects. Runs a
    /// fixpoint over caller edges (bounded iterations for cycle safety) and
    /// recomputes is_pure / purity_score from the combined local+transitive
    /// categories. Call after populate_from_index. C++ counterpart to Go's
    /// SideEffectPropagator.Propagate (internal/core/side_effect_propagation.go).
    void propagate_transitive(const class MasterIndex& indexer);

    /// Direct write to results_ — used by populate_from_index above and
    /// future callers that build SideEffectInfo outside the
    /// begin_function/end_function lifecycle.
    void add_result(std::string key, SideEffectInfo info);

  private:
    AccessTarget classify_target(std::string_view identifier) const;
    std::string build_target_string(std::string_view identifier,
                                    const std::vector<std::string>& field_path,
                                    AccessTarget target_type) const;
    void record_write_side_effect(AccessTarget target_type,
                                  std::string_view identifier, int line);

    AccessPattern analyze_access_pattern(
        const std::vector<FieldAccess>& accesses) const;
    TargetAccessPattern analyze_target_accesses(
        std::string_view target,
        std::vector<FieldAccess>& accesses) const;

    void populate_purity_classification(
        const FunctionAnalysisContext& ctx, SideEffectInfo& info,
        const absl::flat_hash_map<int, bool>& param_index_set) const;
    PurityConfidence determine_confidence(
        const FunctionAnalysisContext& ctx,
        const SideEffectInfo& info) const;
    void compute_purity_score(SideEffectInfo& info) const;

    std::string language_;
    FunctionAnalysisContext* current_func_{};
    FunctionAnalysisContext current_func_storage_;
    absl::flat_hash_map<std::string, SideEffectInfo> results_;
    SideEffectAnalyzerConfig config_;
};

/// Classifies an access sequence string (e.g. "RRWWRR") into a pattern type.
AccessPatternType classify_access_sequence(std::string_view seq);

/// Computes PurityLevel from combined side effect categories.
PurityLevel compute_purity_level(uint32_t categories, bool has_unresolved_calls);

}  // namespace lci
