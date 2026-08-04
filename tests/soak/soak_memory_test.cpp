// Memory soak test: drives the full in-process MCP engine through repeated
// index / mutate / reindex / query cycles and asserts RSS reaches a plateau.
//
// Rationale: LCI ships as a long-running server, and end-of-process leak
// checkers (LSan) cannot see the failure mode that matters most for that
// shape — memory that grows without bound but stays *reachable* (caches that
// never evict, RCU snapshots that never retire, per-request accumulation).
// This test measures the process's own RSS across cycles instead.
//
// Protocol: file contents rotate through a fixed set of variants, so after a
// warmup phase every cycle repeats previously-seen work and all legitimate
// caches (trigram postings, symbol interning, tree-sitter state) are
// saturated. Any steady per-cycle RSS growth after warmup is unbounded
// growth by construction. The threshold is absolute slack, not wall-clock,
// so the test is robust to scheduler contention under ctest -j (see
// .claude/rules on contention-robust tests); allocator noise (ASan
// quarantine, arena growth) fits comfortably inside the slack while a real
// per-cycle leak of ~100 KB crosses it well before the last cycle.

#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/config.h>
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

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Reads the process's resident set size in kilobytes from /proc/self/status.
// Linux-only; the test skips elsewhere.
long read_rss_kb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    long rss_kb = -1;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::sscanf(line, "VmRSS: %ld kB", &rss_kb) == 1) break;
    }
    std::fclose(f);
    return rss_kb;
}

// The same engine assembly the production stdio server and the MCP fuzz
// target use: config owned here and declared first because SearchEngine
// stores a reference into cfg.synonyms.
struct Harness {
    lci::Config cfg;
    lci::MasterIndex index;
    lci::SearchEngine search_engine;
    lci::SemanticAnnotator annotator;
    lci::GraphPropagator propagator;
    lci::SideEffectAnalyzer side_effects;
    lci::CodebaseIntelligenceEngine ci_engine;
    lci::mcp::McpServer server;

    Harness()
        : cfg(lci::make_default_config()),
          index(cfg),
          search_engine(index, cfg.synonyms),
          propagator(&index.ref_tracker()),
          side_effects("generic"),
          server(cfg, index, &search_engine) {
        lci::mcp::register_core_handlers(server, &index, &search_engine,
                                         &side_effects);
        lci::mcp::register_explore_handlers(server, &index);
        lci::mcp::register_index_handlers(server, &index);
        lci::mcp::register_analysis_handlers(server, &index, &annotator,
                                             &side_effects, &propagator,
                                             &ci_engine);
        lci::mcp::register_context_handlers(server, &index);
    }

    // Feeds one newline-delimited JSON-RPC session through the server and
    // returns the response stream, exactly as the stdio transport would.
    std::string run_session(const std::string& input) {
        std::istringstream in(input);
        std::ostringstream sink;
        auto* old_cin = std::cin.rdbuf(in.rdbuf());
        auto* old_cout = std::cout.rdbuf(sink.rdbuf());
        server.run();
        std::cin.rdbuf(old_cin);
        std::cout.rdbuf(old_cout);
        return sink.str();
    }
};

// Rotating content variants per file. Each variant is real, parseable code in
// its language so tree-sitter extraction, the reference tracker, and the
// trigram index all do full work on every update; rotation means cycle N and
// cycle N+kVariants index byte-identical content.
constexpr int kVariants = 4;

std::string go_variant(int v) {
    std::string body = "package server\n\n"
                       "type Router struct{ routes int }\n\n";
    for (int i = 0; i <= v; ++i) {
        body += "func Handle" + std::to_string(i) +
                "(r *Router, path string) int {\n"
                "    return r.routes + len(path) + " + std::to_string(i) +
                "\n}\n\n";
    }
    body += "func main() {\n    r := &Router{}\n    Handle0(r, \"/\")\n}\n";
    return body;
}

std::string py_variant(int v) {
    std::string body;
    for (int i = 0; i <= v; ++i) {
        body += "def normalize_" + std::to_string(i) + "(s):\n"
                "    return s.strip().lower()\n\n";
    }
    body += "def tokenize(text):\n"
            "    return [normalize_0(w) for w in text.split()]\n";
    return body;
}

