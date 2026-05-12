// Real-project code_insight integration tests
//
// Mirrors the Go reference's workflow_scenarios/code_insight_test.go.
// These tests verify CodebaseIntelligenceEngine behavior on actual codebases.

#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "helpers/real_project_helpers.h"

namespace lci {
namespace {

namespace fs = std::filesystem;

#define SKIP_IF_NO_REAL_PROJECT(lang, name)                                 \
    do {                                                                    \
        auto _rp = testing::find_real_project((lang), (name));              \
        if (!_rp) {                                                         \
            GTEST_SKIP() << "Real project not found: " << (lang) << "/"    \
                         << (name)                                          \
                         << ". Run ./scripts/add-real-projects.sh";        \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// Test: code_insight overview on real projects
// ---------------------------------------------------------------------------

class RealProjectCodeInsightTest : public ::testing::Test {};

TEST_F(RealProjectCodeInsightTest, ChiOverviewProducesMetadata) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid()) << "Failed to index go/chi";

    nlohmann::json params;
    params["mode"] = "overview";
    auto result = ctx.code_insight(params);

    ASSERT_FALSE(result.contains("error"))
        << "code_insight error: " << result.value("error", "unknown");

    EXPECT_EQ(result["analysis_mode"].get<std::string>(), "overview");
    EXPECT_TRUE(result.contains("analysis_metadata"));
    EXPECT_TRUE(result.contains("tier"));

    auto& meta = result["analysis_metadata"];
    EXPECT_GE(meta["files_analyzed"].get<int>(), 1);
    EXPECT_GE(meta["analysis_time_ms"].get<int>(), 0);
}

TEST_F(RealProjectCodeInsightTest, ChiStatisticsProducesMetrics) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "statistics";
    auto result = ctx.code_insight(params);

    ASSERT_FALSE(result.contains("error"))
        << "code_insight error: " << result.value("error", "unknown");

    EXPECT_EQ(result["analysis_mode"].get<std::string>(), "statistics");
}

TEST_F(RealProjectCodeInsightTest, ChiStructureProducesTree) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "structure";
    auto result = ctx.code_insight(params);

    ASSERT_FALSE(result.contains("error"))
        << "code_insight error: " << result.value("error", "unknown");

    EXPECT_EQ(result["analysis_mode"].get<std::string>(), "structure");
}

TEST_F(RealProjectCodeInsightTest, ChiDetailedModulesAnalysis) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "modules";
    auto result = ctx.code_insight(params);

    ASSERT_FALSE(result.contains("error"))
        << "code_insight error: " << result.value("error", "unknown");

    EXPECT_EQ(result["analysis_mode"].get<std::string>(), "detailed");
}

TEST_F(RealProjectCodeInsightTest, ChiUnifiedMode) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "unified";
    auto result = ctx.code_insight(params);

    ASSERT_FALSE(result.contains("error"))
        << "code_insight error: " << result.value("error", "unknown");

    EXPECT_EQ(result["analysis_mode"].get<std::string>(), "unified");
}

TEST_F(RealProjectCodeInsightTest, FastapiOverview) {
    SKIP_IF_NO_REAL_PROJECT("python", "fastapi");
    auto path = *testing::find_real_project("python", "fastapi");

    auto ctx = testing::setup_real_project(path, "fastapi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "overview";
    auto result = ctx.code_insight(params);

    ASSERT_FALSE(result.contains("error"))
        << "code_insight error: " << result.value("error", "unknown");

    EXPECT_EQ(result["analysis_mode"].get<std::string>(), "overview");
    EXPECT_TRUE(result.contains("analysis_metadata"));
}

TEST_F(RealProjectCodeInsightTest, PocketbaseOverview) {
    SKIP_IF_NO_REAL_PROJECT("go", "pocketbase");
    auto path = *testing::find_real_project("go", "pocketbase");

    auto ctx = testing::setup_real_project(path, "pocketbase");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "overview";
    auto result = ctx.code_insight(params);

    ASSERT_FALSE(result.contains("error"))
        << "code_insight error: " << result.value("error", "unknown");

    EXPECT_EQ(result["analysis_mode"].get<std::string>(), "overview");
}

TEST_F(RealProjectCodeInsightTest, InvalidModeReturnsError) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "nonexistent_mode";
    auto result = ctx.code_insight(params);

    EXPECT_TRUE(result.contains("error"));
}

// ---------------------------------------------------------------------------
// Test: code_insight performance on real projects
// ---------------------------------------------------------------------------

class RealProjectCodeInsightPerformanceTest : public ::testing::Test {};

TEST_F(RealProjectCodeInsightPerformanceTest, ChiOverviewUnderOneSecond) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "overview";

    auto start = std::chrono::steady_clock::now();
    auto result = ctx.code_insight(params);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    ASSERT_FALSE(result.contains("error"))
        << "code_insight error: " << result.value("error", "unknown");

    EXPECT_LT(elapsed_ms, 1000)
        << "code_insight overview took " << elapsed_ms << "ms (should be < 1s)";
}

TEST_F(RealProjectCodeInsightPerformanceTest, ChiStatisticsUnderOneSecond) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "statistics";

    auto start = std::chrono::steady_clock::now();
    auto result = ctx.code_insight(params);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    ASSERT_FALSE(result.contains("error"));
    EXPECT_LT(elapsed_ms, 1000)
        << "code_insight statistics took " << elapsed_ms << "ms";
}

}  // namespace
}  // namespace lci
