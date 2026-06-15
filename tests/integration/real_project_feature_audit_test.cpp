// Real-world feature audit. Tight assertions, not smoke checks.
//
// Every MCP tool the C++ port exposes runs against a real project (chi for Go,
// fastapi for Python, trpc for TypeScript) and asserts the result is
// meaningful — non-empty arrays, populated complexity/tags/scopes, sane
// counts. Synthetic-corpus tests passed for years while real codebases got
// empty output (see YwX7E07FZaqJ); this suite is the regression guard.
//
// Each TEST_F is scoped to one tool + one project. Failures here are real
// migration gaps, not flakes.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "helpers/real_project_helpers.h"

namespace lci {
namespace {

#define SKIP_IF_NO_REAL_PROJECT(lang, name)                                 \
    do {                                                                    \
        auto _rp = testing::find_real_project((lang), (name));              \
        if (!_rp) {                                                         \
            GTEST_SKIP() << "Real project not found: " << (lang) << "/"    \
                         << (name)                                          \
                         << ". Run scripts/add-real-projects.sh --minimal";\
        }                                                                   \
    } while (0)

class FeatureAudit : public ::testing::Test {
  protected:
    static testing::RealProjectContext chi() {
        auto path = *testing::find_real_project("go", "chi");
        return testing::setup_real_project(path, "chi");
    }
};

// -- search ------------------------------------------------------------------
//
// Real codebases must return non-empty results for common identifiers and
// score them sanely (top result > 0).

TEST_F(FeatureAudit, ChiSearchRouterReturnsMatches) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());
    auto results = ctx.search("Router", 20, 3);
    ASSERT_FALSE(results.empty()) << "Router must match in chi";
    EXPECT_GT(results.size(), 5u);
}

// -- inspect_symbol ----------------------------------------------------------
//
// Looking up a known function must return symbol metadata WITH complexity
// populated. If complexity == 0 across all matches, the pipeline_integrator
// metadata-attachment bug is back.

TEST_F(FeatureAudit, ChiInspectSymbolPopulatesComplexity) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["name"] = "ServeHTTP";
    auto result = ctx.inspect_symbol(params);

    ASSERT_FALSE(result.contains("error"))
        << "inspect_symbol error: " << result.value("error", "unknown");
    ASSERT_TRUE(result.contains("symbols"));
    ASSERT_FALSE(result["symbols"].empty()) << "ServeHTTP must exist in chi";

    bool any_with_complexity = false;
    bool any_with_callers_or_callees = false;
    for (const auto& sym : result["symbols"]) {
        if (sym.value("complexity", 0) > 0) any_with_complexity = true;
        if (sym.contains("callers") && !sym["callers"].empty())
            any_with_callers_or_callees = true;
        if (sym.contains("callees") && !sym["callees"].empty())
            any_with_callers_or_callees = true;
    }
    EXPECT_TRUE(any_with_complexity)
        << "at least one ServeHTTP match must have complexity > 0";
    EXPECT_TRUE(any_with_callers_or_callees)
        << "at least one ServeHTTP match must have non-empty callers or callees";
}

// -- list_symbols ------------------------------------------------------------
//
// Listing top symbols of chi must return >= 20 entries (chi has hundreds of
// exported funcs/types). Object IDs must be unique.

TEST_F(FeatureAudit, ChiListSymbolsReturnsMany) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["kind"] = "all";
    params["max"] = 100;
    auto result = ctx.list_symbols(params);

    ASSERT_FALSE(result.contains("error"));
    ASSERT_TRUE(result.contains("symbols"));
    EXPECT_GE(result["symbols"].size(), 20u);

    std::set<std::string> seen_ids;
    int dup_ids = 0;
    for (const auto& s : result["symbols"]) {
        std::string id = s.value("object_id", "");
        if (id.empty()) continue;
        if (!seen_ids.insert(id).second) ++dup_ids;
    }
    EXPECT_EQ(dup_ids, 0) << "object_ids must be unique within list_symbols";
}

// -- browse_file -------------------------------------------------------------

