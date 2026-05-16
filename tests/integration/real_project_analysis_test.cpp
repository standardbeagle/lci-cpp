// Real-project code_insight integration tests
//
// Mirrors the Go reference's workflow_scenarios/code_insight_test.go.
// These tests verify CodebaseIntelligenceEngine behavior on actual codebases.
//
// FIX-D.1.C: handle_code_insight emits LCF text (not JSON) to match Go's
// wire format. The ctx.code_insight() helper wraps success payloads as
// {"lcf": "<text>"} — tests assert on substrings of that text.

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

namespace {

// Asserts the LCF payload contains the canonical header + the given mode
// declaration line.
void expect_lcf_mode(const nlohmann::json& result, const std::string& mode) {
    ASSERT_TRUE(result.contains("lcf"))
        << "code_insight returned non-LCF payload: " << result.dump();
    const auto& text = result["lcf"].get_ref<const std::string&>();
    EXPECT_NE(text.find("LCF/1.0\n"), std::string::npos)
        << "missing LCF header in payload: " << text;
    EXPECT_NE(text.find("mode=" + mode + "\n"), std::string::npos)
        << "missing mode=" << mode << " line in payload: " << text;
}

}  // namespace

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

    expect_lcf_mode(result, "overview");
    const auto& text = result["lcf"].get_ref<const std::string&>();
    EXPECT_NE(text.find("== REPOSITORY MAP =="), std::string::npos);
    EXPECT_NE(text.find("== HEALTH =="), std::string::npos);
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

    expect_lcf_mode(result, "statistics");
    EXPECT_NE(result["lcf"].get_ref<const std::string&>().find("== STATISTICS =="),
              std::string::npos);
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

    expect_lcf_mode(result, "structure");
    EXPECT_NE(result["lcf"].get_ref<const std::string&>().find("== STRUCTURE =="),
              std::string::npos);
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

    // "detailed" mode falls through to the overview LCF payload — matches
    // Go's shape on corpora without dependency-graph data wired. The mode
    // header therefore reads `mode=overview`.
    expect_lcf_mode(result, "overview");
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

    expect_lcf_mode(result, "unified");
    const auto& text = result["lcf"].get_ref<const std::string&>();
    EXPECT_NE(text.find("== REPOSITORY MAP =="), std::string::npos);
    EXPECT_NE(text.find("== HEALTH =="), std::string::npos);
    EXPECT_NE(text.find("== MODULES =="), std::string::npos);
    EXPECT_NE(text.find("== STATISTICS =="), std::string::npos);
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

    expect_lcf_mode(result, "overview");
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

    expect_lcf_mode(result, "overview");
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
    EXPECT_LT(elapsed_ms, 1000) << "code_insight overview took " << elapsed_ms << "ms";
}

}  // namespace
}  // namespace lci
