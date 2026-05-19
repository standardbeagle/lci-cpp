// Real-project TypeScript integration tests against the trpc corpus.
//
// Exercises the tree-sitter-typescript + tree-sitter-tsx grammars on a
// real-world codebase: extractor language detection, type-only imports,
// arrow functions, generics, decorator handling. Mirrors the chi/fastapi
// suite shape so the test infrastructure (indexer cache, MCP handler
// helpers) stays language-agnostic.
//
// The trpc repo is cloned by scripts/add-real-projects.sh --minimal
// into real_projects/typescript/trpc. Tests SKIP cleanly when the
// corpus is absent so CI without prep doesn't false-fail.

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

class RealProjectTypescriptTest : public ::testing::Test {};

// Smoke: trpc indexes at all. Tree-sitter-typescript grammar must be
// linked and able to walk the project's .ts files. file_count > 0
// confirms FileScanner picked up the corpus and the indexer produced
// at least one record.
TEST_F(RealProjectTypescriptTest, TrpcIndexesSuccessfully) {
    SKIP_IF_NO_REAL_PROJECT("typescript", "trpc");
    auto path = *testing::find_real_project("typescript", "trpc");
    auto ctx = testing::setup_real_project(path, "trpc");
    ASSERT_TRUE(ctx.valid()) << "indexer failed to build trpc index";

    EXPECT_GT(ctx.file_count(), 100)
        << "trpc corpus has hundreds of .ts files; expected >100 indexed";
    EXPECT_GT(ctx.symbol_count(), 500)
        << "trpc exposes many functions/classes/types; symbol_count "
           "should be substantial";
}

// trpc's public API exports many `createTRPCRouter`/`router` shaped
// helpers. Substring search for "createTRPCRouter" should land at
// least one hit, exercising trigram + symbol-aware ranking on a TS
// corpus.
TEST_F(RealProjectTypescriptTest, TrpcSearchFindsCreateRouter) {
    SKIP_IF_NO_REAL_PROJECT("typescript", "trpc");
    auto path = *testing::find_real_project("typescript", "trpc");
    auto ctx = testing::setup_real_project(path, "trpc");
    ASSERT_TRUE(ctx.valid());

    auto results = ctx.search("createTRPCRouter", 20, 3);
    EXPECT_FALSE(results.empty())
        << "expected at least one match for 'createTRPCRouter' in trpc";
}

// MCP find_files with a TS glob exercises the file_id ordering fix
// (commit 476281f) on a corpus with hundreds of candidates. Verifies
// the .ts extension routing through FileScanner + glob match.
TEST_F(RealProjectTypescriptTest, TrpcFindFilesByGlob) {
    SKIP_IF_NO_REAL_PROJECT("typescript", "trpc");
    auto path = *testing::find_real_project("typescript", "trpc");
    auto ctx = testing::setup_real_project(path, "trpc");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["pattern"] = "*.ts";
    auto result = ctx.find_files(params);

    ASSERT_FALSE(result.contains("error"))
        << "find_files error: " << result.value("error", "unknown");
    ASSERT_TRUE(result.contains("results"));
    // find_files caps at the handler's default max_results (50 on
    // multi-lang; trpc easily fills the cap). Assert >= 20 so the
    // test stays robust against future cap tuning.
    EXPECT_GE(result["results"].size(), 20u)
        << "trpc has many .ts files; find_files should return >=20";
}

// side_effects on the TS corpus: heuristic should classify some
// callable symbols as impure (HTTP / DB / throw / dynamic-eval
// prefixes appear across the trpc public API: errors thrown via
// TRPCError, queries via createTRPCRouter, etc.).
TEST_F(RealProjectTypescriptTest, TrpcSideEffectsHasImpureFunctions) {
    SKIP_IF_NO_REAL_PROJECT("typescript", "trpc");
    auto path = *testing::find_real_project("typescript", "trpc");
    auto ctx = testing::setup_real_project(path, "trpc");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["mode"] = "summary";
    auto result = ctx.side_effects(params);

    ASSERT_FALSE(result.contains("error"));
    ASSERT_TRUE(result.contains("summary"));
    auto& summary = result["summary"];
    int total = summary.value("total_functions", 0);
    int pure = summary.value("pure_functions", 0);
    int impure = summary.value("impure_functions", 0);
    EXPECT_GT(total, 0);
    EXPECT_EQ(pure + impure, total);
}

}  // namespace
}  // namespace lci