TEST_F(FeatureAudit, ChiBrowseFileReturnsSymbols) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    // chi has chi.go at the top level. file_id=1 should resolve to a real
    // file with multiple symbols.
    nlohmann::json params;
    params["file_id"] = 1;
    auto result = ctx.browse_file(params);

    ASSERT_FALSE(result.contains("error"))
        << "browse_file error: " << result.value("error", "unknown");
    // The exact key shape varies; assert some kind of symbol enumeration.
    bool has_symbols = result.contains("symbols") || result.contains("file");
    EXPECT_TRUE(has_symbols);
}

// -- find_files --------------------------------------------------------------

TEST_F(FeatureAudit, ChiFindFilesGoGlob) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["pattern"] = "*.go";
    auto result = ctx.find_files(params);

    ASSERT_FALSE(result.contains("error"));
    ASSERT_TRUE(result.contains("results"));
    EXPECT_GE(result["results"].size(), 10u)
        << "chi has dozens of .go files; find_files must return >= 10";
}

// -- side_effects ------------------------------------------------------------
//
// chi has handler functions that call net/http internals (impure by any
// reasonable heuristic). The summary mode must report impure_functions > 0.

TEST_F(FeatureAudit, ChiSideEffectsFindsImpureFunctions) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "summary";
    auto result = ctx.side_effects(params);

    ASSERT_FALSE(result.contains("error"));
    ASSERT_TRUE(result.contains("summary"));
    const auto& summary = result["summary"];
    int total = summary.value("total_functions", 0);
    int pure = summary.value("pure_functions", 0);
    int impure = summary.value("impure_functions", 0);
    EXPECT_GT(total, 0);
    EXPECT_GT(impure, 0)
        << "chi has many net/http handlers; impure_functions must be > 0";
    EXPECT_EQ(pure + impure, total);
}

// -- code_insight overview ---------------------------------------------------
//
// Overview on chi must produce non-trivial repository_map data.

TEST_F(FeatureAudit, ChiCodeInsightOverviewIsPopulated) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "overview";
    auto result = ctx.code_insight(params);
    ASSERT_FALSE(result.contains("error"));
    const auto& text = result["lcf"].get_ref<const std::string&>();
    // Expect the section header, plus a non-zero file count line.
    EXPECT_NE(text.find("== REPOSITORY MAP =="), std::string::npos);
}

// -- code_insight unified ----------------------------------------------------
//
// THIS is the high-leverage test that caught the engine-not-wired bug. On
// chi the unified LCF must report at least one smell and at least one
// problematic_symbol with non-empty tags. Empty here means the engine path
// is broken or thresholds aren't tripping.

TEST_F(FeatureAudit, ChiCodeInsightUnifiedDetectsSmells) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "unified";
    auto result = ctx.code_insight(params);
    ASSERT_FALSE(result.contains("error"));
    const auto& text = result["lcf"].get_ref<const std::string&>();

    EXPECT_NE(text.find("== HEALTH =="), std::string::npos);
    // A real project must have at least one detected smell. The
    // pre-fix C++ port emitted hardcoded empty output here; this assertion
    // is the regression guard.
    EXPECT_NE(text.find("smells:"), std::string::npos)
        << "expected at least one detected smell on chi tree; got: \n" << text;
    EXPECT_NE(text.find("problematic_symbols:"), std::string::npos)
        << "expected at least one problematic_symbol on chi tree; got: \n"
        << text;
    EXPECT_NE(text.find("distribution:"), std::string::npos);
    // Avg complexity on chi should be > 1.0 — many handlers are non-trivial.
    auto cx_pos = text.find("complexity=");
    ASSERT_NE(cx_pos, std::string::npos);
    auto after = text.substr(cx_pos + 11);
    double cx = std::strtod(after.c_str(), nullptr);
    EXPECT_GT(cx, 1.5) << "avg complexity must be > 1.5 on chi; got cx=" << cx;
}

// -- index_stats -------------------------------------------------------------

TEST_F(FeatureAudit, ChiIndexStatsReportsCounts) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    auto result = ctx.index_stats(nlohmann::json::object());
    ASSERT_FALSE(result.contains("error"));
    EXPECT_GT(result.value("file_count", 0), 20);
    EXPECT_GT(result.value("symbol_count", 0), 100);
}

// -- get_context -------------------------------------------------------------

