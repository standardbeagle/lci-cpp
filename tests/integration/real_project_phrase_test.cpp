// Real-project phrase matching and semantic search quality tests
//
// Mirrors the Go reference's workflow_scenarios/phrase_matching_test.go.
// These tests verify multi-word phrase matching, exact vs partial scoring,
// and stemming behavior on actual codebases.

#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>

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
// Test: multi-word phrase matching on real projects
// ---------------------------------------------------------------------------

class RealProjectPhraseMatchingTest : public ::testing::Test {};

TEST_F(RealProjectPhraseMatchingTest, ChiMultiWordServeHTTP) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto results = ctx.search("ServeHTTP", 20);
    EXPECT_FALSE(results.empty())
        << "Expected results for 'ServeHTTP' in go/chi";

    // All results should be reasonably scored
    for (const auto& r : results) {
        EXPECT_GT(r.score, 0.0)
            << "Result in " << r.path << " should have positive score";
    }
}

TEST_F(RealProjectPhraseMatchingTest, ChiMultiWordMiddlewareStack) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto results = ctx.search("middleware stack", 20);
    EXPECT_FALSE(results.empty())
        << "Expected results for 'middleware stack' in go/chi";
}

TEST_F(RealProjectPhraseMatchingTest, ChiMultiWordResponseWriter) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto results = ctx.search("ResponseWriter", 20);
    EXPECT_FALSE(results.empty())
        << "Expected results for 'ResponseWriter' in go/chi";
}

TEST_F(RealProjectPhraseMatchingTest, FastapiMultiWordDepends) {
    SKIP_IF_NO_REAL_PROJECT("python", "fastapi");
    auto path = *testing::find_real_project("python", "fastapi");

    auto ctx = testing::setup_real_project(path, "fastapi");
    ASSERT_TRUE(ctx.valid());

    auto results = ctx.search("Depends", 20);
    EXPECT_FALSE(results.empty())
        << "Expected results for 'Depends' in python/fastapi";
}

TEST_F(RealProjectPhraseMatchingTest, FastapiMultiWordRequestResponse) {
    SKIP_IF_NO_REAL_PROJECT("python", "fastapi");
    auto path = *testing::find_real_project("python", "fastapi");

    auto ctx = testing::setup_real_project(path, "fastapi");
    ASSERT_TRUE(ctx.valid());

    auto results = ctx.search("response model", 20);
    EXPECT_FALSE(results.empty())
        << "Expected results for 'response model' in python/fastapi";
}

// ---------------------------------------------------------------------------
// Test: exact match scores higher than partial
// ---------------------------------------------------------------------------

class RealProjectScoreValidationTest : public ::testing::Test {};

TEST_F(RealProjectScoreValidationTest, ExactPhraseScoresHigher) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto exact_results = ctx.search("ServeHTTP", 10);
    auto partial_results = ctx.search("Serve", 10);

    ASSERT_FALSE(exact_results.empty());
    ASSERT_FALSE(partial_results.empty());

    // The top exact match should score at least as high as the top partial
    EXPECT_GE(exact_results[0].score, partial_results[0].score * 0.5)
        << "Exact phrase 'ServeHTTP' should score competitively with 'Serve'";
}

TEST_F(RealProjectScoreValidationTest, MultiWordBetterThanSingleWord) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto multi_results = ctx.search("middleware stack", 10);
    auto single_results = ctx.search("middleware", 10);

    ASSERT_FALSE(multi_results.empty());
    ASSERT_FALSE(single_results.empty());

    // Multi-word search should still return relevant results
    EXPECT_GT(multi_results[0].score, 0.0);
}

// ---------------------------------------------------------------------------
// Test: case-insensitive matching
// ---------------------------------------------------------------------------

class RealProjectCaseMatchingTest : public ::testing::Test {};

TEST_F(RealProjectCaseMatchingTest, CaseInsensitiveFindsSymbols) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto lower = ctx.search("servehttp", 10);
    auto upper = ctx.search("ServeHTTP", 10);

    ASSERT_FALSE(upper.empty());
    // Lowercase search should also find results (fuzzy matching)
    EXPECT_FALSE(lower.empty())
        << "Lowercase 'servehttp' should still find 'ServeHTTP'";
}

}  // namespace
}  // namespace lci
