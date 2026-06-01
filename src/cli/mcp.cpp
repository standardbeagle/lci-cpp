#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/cli/commands.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/semantic_annotator.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/handlers_context.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/handlers_explore.h>
#include <lci/mcp/handlers_index.h>
#include <lci/mcp/server.h>
#include <lci/search/search_engine.h>
#include <lci/server/server.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <string>

namespace lci {
namespace cli {

// FIX-D.1 sweep (Dart FZJ6Iip4we3U): all 8 parity-compat stubs removed —
// find_files, debug_info, list_symbols, inspect_symbol, browse_file,
// git_analysis, side_effects, code_insight. The stubs shadowed real handlers
// via reverse-iteration dispatch in McpServer::handle_tools_call, making real
// handler implementations dead code and inflating tools/list from Go's 14 to
// C++'s 22. Real handlers in handlers_{core,explore,index,analysis,context}.cpp
// now own dispatch. Prior individual removals (iter-5/6/9/14): search,
// get_context, index_stats. The entire register_parity_compat_tools() helper
// and its private stub-only helpers (collect_symbols, basic_symbol_json,
// iso_timestamp_now, etc.) were deleted alongside. See MODULE_MAP.md
// "Decision: tools/list emit-order parity".

int run_mcp(const GlobalFlags& flags) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    MasterIndex runtime_index(cfg);
    if (!runtime_index.index_directory(cfg.project.root)) {
        std::cerr << "Warning: failed to index project root for MCP runtime\n";
    }

    SearchEngine search_engine(runtime_index, cfg.synonyms);
    SemanticAnnotator annotator;
    // Walk the live index and extract every file's @lci: annotations into
    // the annotator. Without this, the semantic_annotations tool only sees
    // labels seeded externally — which on a typical corpus means zero
    // direct annotations even when files do contain @lci: markers. Has to
    // run before GraphPropagator seeding so the propagator can pick up
    // direct labels as propagation roots.
    annotator.populate_from_index(runtime_index);
    GraphPropagator propagator(&runtime_index.ref_tracker());
    SideEffectAnalyzer side_effect_analyzer("generic");
    // Populate per-function purity from the in-process index. Conservative
    // heuristic: outgoing-callee names matched against known I/O / network
    // / database / throw / dynamic-eval prefixes. Replaces the
    // count_callable_symbols_in_index fallback in handlers_analysis.cpp
    // with real per-function classifications so summary mode can report
    // pure_functions / impure_functions split, and symbol / file / pure /
    // impure / category modes have records to query.
    side_effect_analyzer.populate_from_index(runtime_index);

    // Seed GraphPropagator with the impure functions so transitive
    // purity propagates: any caller of an impure function is itself
    // impure unless its own purity overrides. Decay mode keeps strength
    // bounded so deep call chains don't blow up.
    for (const auto& [key, info] : side_effect_analyzer.results()) {
        if (!info.is_pure) {
            // Resolve back to SymbolID via ref_tracker.find_symbol_by_name
            // (file path + line uniquely identifies the symbol since
            // we keyed on file:line:0 above).
            for (const auto* es : runtime_index.ref_tracker()
                                       .find_symbols_by_name(info.function_name)) {
                if (es && static_cast<int>(es->symbol.line) == info.start_line) {
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
        // Get the union of all labels by enumerating known symbol IDs via the
        // ref_tracker. For each AnnotatedSymbol the annotator has, seed the
        // propagator with each of its labels at its symbol_id.
        // Iterate the label index in deterministic order (sort label keys).
        // Cheap pass — annotations are sparse vs symbols.
        for (FileID fid : runtime_index.get_all_file_ids()) {
            for (const auto* es :
                 runtime_index.ref_tracker().get_file_enhanced_symbols(fid)) {
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

    CodebaseIntelligenceEngine ci_engine;

    // Start MCP server with the live in-process index instead of the stub-only
    // registry so parity and stdio users hit the real handlers.
    mcp::McpServer mcp_server(cfg, runtime_index, &search_engine);
    mcp::register_core_handlers(mcp_server, &runtime_index, &search_engine);
    mcp::register_explore_handlers(mcp_server, &runtime_index);
    mcp::register_index_handlers(mcp_server, &runtime_index);
    mcp::register_analysis_handlers(mcp_server, &runtime_index, &annotator,
                                    &side_effect_analyzer, &propagator,
                                    &ci_engine);
    mcp::register_context_handlers(mcp_server, &runtime_index);

    // Start a shared IndexServer so CLI commands can also connect
    IndexServer index_server(cfg);
    std::string socket_path = get_socket_path_for_root(cfg.project.root);
    index_server.set_socket_path(socket_path);

    bool shared_server_started = index_server.start();
    if (!shared_server_started) {
        std::cerr << "Warning: failed to start shared index server; "
                     "CLI commands won't be able to connect\n";
    }

    int exit_code = mcp_server.run();

    if (shared_server_started) {
        index_server.shutdown(std::chrono::milliseconds(5000));
    }

    return exit_code;
}

}  // namespace cli
}  // namespace lci
