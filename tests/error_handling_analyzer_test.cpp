// Pure-logic tests for the error-handling / resource scoring model
// (docs/plans/2026-08-17-error-handling-score-design.md).

#include <lci/analysis/error_handling_analyzer.h>

#include <gtest/gtest.h>

namespace lci {
namespace {

TEST(EhProductionPathTest, TestAndVendorPathsAreExcluded) {
    EXPECT_FALSE(ErrorHandlingAnalyzer::is_production_path("foo_test.go"));
    EXPECT_FALSE(ErrorHandlingAnalyzer::is_production_path("tests/helper.py"));
    EXPECT_FALSE(
        ErrorHandlingAnalyzer::is_production_path("vendor/lib/x.go"));
    EXPECT_FALSE(
        ErrorHandlingAnalyzer::is_production_path("node_modules/a/b.js"));
    EXPECT_FALSE(
        ErrorHandlingAnalyzer::is_production_path("examples/demo.rs"));
    EXPECT_FALSE(
        ErrorHandlingAnalyzer::is_production_path("src/foo.spec.ts"));
}

// pocketbase verification: ui/public/libs minified bundles were scoring and
// even owned the worst-module slot — vendored/minified/built output never
// scores (insight-verification lesson: vendor contamination).
TEST(EhProductionPathTest, MinifiedAndBundledAssetsAreExcluded) {
    EXPECT_FALSE(ErrorHandlingAnalyzer::is_production_path(
        "ui/public/libs/uplot/uplot.iife.js"));
    EXPECT_FALSE(
        ErrorHandlingAnalyzer::is_production_path("dist/app.min.js"));
    EXPECT_FALSE(
        ErrorHandlingAnalyzer::is_production_path("ui/public/app.js"));
}

TEST(EhProductionPathTest, PlainSourcePathsAreIncluded) {
    EXPECT_TRUE(ErrorHandlingAnalyzer::is_production_path("src/server.go"));
    EXPECT_TRUE(ErrorHandlingAnalyzer::is_production_path("core/logger.go"));
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
