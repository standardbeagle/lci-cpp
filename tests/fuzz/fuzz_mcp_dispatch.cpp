#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/side_effect_analyzer.h>
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

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

#include "fuzz_fixture.h"

// libFuzzer target: the full MCP stdio boundary — the external, untrusted-input
// surface. Arbitrary bytes are fed as the server's stdin (newline-delimited
// JSON-RPC framing), driving parse -> method dispatch -> tools/call ->
// per-tool argument validation (the unknown-parameter guard) -> real handler
// against a live in-process index. This is the broadest target: a single valid
// tools/call line reaches any of the 14 registered handlers with fuzzed
// arguments.
//
// The server and its backing index/analyzers are built once and reused; each
// iteration only swaps std::cin/std::cout buffers, so the fuzzer isn't paying
// tree-sitter indexing costs per input.

namespace {

// Holds the server plus everything it borrows by pointer/reference. Assembled
// once and kept alive for the process lifetime (the handlers capture these
// addresses). `cfg` is owned here and declared first: SearchEngine stores a
// `const SynonymTable&` into cfg.synonyms, so the config must outlive the
// engine — mirrors cli/mcp.cpp keeping cfg as a long-lived local.
struct Harness {
    lci::Config cfg;
    lci::MasterIndex index;
    lci::SearchEngine search_engine;
    lci::SemanticAnnotator annotator;
    lci::GraphPropagator propagator;
    lci::SideEffectAnalyzer side_effects;
    lci::CodebaseIntelligenceEngine ci_engine;
    lci::mcp::McpServer server;

    explicit Harness(lci::Config c)
        : cfg(std::move(c)),
          index(cfg),
          search_engine(index, cfg.synonyms),
          propagator(&index.ref_tracker()),
          side_effects("generic"),
          server(cfg, index, &search_engine) {
        lci::fuzz::seed_fuzz_corpus(index);
        annotator.populate_from_index(index);
        side_effects.populate_from_index(index);

        lci::mcp::register_core_handlers(server, &index, &search_engine,
                                         &side_effects);
        lci::mcp::register_explore_handlers(server, &index);
        lci::mcp::register_index_handlers(server, &index);
        lci::mcp::register_analysis_handlers(server, &index, &annotator,
                                             &side_effects, &propagator,
                                             &ci_engine);
        lci::mcp::register_context_handlers(server, &index);
    }
};

Harness& harness() {
    static Harness* h = new Harness(lci::fuzz::make_fuzz_config());
    return *h;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 16384) {
        return 0;
    }

    // Feed the raw fuzz bytes as stdin; the run loop splits on newlines and
    // parses each line as one JSON-RPC message. Swallow stdout into a sink so
    // the fuzzer doesn't flood the terminal.
    std::string input(reinterpret_cast<const char*>(data), size);

    std::istringstream in(input);
    std::ostringstream sink;
    auto* old_cin = std::cin.rdbuf(in.rdbuf());
    auto* old_cout = std::cout.rdbuf(sink.rdbuf());

    harness().server.run();

    std::cin.rdbuf(old_cin);
    std::cout.rdbuf(old_cout);
    return 0;
}