std::string ts_variant(int v) {
    std::string body = "export class App {\n  private count = 0;\n";
    for (int i = 0; i <= v; ++i) {
        body += "  handle" + std::to_string(i) +
                "(req: string): number {\n"
                "    return this.count + req.length + " + std::to_string(i) +
                ";\n  }\n";
    }
    body += "}\n\nexport function boot(): App { return new App(); }\n";
    return body;
}

// Corpus files live in a real temp directory: MasterIndex::update_file
// verifies the path exists on disk before indexing the provided content.
struct Corpus {
    std::filesystem::path dir;

    Corpus() {
        dir = std::filesystem::temp_directory_path() /
              ("lci-soak-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
    }
    ~Corpus() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // Writes content to disk and returns the absolute path as a string.
    std::string place(const char* rel, const std::string& content) const {
        auto path = dir / rel;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << content;
        return path.string();
    }
};

// One soak cycle: mutate every file (forcing extraction + index rebuild for
// those files), drop and re-add one file (exercising the removal path), then
// run a query batch across the read-path tools. Returns the session output so
// the test can prove the cycle did real work, not vacuously succeed.
std::string run_cycle(Harness& h, const Corpus& corpus, int cycle) {
    const int v = cycle % kVariants;
    const std::string go = corpus.place("router.go", go_variant(v));
    const std::string py = corpus.place("util/strings.py", py_variant(v));
    const std::string ts = corpus.place("web/app.ts", ts_variant(v));
    EXPECT_TRUE(h.index.update_file(go, go_variant(v)));
    EXPECT_TRUE(h.index.update_file(py, py_variant(v)));
    EXPECT_TRUE(h.index.update_file(ts, ts_variant(v)));
    h.index.remove_file(ts);
    EXPECT_TRUE(h.index.update_file(ts, ts_variant(v)));

    return h.run_session(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"search","arguments":{"pattern":"Router"}}})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"find_files","arguments":{"pattern":"app"}}})"
        "\n"
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_symbols","arguments":{"file":"router.go"}}})"
        "\n"
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"inspect_symbol","arguments":{"name":"Router"}}})"
        "\n"
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"browse_file","arguments":{"file":"web/app.ts"}}})"
        "\n"
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"get_context","arguments":{"name":"Handle0","mode":"full"}}})"
        "\n");
}

TEST(MemorySoak, RssPlateausAcrossReindexQueryCycles) {
    if (read_rss_kb() < 0) {
        GTEST_SKIP() << "/proc/self/status unavailable; RSS soak is Linux-only";
    }

    // Cycle counts trade runtime for sensitivity: warmup must rotate through
    // every variant a few times so caches saturate; the measured phase must be
    // long enough that a steady per-cycle leak dwarfs allocator noise.
    constexpr int kWarmupCycles = 24;
    constexpr int kMeasuredCycles = 96;
    constexpr long kSlackKb = 12 * 1024;  // 12 MB absolute growth budget

    Harness h;
    Corpus corpus;

    // Discrimination guard: the first cycle must demonstrably exercise the
    // engine — the search must find the seeded symbol and no call may error.
    // Without this, a silently no-op cycle would make the plateau vacuous.
    const std::string first = run_cycle(h, corpus, 0);
    ASSERT_NE(first.find("Router"), std::string::npos)
        << "search returned no hit — soak cycles are not doing real work: "
        << first;
    ASSERT_EQ(first.find("\"error\""), std::string::npos)
        << "a tool call in the soak batch failed: " << first;

    for (int c = 1; c < kWarmupCycles; ++c) run_cycle(h, corpus, c);

    const long baseline_kb = read_rss_kb();
    ASSERT_GT(baseline_kb, 0);

    std::vector<long> samples;
    samples.reserve(kMeasuredCycles / 8);
    for (int c = 0; c < kMeasuredCycles; ++c) {
        run_cycle(h, corpus, kWarmupCycles + c);
        if ((c + 1) % 8 == 0) samples.push_back(read_rss_kb());
    }
    const long final_kb = samples.back();

    // Attach the whole trajectory so a failure is debuggable from the log.
    std::ostringstream trajectory;
    trajectory << "post-warmup RSS " << baseline_kb << " kB; samples (kB):";
    for (long s : samples) trajectory << ' ' << s;

    EXPECT_LE(final_kb, baseline_kb + kSlackKb)
        << "RSS grew " << (final_kb - baseline_kb) << " kB over "
        << kMeasuredCycles << " reindex+query cycles (budget " << kSlackKb
        << " kB) — unbounded reachable growth. " << trajectory.str();
}

}  // namespace
