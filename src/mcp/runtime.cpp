#include <lci/mcp/runtime.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/handlers_context.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/handlers_explore.h>
#include <lci/mcp/handlers_index.h>
#include <lci/mcp/server.h>
#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>
#include <lci/parser/unified_extractor.h>

#include <tree_sitter/api.h>

namespace lci {
namespace mcp {

namespace {

// Drives the SideEffectAnalyzer per-function lifecycle from real syntax. Each
// indexed file is re-read and re-parsed once, then walked by UnifiedExtractor
// with the analyzer attached as its side-effect sink so param / receiver /
// global writes, throws, and channel ops are recorded from the AST instead of
// callee-name guessing. Records are keyed by absolute path + start line, the
// same scheme populate_from_index uses, so the heuristic pass augments these
// records rather than colliding with them.
void populate_side_effects_from_ast(MasterIndex& index,
                                    SideEffectAnalyzer& analyzer) {
    for (FileID fid : index.get_all_file_ids()) {
        std::string path = index.get_file_path(fid);
        std::string ext = std::filesystem::path(path).extension().string();
        if (ext.empty()) continue;

        parser::Language lang{};
        if (!parser::language_from_extension(ext, lang)) continue;

        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string content = ss.str();
        if (content.empty()) continue;

        parser::PooledParser parser_guard(lang);
        if (!parser_guard) continue;

        parser::UniqueTree tree(ts_parser_parse_string(
            parser_guard.get(), nullptr, content.data(),
            static_cast<uint32_t>(content.size())));
        if (!tree) continue;

        parser::UnifiedExtractor extractor;
        extractor.init(content, fid, ext, path);
        extractor.set_side_effect_sink(&analyzer);
        extractor.extract(tree.get());
    }
}

}  // namespace

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

    // Phase 1a: AST pass. Re-walk each file's syntax tree and drive the
    // per-function lifecycle so param/receiver/global writes, throws, and
    // channel ops are recorded from real AST facts — effects the
    // callee-name heuristic below cannot see (e.g. `x.field = 1` with no
    // impure callee, or a bare `raise`/`throw` statement).
    populate_side_effects_from_ast(index, side_effects);

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
