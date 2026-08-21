// Pure-logic tests for the error-handling / resource scoring model
// (docs/plans/2026-08-17-error-handling-score-design.md).

#include <lci/analysis/error_handling_analyzer.h>
#include <lci/path_classifier.h>

#include <gtest/gtest.h>

namespace lci {
namespace {

// The analysis gate is the attribute registry — the same one search, refs,
// and every other tool read. The analyzer used to carry a private substring
// list ("test", "mock", "/libs/"), which knew nothing about the shipped
// ruleset or a project's `.lci.kdl`: a benchmark harness scored as product
// code, and a configured attribute never reached this gate.
bool scores_as_product_code(std::string_view rel_path) {
    PathClassifier classifier;
    return classifier.registry().activates(classifier.classify(rel_path),
                                           Capability::Analysis);
}

TEST(EhAnalysisGateTest, TestAndVendorPathsAreExcluded) {
    EXPECT_FALSE(scores_as_product_code("foo_test.go"));
    EXPECT_FALSE(scores_as_product_code("tests/helper.py"));
    EXPECT_FALSE(scores_as_product_code("vendor/lib/x.go"));
    EXPECT_FALSE(scores_as_product_code("node_modules/a/b.js"));
    EXPECT_FALSE(scores_as_product_code("examples/demo.rs"));
    EXPECT_FALSE(scores_as_product_code("src/foo.spec.ts"));
    EXPECT_FALSE(scores_as_product_code("internal/mocks/store.go"));
}

// pocketbase verification: ui/public/libs minified bundles were scoring and
// even owned the worst-module slot — vendored/minified/built output never
// scores (insight-verification lesson: vendor contamination).
TEST(EhAnalysisGateTest, MinifiedAndBundledAssetsAreExcluded) {
    EXPECT_FALSE(scores_as_product_code("ui/public/libs/uplot/uplot.iife.js"));
    EXPECT_FALSE(scores_as_product_code("dist/app.min.js"));
    EXPECT_FALSE(scores_as_product_code("ui/public/app.js"));
}

// The finding that started this: a benchmark harness is not the product, so
// its deliberately lax error handling is not a defect in the product.
TEST(EhAnalysisGateTest, BenchmarkHarnessesAreExcluded) {
    EXPECT_FALSE(scores_as_product_code("benchmarks/repo-qa/scripts/bench.py"));
    EXPECT_FALSE(scores_as_product_code("bench/latency.cpp"));
    EXPECT_FALSE(scores_as_product_code("pkg/store/store_bench.go"));
}

TEST(EhAnalysisGateTest, PlainSourcePathsAreIncluded) {
    EXPECT_TRUE(scores_as_product_code("src/server.go"));
    EXPECT_TRUE(scores_as_product_code("core/logger.go"));
}

TEST(EhScoringTest, DeductionScalesWithSeverityConfidenceFanin) {
    // Leaf function (fan-in 0): half weight.
    double leaf_high =
        ErrorHandlingAnalyzer::finding_deduction(FindingSeverity::High, 1.0,
                                                 0.0);
    // Load-bearing function (max fan-in): full weight.
    double hub_high =
        ErrorHandlingAnalyzer::finding_deduction(FindingSeverity::High, 1.0,
                                                 1.0);
    EXPECT_DOUBLE_EQ(leaf_high * 2.0, hub_high);
    // Severity ordering holds at fixed confidence + fan-in.
    double med = ErrorHandlingAnalyzer::finding_deduction(FindingSeverity::Med,
                                                          1.0, 1.0);
    double low = ErrorHandlingAnalyzer::finding_deduction(FindingSeverity::Low,
                                                          1.0, 1.0);
    EXPECT_GT(hub_high, med);
    EXPECT_GT(med, low);
    // Confidence scales linearly.
    EXPECT_DOUBLE_EQ(
        ErrorHandlingAnalyzer::finding_deduction(FindingSeverity::High, 0.5,
                                                 1.0),
        hub_high * 0.5);
}

TEST(EhScoringTest, FunctionScoreStartsAtOneAndFloorsAtZero) {
    EXPECT_DOUBLE_EQ(ErrorHandlingAnalyzer::function_score({}, 0.5), 1.0);

    std::vector<EhFinding> one{{EhSignal::EmptyCatch, FindingSeverity::High,
                                0.9, 3, ""}};
    double s = ErrorHandlingAnalyzer::function_score(one, 0.0);
    EXPECT_LT(s, 1.0);
    EXPECT_GT(s, 0.0);

    // A pile of findings cannot push the score below zero.
    std::vector<EhFinding> many(20, {EhSignal::EmptyCatch,
                                     FindingSeverity::High, 1.0, 3, ""});
    EXPECT_DOUBLE_EQ(ErrorHandlingAnalyzer::function_score(many, 1.0), 0.0);
}

TEST(EhScoringTest, MoreFindingsNeverRaiseTheScore) {
    std::vector<EhFinding> one{{EhSignal::BroadCatch, FindingSeverity::Med,
                                0.6, 3, ""}};
    std::vector<EhFinding> two = one;
    two.push_back({EhSignal::LogAndSwallow, FindingSeverity::Med, 0.6, 5, ""});
    EXPECT_LT(ErrorHandlingAnalyzer::function_score(two, 0.5),
              ErrorHandlingAnalyzer::function_score(one, 0.5));
}

}  // namespace
}  // namespace lci
