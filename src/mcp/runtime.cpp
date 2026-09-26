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
    //
    // Phases 1b, 2 and the publish run excluded from index writes: the server
    // runs warmup on its own thread once the index is ready, and a /reindex
    // stages its merges into this same analyzer. Unexcluded, the two write
    // one map concurrently and publish() can swap the run's half-merged
    // staging map in as the reader generation (IDX-2 review B2).
    index.run_exclusive_of_index_writes([&] {
        side_effects.populate_from_index(index);

        // Phase 2: propagate impurity transitively upstream through the call
        // graph so a function that (indirectly) reaches an impure callee is
        // itself marked impure (populates transitive_categories; recomputes
        // is_pure).
        side_effects.propagate_transitive(index);

        // Publish the augmented generation (AST facts + heuristic + transitive)
        // with one atomic swap. This is the reader-visible commit for the MCP
        // surface; handlers pin this snapshot and never touch the staging map
        // the bulk pipeline wrote into.
        side_effects.publish();
    });

    // Seed GraphPropagator with the impure functions so transitive
    // purity propagates: any caller of an impure function is itself
    // impure unless its own purity overrides. Decay mode keeps strength
    // bounded so deep call chains don't blow up.
    // Finding 11: matching by (name, start_line) misses every anonymous
    // function/closure — SideEffectInfo carries no name for those, so
    // find_symbols_by_name("") never resolves them and they silently never
    // seed the propagator. Match by (file, line) instead: walk each impure
    // result's own file's enhanced symbols and compare start_line directly,
    // which needs no name at all.
    auto rt_snap = index.ref_tracker().pin();
    for (const auto& [key, info] : side_effects.results()) {
        if (info.is_pure) continue;
        FileID fid = index.path_to_id(info.file_path);
        if (fid == FileID{0}) continue;
        for (const auto& es : rt_snap->get_file_enhanced_symbols(fid)) {
            if (es && static_cast<int>(es->symbol.line) == info.start_line) {
                propagator.seed_label(es->id, "impure", 1.0);
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
                const auto* ann = annotator.get_annotation(
                    fid, SemanticAnnotator::annotation_key(
                             fid, es->symbol.line, es->symbol.column));
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
                           &runtime->side_effects, &runtime->propagator,
                           &runtime->annotator);
    register_explore_handlers(server, index);
    register_index_handlers(server, index);
    register_analysis_handlers(server, index, &runtime->annotator,
                               &runtime->side_effects, &runtime->propagator,
                               &runtime->ci_engine);
    register_context_handlers(server, index, &runtime->side_effects);
}

}  // namespace mcp
}  // namespace lci
