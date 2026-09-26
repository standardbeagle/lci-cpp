#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <lci/config.h>
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_analysis.h>
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

// Build a Go source whose one function has a deliberately high cyclomatic
// complexity (many `if` branches) so it lands in high_complexity_funcs (cc
// threshold 20), with a caller-chosen name and package.
static std::string high_cc_go_function(std::string_view pkg,
                                       std::string_view name) {
    std::string s = "package " + std::string(pkg) + "\n\nfunc " +
                    std::string(name) + "(x int) int {\n\ts := 0\n";
    for (int i = 1; i <= 30; ++i) {
        s += "\tif x > " + std::to_string(i) + " { s += x }\n";
    }
    s += "\treturn s\n}\n";
    return s;
}

// S11 follow-up (criterion 1): McpRuntime::ci_engine was default-constructed,
// so HealthAnalyzer ran on the builtin PathClassifier and a project's
// `.lci.kdl` `test` attribute never reached the code_insight health gate. The
// directory `micro/` is unknown to the shipped ruleset, so the builtin
// classifier scores it as production (fallback, activates Analysis) and its
// function stays in high_complexity; only the project's own
// `attributes { test "micro/" }` can exclude it. Pre-wiring, the shipped-only
// classifier left it in; with the engine carrying the index's attr registry it
// is dropped, while the production twin (identical cc) is still reported. This
// exercises the production McpRuntime object, not a hand-built engine, so it
// pins the wiring rather than the mechanism.
// Direct @lci: labels must seed the propagator. The annotator keys
// annotations by a synthetic (file, line, column) position, and warmup
// looked them up by the real EnhancedSymbol id, so no explicit label ever
// seeded propagation.
TEST(McpRuntimeSeedingTest, SeedsDirectLciLabelsOntoTheAnnotatedSymbol) {
    auto dir = lci::test::unique_temp_dir("lci_runtime_labels_");
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "api.go")
        << "package api\n"
           "\n"
           "// @lci:labels[critical]\n"
           "func Handle() {\n"
           "}\n";

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());

    McpRuntime runtime(indexer);
    runtime.warmup(indexer);

    auto snapshot = indexer.ref_tracker().pin();
    auto handle = snapshot->find_symbol_by_name("Handle");
    ASSERT_NE(handle, nullptr);
    bool critical = false;
    for (const auto& l : runtime.propagator.get_labels(handle->id)) {
        if (l.label == "critical") critical = true;
    }
    EXPECT_TRUE(critical)
        << "an explicit @lci:labels[critical] must seed the propagator";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(McpRuntimeAttrRegistryTest, CiEngineReceivesProjectAttributesIntoHealthGate) {
    auto dir = lci::test::unique_temp_dir("lci_runtime_attr_");
    std::filesystem::create_directories(dir / "micro");
    std::filesystem::create_directories(dir / "prod");
    std::ofstream(dir / ".lci.kdl") << "attributes {\n    test \"micro/\"\n}\n";
    std::ofstream(dir / "micro" / "hot.go") << high_cc_go_function("micro", "MicroComplex");
    std::ofstream(dir / "prod" / "hot.go") << high_cc_go_function("main", "ProdComplex");

    auto loaded = load_config(dir.string());
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    loaded.config.project.root = dir.string();
    MasterIndex indexer(loaded.config);
    indexer.index_directory(dir.string());

    // The gate genuinely sees the project rule: micro/hot.go must be tagged
    // `test` by the index's own registry, else the test asserts nothing.
    {
        PathAttrId id{};
        ASSERT_TRUE(indexer.attr_registry().find("test", id));
        EXPECT_FALSE(indexer.attr_registry().activates(id, Capability::Analysis))
            << "`test` must exclude its files from the analysis gate";
    }

    McpRuntime runtime(indexer);

    // attributes=all keeps the config-tagged file in the analyzed set (the
    // shipping-scope gate would drop it before health sees it), so the only
    // thing that can exclude it from high_complexity is the health analyzer's
    // own project-aware gate — which is exactly the ci_engine wiring under
    // test. With the builtin classifier it stays; with the project registry it
    // is dropped, while the untouched production twin remains.
    nlohmann::json params;
    params["mode"] = "statistics";
    params["attributes"] = "all";
    auto result = handle_code_insight(params, runtime.ci_engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("ProdComplex"), std::string::npos)
        << "production high-cc function must remain in high_complexity:\n"
        << result.text;
    EXPECT_EQ(result.text.find("MicroComplex"), std::string::npos)
        << "a function the project tags `test` must be excluded from the "
           "health high-complexity list even under attributes=all (the config "
           "never reached the health gate):\n"
        << result.text;

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// IDX-2 review blocker B2. The server runs McpRuntime::warmup on its own
// thread once the index is ready, and a /reindex can open its bulk window
// while warmup is still writing. Warmup's side-effect passes and its
// publish() must be excluded from that window: otherwise publish() swaps the
// run's half-merged STAGING map in as the reader-visible generation before
// the run commits (or after it is cancelled). The bulk thread is parked in
// the post-parse hook — staging open, commit not reached — and warmup is
// raced against it. Readers must still see the pre-run record set — not the
// new file's SecondFunc, which exists only in the staging map. (Compared by
// key set, not map identity: begin_staging itself publishes a frozen COPY of
// the pre-run map.) Pre-fix warmup finishes inside the window and publishes
// the staging map; with the fix it blocks until the run commits.
TEST(McpRuntimeWarmupTest, WarmupDuringReindexCannotPublishTheStagingMap) {
    auto dir = lci::test::unique_temp_dir("lci_runtime_warmup_race_");
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "a.go")
        << "package main\nfunc First() { panic(\"x\") }\n";

    Config config = make_default_config();
    config.project.root = dir.string();
    MasterIndex indexer(config);
    McpRuntime runtime(indexer);
    indexer.set_side_effect_sink(&runtime.side_effects);
    ASSERT_TRUE(indexer.index_directory(dir.string()));
    runtime.warmup(indexer);

    std::set<std::string> pre_run_keys;
    for (const auto& [key, info] : runtime.side_effects.results()) {
        pre_run_keys.insert(key);
    }
    ASSERT_FALSE(pre_run_keys.empty());

    std::ofstream(dir / "b.go")
        << "package main\nfunc SecondFunc() { panic(\"y\") }\n";

    std::mutex mu;
    std::condition_variable cv;
    bool parked = false;
    bool release = false;
    bool warmup_done = false;
    std::set<std::string> observed_keys;
    std::thread warmer;
    indexer.set_post_parse_hook([&] {
        std::unique_lock lock(mu);
        parked = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });

    std::thread reader([&] {
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return parked; });
        }
        warmer = std::thread([&] {
            runtime.warmup(indexer);
            std::lock_guard lock(mu);
            warmup_done = true;
            cv.notify_all();
        });
        // Give an unexcluded warmup ample time to finish on this two-file
        // fixture; an excluded one is blocked for the whole wait.
        {
            std::unique_lock lock(mu);
            cv.wait_for(lock, std::chrono::milliseconds(500),
                        [&] { return warmup_done; });
        }
        for (const auto& [key, info] : runtime.side_effects.results()) {
            observed_keys.insert(key);
        }
        std::lock_guard lock(mu);
        release = true;
        cv.notify_all();
    });

    ASSERT_TRUE(indexer.index_directory(dir.string()));
    reader.join();
    warmer.join();

    EXPECT_EQ(observed_keys, pre_run_keys)
        << "warmup published a generation inside the reindex bulk window";
    bool has_second = false;
    for (const auto& [key, info] : runtime.side_effects.results()) {
        if (info.function_name == "SecondFunc") has_second = true;
    }
    EXPECT_TRUE(has_second)
        << "the committed generation is missing the new run's record";
    std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace mcp
}  // namespace lci
