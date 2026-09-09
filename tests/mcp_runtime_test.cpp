#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <lci/config.h>
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/runtime.h>
#include <lci/types.h>

#include "unique_temp.h"

namespace lci {
namespace mcp {
namespace {

// Finding 11: McpRuntime::warmup seeded the propagator by matching
// SideEffectInfo's (function_name, start_line) against
// find_symbols_by_name(name) — a match that ignores WHICH FILE the symbol
// lives in and breaks down entirely when the name is empty (an anonymous
// function/closure never resolves at all). Two files declaring a
// same-named, same-line function — only one of them impure — pin the file
// half of the fix: the old code would have seeded "impure" onto the pure
// twin too, since find_symbols_by_name("Handler") does not filter by file.
//
// This test lives in its own translation unit (rather than
// mcp_handlers_core_test.cpp) because including <lci/mcp/runtime.h> there
// pulls in <lci/analysis/codebase_intelligence_types.h>, whose
// ComplexityMetrics collides (ODR/redefinition) with the one in
// <lci/core/context_lookup_types.h> that file already includes.
TEST(McpRuntimeSeedingTest, SeedsOnlyTheImpureSymbolNotItsCrossFileNamesake) {
    auto dir = lci::test::unique_temp_dir("lci_runtime_seed_");
    std::filesystem::create_directories(dir / "foo");
    std::filesystem::create_directories(dir / "bar");
    // Same declared line (line 2) and same name in both files so a
    // name+line-only match cannot tell them apart.
    std::ofstream(dir / "foo" / "a.go")
        << "package foo\n"
           "func Handler() {\n"
           "\tpanic(\"boom\")\n"
           "}\n";
    std::ofstream(dir / "bar" / "b.go")
        << "package bar\n"
           "func Handler() {\n"
           "\treturn\n"
           "}\n";

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());

    McpRuntime runtime(indexer);
    runtime.warmup(indexer);

    auto snapshot = indexer.ref_tracker().pin();
    SymbolID impure_id{}, pure_id{};
    bool found_impure = false, found_pure = false;
    for (const auto& es : snapshot->find_symbols_by_name("Handler")) {
        if (!es) continue;
        std::string path = indexer.get_file_path(es->symbol.file_id);
        if (path.find("/foo/") != std::string::npos) {
            impure_id = es->id;
            found_impure = true;
        } else if (path.find("/bar/") != std::string::npos) {
            pure_id = es->id;
            found_pure = true;
        }
    }
    ASSERT_TRUE(found_impure);
    ASSERT_TRUE(found_pure);

    auto impure_labels = runtime.propagator.get_labels(impure_id);
    auto pure_labels = runtime.propagator.get_labels(pure_id);
    bool impure_marked = false;
    for (const auto& l : impure_labels) {
        if (l.label == "impure") impure_marked = true;
    }
    bool pure_marked = false;
    for (const auto& l : pure_labels) {
        if (l.label == "impure") pure_marked = true;
    }
    EXPECT_TRUE(impure_marked)
        << "the panic()-calling Handler in foo/ must be seeded impure";
    EXPECT_FALSE(pure_marked)
        << "the empty Handler in bar/ must NOT inherit foo/'s impure "
           "seeding just because it shares a name and start line";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace
}  // namespace mcp
}  // namespace lci
