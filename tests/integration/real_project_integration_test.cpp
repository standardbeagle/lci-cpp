// Real-project integration tests
//
// Mirrors the Go reference's workflow tests (internal/mcp/workflow_helpers.go
// and internal/mcp/workflow_scenarios/*.go). These tests verify LCI behavior
// against actual open-source codebases.
//
// All tests skip gracefully if real_projects/ is not populated, so the test
// suite passes even without submodules checked out.

#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "helpers/real_project_helpers.h"

namespace lci {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Skips the current test if the real project is not available.
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
// Test: indexing real projects completes successfully
// ---------------------------------------------------------------------------

class RealProjectIndexingTest : public ::testing::Test {};

TEST_F(RealProjectIndexingTest, GoChiIndexesSuccessfully) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid()) << "Failed to index go/chi";

    EXPECT_GT(ctx.file_count(), 0)
        << "Expected non-zero file count for go/chi";
    EXPECT_GT(ctx.symbol_count(), 0)
        << "Expected non-zero symbol count for go/chi";

    // Chi is a small web framework; should have ServeHTTP and Router symbols
    auto results = ctx.search("ServeHTTP", 10);
    EXPECT_FALSE(results.empty()) << "Expected to find ServeHTTP in go/chi";
}

TEST_F(RealProjectIndexingTest, PythonFastapiIndexesSuccessfully) {
    SKIP_IF_NO_REAL_PROJECT("python", "fastapi");
    auto path = *testing::find_real_project("python", "fastapi");

    auto ctx = testing::setup_real_project(path, "fastapi");
    ASSERT_TRUE(ctx.valid()) << "Failed to index python/fastapi";

    EXPECT_GT(ctx.file_count(), 0)
        << "Expected non-zero file count for python/fastapi";
    EXPECT_GT(ctx.symbol_count(), 0)
        << "Expected non-zero symbol count for python/fastapi";

    auto results = ctx.search("Depends", 10);
    EXPECT_FALSE(results.empty())
        << "Expected to find Depends in python/fastapi";
}

TEST_F(RealProjectIndexingTest, GoPocketbaseIndexesSuccessfully) {
    SKIP_IF_NO_REAL_PROJECT("go", "pocketbase");
    auto path = *testing::find_real_project("go", "pocketbase");

    auto ctx = testing::setup_real_project(path, "pocketbase");
    ASSERT_TRUE(ctx.valid()) << "Failed to index go/pocketbase";

    EXPECT_GT(ctx.file_count(), 0)
        << "Expected non-zero file count for go/pocketbase";
    EXPECT_GT(ctx.symbol_count(), 0)
        << "Expected non-zero symbol count for go/pocketbase";
}

// ---------------------------------------------------------------------------
// Test: search quality on real projects
// ---------------------------------------------------------------------------

class RealProjectSearchTest : public ::testing::Test {};

TEST_F(RealProjectSearchTest, GoChiFindsRouterMiddleware) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto results = ctx.search("middleware stack", 20, 3);
    EXPECT_FALSE(results.empty())
        << "Expected search results for 'middleware stack' in go/chi";

    // Results should have scores > 0
    for (const auto& r : results) {
        EXPECT_GT(r.score, 0.0)
            << "Expected positive score for result in " << r.path;
    }
}

TEST_F(RealProjectSearchTest, PythonFastapiFindsAppClass) {
    SKIP_IF_NO_REAL_PROJECT("python", "fastapi");
    auto path = *testing::find_real_project("python", "fastapi");

    auto ctx = testing::setup_real_project(path, "fastapi");
    ASSERT_TRUE(ctx.valid());

    auto results = ctx.search("FastAPI", 20, 3);
    EXPECT_FALSE(results.empty())
        << "Expected search results for 'FastAPI' in python/fastapi";
}

// ---------------------------------------------------------------------------
// Test: index stats are reasonable
// ---------------------------------------------------------------------------

class RealProjectStatsTest : public ::testing::Test {};

TEST_F(RealProjectStatsTest, ChiStatsAreReasonable) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto stats = ctx.stats();
    EXPECT_GT(stats.total_files, 0);
    EXPECT_GT(stats.total_symbols, 0);
    EXPECT_GE(stats.total_files, stats.total_symbols / 100)
        << "Expected reasonable file-to-symbol ratio";
}

// ---------------------------------------------------------------------------
// Test: concurrent search on indexed real project
// ---------------------------------------------------------------------------

class RealProjectConcurrentTest : public ::testing::Test {};

TEST_F(RealProjectConcurrentTest, ConcurrentSearchesOnChi) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    const int kThreads = 4;
    const int kQueriesPerThread = 10;

    std::vector<std::string> queries = {
        "ServeHTTP",
        "Router",
        "Middleware",
        "Context",
        "Handler",
    };

    std::atomic<int> total_results{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int q = 0; q < kQueriesPerThread; ++q) {
                const auto& query = queries[q % queries.size()];
                auto results = ctx.search(query, 10);
                total_results.fetch_add(
                    static_cast<int>(results.size()),
                    std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_GT(total_results.load(), 0)
        << "Expected non-zero total results from concurrent searches";
}

// ---------------------------------------------------------------------------
// Test: all available projects can be listed and indexed
// ---------------------------------------------------------------------------

class RealProjectAvailabilityTest : public ::testing::Test {};

TEST_F(RealProjectAvailabilityTest, ListsAvailableProjects) {
    auto projects = testing::list_available_real_projects();

    // This test passes even if no projects are available (just reports it)
    if (projects.empty()) {
        GTEST_SKIP() << "No real projects available. "
                        "Run ./scripts/add-real-projects.sh to populate.";
    }

    EXPECT_GE(projects.size(), 1)
        << "Expected at least one available real project";

    for (const auto& [lang, name] : projects) {
        EXPECT_FALSE(lang.empty());
        EXPECT_FALSE(name.empty());
    }
}

TEST_F(RealProjectAvailabilityTest, EachAvailableProjectIndexes) {
    auto projects = testing::list_available_real_projects();
    if (projects.empty()) {
        GTEST_SKIP() << "No real projects available";
    }

    for (const auto& [lang, name] : projects) {
        auto path = testing::find_real_project(lang, name);
        ASSERT_TRUE(path) << "Failed to find " << lang << "/" << name;

        auto ctx = testing::setup_real_project(*path, name);
        ASSERT_TRUE(ctx.valid())
            << "Failed to index " << lang << "/" << name;

        EXPECT_GT(ctx.file_count(), 0)
            << lang << "/" << name << " should have indexed files";
    }
}

}  // namespace
}  // namespace lci
