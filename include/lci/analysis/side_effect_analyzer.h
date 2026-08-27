#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/analysis/finding_suppressions.h>
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
    bool returns_value{};
    std::vector<int> error_return_lines;

    std::vector<CatchSiteInfo> catch_sites;
    std::vector<EhFinding> error_findings;   // dropped-error etc., pre-classified
    /// State-changing calls in source order — the undo-cost model. See
    /// classify_work_pairing.
    std::vector<WorkOp> work_ops;
    std::vector<ResourceOp> resource_acquires;
    std::vector<ResourceOp> resource_releases;

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

    /// Per-file suppression directives (see finding_suppressions.h). Set by
    /// the extractor before the file's functions are walked; applied to
    /// error and resource findings when each function ends. Reset to empty
    /// for a file without directives.
    void set_suppressions(FindingSuppressions s) { suppressions_ = std::move(s); }

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
    void record_error_return(int line);
    void record_channel_op(int line);

    /// A `return <expr>` site — the function hands a value to its caller
    /// (gates the leak-no-release factory suppression).
    void record_return_value();

    /// One catch/except/rescue site with its syntactic facts; classified into
    /// swallow findings (empty-catch / catch-and-continue / broad-catch /
    /// log-and-swallow / rethrow-no-cause) at end_function.
    void record_catch(const CatchSiteInfo& site);

    /// A finally/ensure block that returns or throws, discarding any
    /// in-flight exception. `verb` is "return" or "throw" for the message.
    void record_finally_hijack(int line, std::string_view verb);

    /// Go dropped-error evidence: `_ = err` / `_ = f()`. Always high
    /// severity — the caller reaches this only for a SOLE discarded result,
    /// which is unambiguously an error being thrown away.
    void record_dropped_error(int line, std::string_view detail);

    /// Classifies a call site against the acquire/release tables and records
    /// a ResourceOp when it matches. `guarded` = inside a defer/errdefer/
    /// finally/ensure/using/with scope. No-op for unclassified callees.
    /// `branch_id` is the enclosing alternative arm (WorkOp::branch_id).
    void record_call_site_resources(std::string_view callee, int line,
                                    bool guarded, uint32_t branch_id = 0,
                                    std::string_view qualifier = {});

    // -- Results --------------------------------------------------------------

    const absl::flat_hash_map<std::string, SideEffectInfo>& results() const {
        return results_;
    }
    const SideEffectInfo* get_result(std::string_view file, int line) const;

    /// Walks every function/method symbol in the indexer and augments results_
    /// with a conservative purity classification derived from callee-name
    /// heuristics. Functions whose outgoing refs target only internal symbols
    /// stay Pure; those that call a known I/O / network / database / throw /
    /// dynamic-eval symbol get marked accordingly.
    ///
    /// Runs AFTER the AST pass (UnifiedExtractor driving begin_function /
    /// record_access / record_throw via set_side_effect_sink): for a function
    /// the AST already recorded, it only OR-s in the heuristic categories the
    /// AST can't see from a bare call node, preserving the AST-derived
    /// param/receiver/global writes and throws. For functions with no AST
    /// record it builds the heuristic-only classification as a fallback.
    ///
    /// C++ counterpart to Go's SideEffectAnalyzer.AnalyzeAll(symbolIndex).
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
    FindingSuppressions suppressions_;
    FunctionAnalysisContext current_func_storage_;
    absl::flat_hash_map<std::string, SideEffectInfo> results_;
    SideEffectAnalyzerConfig config_;
};

/// Classifies an access sequence string (e.g. "RRWWRR") into a pattern type.
AccessPatternType classify_access_sequence(std::string_view seq);

/// Computes PurityLevel from combined side effect categories.
PurityLevel compute_purity_level(uint32_t categories, bool has_unresolved_calls);

/// Resource acquire/release callee classification (prefix tables, extends the
/// classify_callee_category mechanism).
enum class ResourceOpKind : uint8_t { None = 0, Acquire, Release };
ResourceOpKind classify_resource_callee(std::string_view callee);

/// Undo-cost work classification (same prefix-table mechanism).
WorkKind classify_work_callee(std::string_view callee);
void classify_work_pairing(const std::vector<WorkOp>& ops,
                           const std::vector<int>& throw_lines,
                           bool has_catch, uint32_t effects,
                           std::vector<EhFinding>& out);

/// Classifies one catch site's syntactic facts into swallow findings.
/// Appends to `out`, deterministic order. Exposed for unit tests.
/// Classifies one catch site into findings. `fn_name` is the enclosing
/// function's name: a name that promises a sentinel on failure (isValid*,
/// try*) makes a sentinel return the answer rather than a swallow.
void classify_catch_site(const CatchSiteInfo& site, std::string_view fn_name,
                         std::vector<EhFinding>& out);

/// Pairs a function's acquires against its release credits and appends
/// leak-no-release / leak-on-error-path / unguarded-release findings.
/// `throw_lines` = lines of throw/raise/panic sites. `returns_value` marks a
/// function that returns a value: factories/constructors hand the acquired
/// resource to their caller, so leak-no-release is suppressed there
/// (pocketbase DefaultDBConnect class — precision over recall, no dataflow).
/// Exposed for unit tests.
void classify_resource_pairing(const std::vector<ResourceOp>& acquires,
                               const std::vector<ResourceOp>& releases,
                               const std::vector<int>& throw_lines,
                               bool returns_value,
                               std::vector<EhFinding>& out);

}  // namespace lci