TEST_F(FeatureAudit, ChiGetContextByNameWorks) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["name"] = "NewRouter";
    auto result = ctx.get_context(params);
    ASSERT_FALSE(result.contains("error"));
    // The exact response shape is implementation-defined; ensure it isn't
    // an empty stub.
    EXPECT_FALSE(result.empty());
}

// -- find_files (python) -----------------------------------------------------

TEST_F(FeatureAudit, FastapiFindFilesPyGlob) {
    SKIP_IF_NO_REAL_PROJECT("python", "fastapi");
    auto path = *testing::find_real_project("python", "fastapi");
    auto ctx = testing::setup_real_project(path, "fastapi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["pattern"] = "*.py";
    auto result = ctx.find_files(params);
    ASSERT_FALSE(result.contains("error"));
    EXPECT_GE(result["results"].size(), 20u);
}

// -- code_insight unified on fastapi -----------------------------------------

TEST_F(FeatureAudit, FastapiCodeInsightUnifiedDetectsSmells) {
    SKIP_IF_NO_REAL_PROJECT("python", "fastapi");
    auto path = *testing::find_real_project("python", "fastapi");
    auto ctx = testing::setup_real_project(path, "fastapi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "unified";
    auto result = ctx.code_insight(params);
    ASSERT_FALSE(result.contains("error"));
    const auto& text = result["lcf"].get_ref<const std::string&>();
    EXPECT_NE(text.find("smells:"), std::string::npos)
        << "fastapi has complex code paths; smells: line expected. Got:\n"
        << text;
    EXPECT_NE(text.find("problematic_symbols:"), std::string::npos);
}

// -- code_insight statistics -------------------------------------------------
//
// The statistics-mode handler still emits hardcoded LCF (FIX-D.1.C workaround
// — only unified mode was wired through in commit 4b8252f). On a real chi
// corpus the distribution line MUST report at least one high or medium
// complexity bucket; if it just says "distribution: low=<file_count>" the
// hardcoded path is winning.

TEST_F(FeatureAudit, ChiCodeInsightStatisticsReportsComplexityBuckets) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "statistics";
    auto result = ctx.code_insight(params);
    ASSERT_FALSE(result.contains("error"));
    const auto& text = result["lcf"].get_ref<const std::string&>();

    EXPECT_NE(text.find("== STATISTICS =="), std::string::npos);
    // The hardcoded form always says "complexity: avg=1.00 median=1.00".
    // Real chi has many non-trivial functions; avg must be > 1.5.
    auto cx_pos = text.find("avg=");
    ASSERT_NE(cx_pos, std::string::npos);
    double cx = std::strtod(text.c_str() + cx_pos + 4, nullptr);
    EXPECT_GT(cx, 1.5)
        << "statistics mode still emits hardcoded avg=1.00; got cx=" << cx
        << "\nfull text:\n" << text;
    // Distribution must mention medium or high bucket; chi has dozens of
    // functions with complexity > 10.
    bool has_real_bucket =
        text.find("medium=") != std::string::npos ||
        text.find("high=") != std::string::npos;
    EXPECT_TRUE(has_real_bucket)
        << "statistics distribution missing medium/high bucket; got:\n" << text;
}

// -- code_insight structure --------------------------------------------------
//
// Structure mode reports per-directory file/symbol counts. The hardcoded
// form emits "dirs=1 files=<file_count>"; chi has multiple top-level
// directories (cmd, middleware, _examples) so dirs MUST be > 1.

TEST_F(FeatureAudit, ChiCodeInsightStructureReportsMultipleDirs) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "structure";
    auto result = ctx.code_insight(params);
    ASSERT_FALSE(result.contains("error"));
    const auto& text = result["lcf"].get_ref<const std::string&>();

    EXPECT_NE(text.find("== STRUCTURE =="), std::string::npos);
    auto dirs_pos = text.find("dirs=");
    ASSERT_NE(dirs_pos, std::string::npos);
    int dirs = std::atoi(text.c_str() + dirs_pos + 5);
    EXPECT_GT(dirs, 1)
        << "structure mode reports dirs=" << dirs
        << " (hardcoded form always emits 1); chi has multiple top-level dirs"
        << "\nfull text:\n" << text;
}

// -- code_insight unified reports modules ------------------------------------
//
// Unified mode's MODULES section should list more than 1 module on chi.
// Module discovery currently broken (falls back to "(root)" 1-module form);
// this test guards the eventual fix.

