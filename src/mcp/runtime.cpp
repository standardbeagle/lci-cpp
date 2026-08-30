#include <lci/mcp/runtime.h>

#include <iostream>
#include <string>

#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/handlers_context.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/handlers_explore.h>
#include <lci/mcp/handlers_index.h>
#include <lci/mcp/server.h>

namespace lci {
namespace mcp {

void McpRuntime::warmup(MasterIndex& index) {
    // Walk the live index and extract every file's @lci: annotations into
    // the annotator. Without this, the semantic_annotations tool only sees
    // labels seeded externally — which on a typical corpus means zero
    // direct annotations even when files do contain @lci: markers. Has to
    // run before GraphPropagator seeding so the propagator can pick up
    // direct labels as propagation roots.
    annotator.populate_from_index(index);
    {
        std::string manifest_error;
        annotator.load_project_manifest(index, &manifest_error);
        if (!manifest_error.empty()) {
            std::cerr << "Warning: " << manifest_error << "\n";
        }
    }

    // Phase 1a (AST pass) happens DURING indexing now: MasterIndex's
    // side-effect sink drives per-worker analyzers inside the extraction
    // the index performs anyway, and their records land in `side_effects`
    // before this warmup runs. The old serial whole-corpus re-parse here
    // was 31% of total CPU on one thread (~85s wall on the dotnet corpus).
    // Callers must set_side_effect_sink(&runtime.side_effects) BEFORE
    // index_directory or the AST-fact records are simply absent (the
    // heuristic pass below still fills every function, at lower fidelity).

    // Phase 1b: callee-name heuristic. Augments the AST records with
    // IO / network / database / throw categories inferred from outgoing
    // callee names (which a bare call node in the AST can't classify) and
    // fills in functions the AST walk didn't record, so summary mode can
    // report the pure / impure split and every query mode has records to
    // serve.
    side_effects.populate_from_index(index);

    // Phase 2: propagate impurity transitively upstream through the call
    // graph so a function that (indirectly) reaches an impure callee is
    // itself marked impure (populates transitive_categories; recomputes
    // is_pure).
    side_effects.propagate_transitive(index);

    // Seed GraphPropagator with the impure functions so transitive
    // purity propagates: any caller of an impure function is itself
    // impure unless its own purity overrides. Decay mode keeps strength
    // bounded so deep call chains don't blow up.
    auto rt_snap = index.ref_tracker().pin();
    for (const auto& [key, info] : side_effects.results()) {
        if (!info.is_pure) {
            for (const auto& es :
                 rt_snap->find_symbols_by_name(info.function_name)) {
                if (es &&
                    static_cast<int>(es->symbol.line) == info.start_line) {
                    propagator.seed_label(es->id, "impure", 1.0);
                }
            }
        }
    }
    // Seed propagator with direct @lci: labels from the annotator so the
    // propagator computes transitive labels across the call graph. Without
    // this seeding, only impurity labels propagate. Strength 1.0 = explicit
    // annotation (vs propagated values which decay per hop).
    {
        auto ann_rt_snap = index.ref_tracker().pin();
        for (FileID fid : index.get_all_file_ids()) {
            for (const auto& es : ann_rt_snap->get_file_enhanced_symbols(fid)) {
                if (!es) continue;
                const auto* ann = annotator.get_annotation(fid, es->id);
                if (!ann) continue;
                for (const auto& lbl : ann->labels) {
                    propagator.seed_label(es->id, lbl, 1.0);
                }
            }
        }
    }
    propagator.propagate();
}

void register_all_handlers(McpServer& server, MasterIndex* index,
                           SearchEngine* search_engine, McpRuntime* runtime) {
    register_core_handlers(server, index, search_engine,
                           &runtime->side_effects);
    register_explore_handlers(server, index);
    register_index_handlers(server, index);
    register_analysis_handlers(server, index, &runtime->annotator,
                               &runtime->side_effects, &runtime->propagator,
                               &runtime->ci_engine);
    register_context_handlers(server, index);
}

}  // namespace mcp
}  // namespace lci