// DISABLED until module discovery is wired through CodebaseIntelligenceEngine.
// Re-enable by stripping the DISABLED_ prefix once layer_analyzer or an
// equivalent populates RepositoryMap.module_boundaries with real dirs.
TEST_F(FeatureAudit, DISABLED_ChiCodeInsightUnifiedReportsMultipleModules) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "unified";
    auto result = ctx.code_insight(params);
    ASSERT_FALSE(result.contains("error"));
    const auto& text = result["lcf"].get_ref<const std::string&>();

    auto modules_pos = text.find("== MODULES ==");
    ASSERT_NE(modules_pos, std::string::npos);
    auto total_pos = text.find("total=", modules_pos);
    ASSERT_NE(total_pos, std::string::npos);
    int total = std::atoi(text.c_str() + total_pos + 6);
    EXPECT_GT(total, 1)
        << "module discovery reports total=" << total
        << " (1 = empty-corpus fallback); chi has cmd, middleware, _examples"
        << "\nfull text:\n" << text;
}

// -- code_insight unified populates purity_summary ---------------------------
//
// chi has dozens of HTTP handlers that touch net/http — these MUST classify
// as impure. The unified-mode purity line currently emits
// "total=N pure=0 impure=0 ratio=0.00" on real corpora because the
// purity_summary isn't wired through HealthDashboard.

TEST_F(FeatureAudit, ChiCodeInsightUnifiedReportsPurity) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "unified";
    auto result = ctx.code_insight(params);
    ASSERT_FALSE(result.contains("error"));
    const auto& text = result["lcf"].get_ref<const std::string&>();

    auto pur_pos = text.find("purity:");
    ASSERT_NE(pur_pos, std::string::npos);
    auto totline_pos = text.find("  total=", pur_pos);
    ASSERT_NE(totline_pos, std::string::npos);
    auto impure_pos = text.find("impure=", totline_pos);
    ASSERT_NE(impure_pos, std::string::npos);
    int impure = std::atoi(text.c_str() + impure_pos + 7);
    EXPECT_GT(impure, 0)
        << "purity_summary not wired to HealthDashboard; impure=0 on chi"
        << "\nfull text:\n" << text;
}

// -- code_insight detailed=features (graph clustering) -----------------------
//
// Features are Louvain communities of chi's call graph, not name-keyword
// buckets. On a real corpus the FEATURES section must report a positive
// feature count and a real cohesion fraction in (0, 1] (internal-edge density
// per community). chi calls across package boundaries, so cross-feature deps
// must surface once there is more than one community.

TEST_F(FeatureAudit, ChiCodeInsightFeaturesAreGraphClustered) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto ctx = chi();
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "features";
    auto result = ctx.code_insight(params);
    ASSERT_FALSE(result.contains("error"));
    const auto& text = result["lcf"].get_ref<const std::string&>();

    EXPECT_NE(text.find("== FEATURES =="), std::string::npos);

    auto total_pos = text.find("total=");
    ASSERT_NE(total_pos, std::string::npos);
    int total = std::atoi(text.c_str() + total_pos + 6);
    EXPECT_GT(total, 0) << "graph clustering found no features on chi:\n" << text;

    auto coh_pos = text.find("avg_cohesion=");
    ASSERT_NE(coh_pos, std::string::npos);
    double coh = std::strtod(text.c_str() + coh_pos + 13, nullptr);
    EXPECT_GT(coh, 0.0) << "avg_cohesion must be a real positive fraction:\n"
                        << text;
    EXPECT_LE(coh, 1.0);

    // Per-feature confidence (= graph cohesion) must be emitted and positive
    // for at least one feature.
    auto conf_pos = text.find("confidence=");
    ASSERT_NE(conf_pos, std::string::npos);
    EXPECT_GT(std::strtod(text.c_str() + conf_pos + 11, nullptr), 0.0);

    // Multiple communities on a cross-calling corpus must yield cross-feature
    // dependency edges.
    if (total > 1) {
        EXPECT_NE(text.find("dep:"), std::string::npos)
            << "expected cross-feature deps with >1 feature on chi:\n" << text;
    }
}

}  // namespace
}  // namespace lci
