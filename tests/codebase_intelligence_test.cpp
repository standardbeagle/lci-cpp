#include <gtest/gtest.h>

#include <lci/reference.h>
#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/analysis/ci_vocabulary_analyzer.h>
#include <lci/analysis/health_analyzer.h>
#include <lci/analysis/token_budget.h>

namespace lci {
namespace {

// ===========================================================================
// Threshold constants
// ===========================================================================

TEST(CIThresholds, MatchGoConstants) {
    EXPECT_EQ(ci_thresholds::kComplexityLow, 10);
    EXPECT_EQ(ci_thresholds::kComplexityModerate, 15);
    EXPECT_EQ(ci_thresholds::kComplexityHigh, 20);
    EXPECT_EQ(ci_thresholds::kHotspotComplexity, 10);
    EXPECT_EQ(ci_thresholds::kHotspotLinecount, 50);
    EXPECT_EQ(ci_thresholds::kHighReferenceCount, 10);
    EXPECT_EQ(ci_thresholds::kHighUsage, 5);
    EXPECT_DOUBLE_EQ(ci_thresholds::kRiskScoreMax, 10.0);
}

// ===========================================================================
// Health analyzer - severity ranking
// ===========================================================================

TEST(HealthAnalyzer, SeverityRankHigh) {
    EXPECT_EQ(HealthAnalyzer::severity_rank("high"), 2);
}

TEST(HealthAnalyzer, SeverityRankMedium) {
    EXPECT_EQ(HealthAnalyzer::severity_rank("medium"), 1);
}

TEST(HealthAnalyzer, SeverityRankLow) {
    EXPECT_EQ(HealthAnalyzer::severity_rank("low"), 0);
}

// ===========================================================================
// Health analyzer - maintainability rating
// ===========================================================================

TEST(HealthAnalyzer, MaintainabilityRatingA) {
    EXPECT_EQ(HealthAnalyzer::get_maintainability_rating(85.0), "A");
}

TEST(HealthAnalyzer, MaintainabilityRatingB) {
    EXPECT_EQ(HealthAnalyzer::get_maintainability_rating(75.0), "B");
}

TEST(HealthAnalyzer, MaintainabilityRatingC) {
    EXPECT_EQ(HealthAnalyzer::get_maintainability_rating(65.0), "C");
}

TEST(HealthAnalyzer, MaintainabilityRatingD) {
    EXPECT_EQ(HealthAnalyzer::get_maintainability_rating(55.0), "D");
}

TEST(HealthAnalyzer, MaintainabilityRatingF) {
    EXPECT_EQ(HealthAnalyzer::get_maintainability_rating(40.0), "F");
}

// ===========================================================================
// Health analyzer - debt remediation
// ===========================================================================

TEST(HealthAnalyzer, DebtRemediationLow) {
    EXPECT_EQ(HealthAnalyzer::estimate_debt_remediation_time(0.03), "1 day");
}

TEST(HealthAnalyzer, DebtRemediationWeek) {
    EXPECT_EQ(HealthAnalyzer::estimate_debt_remediation_time(0.08), "1 week");
}

TEST(HealthAnalyzer, DebtRemediationTwoWeeks) {
    EXPECT_EQ(HealthAnalyzer::estimate_debt_remediation_time(0.15), "2 weeks");
}

TEST(HealthAnalyzer, DebtRemediationMonth) {
    EXPECT_EQ(HealthAnalyzer::estimate_debt_remediation_time(0.25), "1 month");
}

TEST(HealthAnalyzer, DebtRemediationMonths) {
    EXPECT_EQ(HealthAnalyzer::estimate_debt_remediation_time(0.50),
              "3+ months");
}

// ===========================================================================
// Health analyzer - overall health score
// ===========================================================================

TEST(HealthAnalyzer, HealthScoreAllLowComplexity) {
    ComplexityMetrics cm;
    cm.average_cc = 3.0;
    cm.distribution["low"] = 100;
    cm.distribution["medium"] = 0;
    cm.distribution["high"] = 0;

    // no penalties -> perfect score (no bonus needed, none applied)
    double score = HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0);
    EXPECT_DOUBLE_EQ(score, 10.0);
}

TEST(HealthAnalyzer, HealthScoreAllHighComplexity) {
    ComplexityMetrics cm;
    cm.average_cc = 25.0;
    cm.distribution["low"] = 0;
    cm.distribution["medium"] = 0;
    cm.distribution["high"] = 100;

    double score = HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0);
    // 10 - (1.0 * 4.0) - ((25-10)*0.15 = 2.25) = 3.75
    EXPECT_NEAR(score, 3.75, 0.01);
}

TEST(HealthAnalyzer, HealthScoreMixedComplexity) {
    ComplexityMetrics cm;
    cm.average_cc = 8.0;
    cm.distribution["low"] = 60;
    cm.distribution["medium"] = 30;
    cm.distribution["high"] = 10;

    double score = HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0);
    // high_ratio=0.1 => -0.4, med_ratio=0.3 => -0.45
    // avg=8 <= 10 => no deduction
    // 10 - 0.4 - 0.45 = 9.15
    EXPECT_NEAR(score, 9.15, 0.01);
}

TEST(HealthAnalyzer, HealthScoreClampedToZero) {
    ComplexityMetrics cm;
    cm.average_cc = 50.0;
    cm.distribution["low"] = 0;
    cm.distribution["medium"] = 0;
    cm.distribution["high"] = 100;

    double score = HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0);
    // 10 - 4.0 - 3.0 (capped) = 3.0
    EXPECT_NEAR(score, 3.0, 0.01);
}

TEST(HealthAnalyzer, HealthScoreEmptyDistribution) {
    ComplexityMetrics cm;
    cm.average_cc = 0.0;
    double score = HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0);
    EXPECT_DOUBLE_EQ(score, 10.0);
}

// ===========================================================================
// Health analyzer - score de-saturation (D3): monotonicity properties and
// criteria-case regressions on synthetic metrics
// ===========================================================================

namespace score_props {

ComplexityMetrics healthy_baseline() {
    ComplexityMetrics cm;
    cm.average_cc = 3.0;
    cm.max_cc = 5.0;
    cm.distribution["low"] = 500;
    cm.distribution["medium"] = 0;
    cm.distribution["high"] = 0;
    return cm;
}

}  // namespace score_props

TEST(HealthAnalyzer, HealthScoreMonotoneDecreasingInMaxCC) {
    double prev = 11.0;
    for (double max_cc : {5.0, 20.0, 30.0, 45.0, 70.0, 90.0}) {
        auto cm = score_props::healthy_baseline();
        cm.max_cc = max_cc;
        double s = HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0);
        EXPECT_LE(s, prev) << "max_cc=" << max_cc;
        prev = s;
    }
    // and strictly below perfect once past the high threshold
    auto cm = score_props::healthy_baseline();
    cm.max_cc = 70.0;
    EXPECT_LT(HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0),
              10.0);
}

TEST(HealthAnalyzer, HealthScoreMonotoneDecreasingInDebtRatio) {
    auto cm = score_props::healthy_baseline();
    double prev = 11.0;
    for (double debt : {0.0, 0.05, 0.1, 0.2, 0.4, 0.8}) {
        double s = HealthAnalyzer::calculate_overall_health_score(cm, debt, 0);
        EXPECT_LE(s, prev) << "debt=" << debt;
        prev = s;
    }
    EXPECT_LT(HealthAnalyzer::calculate_overall_health_score(cm, 0.2, 0),
              HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0));
}

TEST(HealthAnalyzer, HealthScoreMonotoneDecreasingInProblematicCount) {
    auto cm = score_props::healthy_baseline();
    double prev = 11.0;
    for (int count : {0, 1, 3, 5, 7, 20}) {
        double s =
            HealthAnalyzer::calculate_overall_health_score(cm, 0.0, count);
        EXPECT_LE(s, prev) << "count=" << count;
        prev = s;
    }
    EXPECT_LT(HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 5),
              HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0));
}

TEST(HealthAnalyzer, HealthScoreMonotoneDecreasingInHighComplexityCount) {
    double prev = 11.0;
    for (int high : {0, 5, 20, 50, 100}) {
        auto cm = score_props::healthy_baseline();
        cm.distribution["high"] = high;
        double s = HealthAnalyzer::calculate_overall_health_score(cm, 0.0, 0);
        EXPECT_LE(s, prev) << "high=" << high;
        prev = s;
    }
}

TEST(HealthAnalyzer, HealthScorePerfectOnlyWhenClean) {
    // 10.0 requires zero penalty signals; any defect input drops it.
    auto clean = score_props::healthy_baseline();
    EXPECT_DOUBLE_EQ(
        HealthAnalyzer::calculate_overall_health_score(clean, 0.0, 0), 10.0);

    auto dirty = clean;
    dirty.max_cc = 70.0;
    EXPECT_LT(HealthAnalyzer::calculate_overall_health_score(dirty, 0.05, 5),
              10.0);
}

TEST(HealthAnalyzer, HealthScoreDiscriminatesCriteriaRepos) {
    // chi-like: mostly low, one cc~25 outlier, little debt, 2 risk symbols.
    ComplexityMetrics chi;
    chi.average_cc = 3.0;
    chi.max_cc = 25.0;
    chi.distribution["low"] = 700;
    chi.distribution["medium"] = 25;
    chi.distribution["high"] = 5;
    double chi_score =
        HealthAnalyzer::calculate_overall_health_score(chi, 0.02, 2);

    // guzzle-like: cc=70 monster (applyHandlerOptions), 3 high funcs,
    // 5+ risk symbols, real debt.
    ComplexityMetrics guzzle;
    guzzle.average_cc = 4.0;
    guzzle.max_cc = 70.0;
    guzzle.distribution["low"] = 900;
    guzzle.distribution["medium"] = 60;
    guzzle.distribution["high"] = 3;
    double guzzle_score =
        HealthAnalyzer::calculate_overall_health_score(guzzle, 0.06, 5);

    // pocketbase-like: 115-method god object, cc extremes, many risk syms.
    ComplexityMetrics pb;
    pb.average_cc = 4.5;
    pb.max_cc = 60.0;
    pb.distribution["low"] = 3000;
    pb.distribution["medium"] = 250;
    pb.distribution["high"] = 40;
    double pb_score =
        HealthAnalyzer::calculate_overall_health_score(pb, 0.09, 8);

    // The old formula scored all three exactly 10.00. The scale must
    // discriminate: none saturated, ordered by defect load.
    EXPECT_LT(chi_score, 10.0);
    EXPECT_GT(chi_score, guzzle_score);
    EXPECT_GT(guzzle_score, 0.0);
    EXPECT_GT(chi_score, pb_score);
    EXPECT_LT(guzzle_score, 8.0);
    EXPECT_LT(pb_score, 8.0);
}

TEST(HealthAnalyzer, ComplexityFromFilesTracksMaxCC) {
    EnhancedSymbol a, b;
    a.symbol.name = "small";
    a.symbol.type = SymbolType::Function;
    a.complexity = 3;
    b.symbol.name = "applyHandlerOptions";
    b.symbol.type = SymbolType::Function;
    b.complexity = 70;

    FileSymbolData fsd;
    fsd.path = "client.php";
    fsd.symbols = {&a, &b};

    HealthAnalyzer ha;
    auto cm = ha.calculate_complexity_from_files({fsd});
    EXPECT_DOUBLE_EQ(cm.max_cc, 70.0);
}

// ===========================================================================
// Health analyzer - complexity from files
// ===========================================================================

TEST(HealthAnalyzer, ComplexityFromFilesEmpty) {
    HealthAnalyzer ha;
    auto cm = ha.calculate_complexity_from_files({});
    EXPECT_DOUBLE_EQ(cm.average_cc, 0.0);
    EXPECT_DOUBLE_EQ(cm.median_cc, 0.0);
    EXPECT_TRUE(cm.distribution.empty());
}

TEST(HealthAnalyzer, ComplexityFromFilesSingleFunction) {
    EnhancedSymbol sym;
    sym.symbol.name = "foo";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 10;
    sym.complexity = 5;

    FileSymbolData fsd;
    fsd.path = "test.go";
    fsd.symbols.push_back(&sym);

    HealthAnalyzer ha;
    auto cm = ha.calculate_complexity_from_files({fsd});
    EXPECT_DOUBLE_EQ(cm.average_cc, 5.0);
    EXPECT_DOUBLE_EQ(cm.median_cc, 5.0);
    EXPECT_EQ(cm.distribution["low"], 1);
}

TEST(HealthAnalyzer, ComplexityDistributionCategories) {
    EnhancedSymbol low_sym;
    low_sym.symbol.name = "low_func";
    low_sym.symbol.type = SymbolType::Function;
    low_sym.complexity = 5;

    EnhancedSymbol med_sym;
    med_sym.symbol.name = "med_func";
    med_sym.symbol.type = SymbolType::Function;
    med_sym.complexity = 15;

    EnhancedSymbol high_sym;
    high_sym.symbol.name = "high_func";
    high_sym.symbol.type = SymbolType::Function;
    high_sym.complexity = 25;

    FileSymbolData fsd;
    fsd.path = "test.go";
    fsd.symbols = {&low_sym, &med_sym, &high_sym};

    HealthAnalyzer ha;
    auto cm = ha.calculate_complexity_from_files({fsd});
    EXPECT_EQ(cm.distribution["low"], 1);
    EXPECT_EQ(cm.distribution["medium"], 1);
    EXPECT_EQ(cm.distribution["high"], 1);
    EXPECT_DOUBLE_EQ(cm.average_cc, 15.0);
    EXPECT_DOUBLE_EQ(cm.median_cc, 15.0);
}

// ===========================================================================
// Health analyzer - hotspots
// ===========================================================================

TEST(HealthAnalyzer, HotspotsEmpty) {
    HealthAnalyzer ha;
    auto hotspots = ha.identify_hotspots_from_files({});
    EXPECT_TRUE(hotspots.empty());
}

TEST(HealthAnalyzer, HotspotsIdentifiedByComplexity) {
    EnhancedSymbol sym;
    sym.symbol.name = "complex_func";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 10;
    sym.symbol.end_line = 30;
    sym.complexity = 15;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols.push_back(&sym);

    HealthAnalyzer ha;
    auto hotspots = ha.identify_hotspots_from_files({fsd});
    EXPECT_EQ(hotspots.size(), 1u);
    EXPECT_DOUBLE_EQ(hotspots[0].complexity, 15.0);
    EXPECT_GT(hotspots[0].risk_score, 0.0);
}

TEST(HealthAnalyzer, HotspotsIdentifiedByLineCount) {
    EnhancedSymbol sym;
    sym.symbol.name = "long_func";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 100;
    sym.complexity = 3;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols.push_back(&sym);

    HealthAnalyzer ha;
    auto hotspots = ha.identify_hotspots_from_files({fsd});
    EXPECT_EQ(hotspots.size(), 1u);
}

TEST(HealthAnalyzer, HotspotsSkipTestHelpers) {
    EnhancedSymbol sym;
    sym.symbol.name = "setupTestData";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 100;
    sym.complexity = 25;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols.push_back(&sym);

    HealthAnalyzer ha;
    auto hotspots = ha.identify_hotspots_from_files({fsd});
    EXPECT_TRUE(hotspots.empty());
}

TEST(HealthAnalyzer, HotspotsSortedByRiskScore) {
    EnhancedSymbol sym1;
    sym1.symbol.name = "func_a";
    sym1.symbol.type = SymbolType::Function;
    sym1.symbol.line = 1;
    sym1.symbol.end_line = 60;
    sym1.complexity = 12;

    EnhancedSymbol sym2;
    sym2.symbol.name = "func_b";
    sym2.symbol.type = SymbolType::Function;
    sym2.symbol.line = 1;
    sym2.symbol.end_line = 200;
    sym2.complexity = 25;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols = {&sym1, &sym2};

    HealthAnalyzer ha;
    auto hotspots = ha.identify_hotspots_from_files({fsd});
    EXPECT_GE(hotspots.size(), 2u);
    EXPECT_GE(hotspots[0].risk_score, hotspots[1].risk_score);
}

// ===========================================================================
// Health analyzer - technical debt
// ===========================================================================

TEST(HealthAnalyzer, TechDebtRatioEmpty) {
    HealthAnalyzer ha;
    EXPECT_DOUBLE_EQ(ha.calculate_tech_debt_ratio_from_files({}), 0.0);
}

TEST(HealthAnalyzer, TechDebtRatioComputed) {
    EnhancedSymbol clean;
    clean.symbol.name = "clean";
    clean.symbol.type = SymbolType::Function;
    clean.complexity = 5;

    EnhancedSymbol debt;
    debt.symbol.name = "debt";
    debt.symbol.type = SymbolType::Function;
    debt.complexity = 20;  // > kComplexityModerate

    FileSymbolData fsd;
    fsd.path = "test.go";
    fsd.symbols = {&clean, &debt};

    HealthAnalyzer ha;
    double ratio = ha.calculate_tech_debt_ratio_from_files({fsd});
    EXPECT_DOUBLE_EQ(ratio, 0.5);  // 1 out of 2
}

// ===========================================================================
// Health analyzer - code smells
// ===========================================================================

TEST(HealthAnalyzer, CodeSmellLongFunction) {
    EnhancedSymbol sym;
    sym.symbol.name = "big_func";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 120;  // 119 lines > 50
    sym.complexity = 5;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols.push_back(&sym);

    HealthAnalyzer ha;
    auto smells = ha.calculate_detailed_code_smells({fsd});
    ASSERT_GE(smells.size(), 1u);

    bool found = false;
    for (const auto& s : smells) {
        if (s.type == "long-function") {
            found = true;
            EXPECT_EQ(s.severity, "high");
        }
    }
    EXPECT_TRUE(found);
}

TEST(HealthAnalyzer, CodeSmellHighComplexity) {
    EnhancedSymbol sym;
    sym.symbol.name = "complex_func";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 20;
    sym.complexity = 25;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols.push_back(&sym);

    HealthAnalyzer ha;
    auto smells = ha.calculate_detailed_code_smells({fsd});
    bool found = false;
    for (const auto& s : smells) {
        if (s.type == "high-complexity") {
            found = true;
            // 25 > kComplexityHigh (20) but <= 40 -> medium severity
            EXPECT_EQ(s.severity, "medium");
        }
    }
    EXPECT_TRUE(found);
}

TEST(HealthAnalyzer, CodeSmellGodClass) {
    // Parent class
    EnhancedSymbol cls;
    cls.symbol.name = "BigClass";
    cls.symbol.type = SymbolType::Class;
    cls.symbol.line = 1;
    cls.symbol.end_line = 500;
    cls.complexity = 2;

    // Create 20 child methods
    std::vector<EnhancedSymbol> methods(20);
    std::vector<const EnhancedSymbol*> ptrs;
    ptrs.push_back(&cls);
    for (int i = 0; i < 20; i++) {
        methods[i].symbol.name = "method_" + std::to_string(i);
        methods[i].symbol.type = SymbolType::Method;
        methods[i].symbol.line = 10 + i * 20;
        methods[i].symbol.end_line = 10 + i * 20 + 15;
        methods[i].complexity = 3;
        ptrs.push_back(&methods[i]);
    }

    FileSymbolData fsd;
    fsd.path = "big.go";
    fsd.symbols = ptrs;

    HealthAnalyzer ha;
    auto smells = ha.calculate_detailed_code_smells({fsd});
    bool found = false;
    for (const auto& s : smells) {
        if (s.type == "god-class") {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(HealthAnalyzer, SmellsLimitedToMax) {
    std::vector<EnhancedSymbol> syms(20);
    std::vector<const EnhancedSymbol*> ptrs;
    for (int i = 0; i < 20; i++) {
        syms[i].symbol.name = "func_" + std::to_string(i);
        syms[i].symbol.type = SymbolType::Function;
        syms[i].symbol.line = 1;
        syms[i].symbol.end_line = 200;
        syms[i].complexity = 25;
        ptrs.push_back(&syms[i]);
    }

    FileSymbolData fsd;
    fsd.path = "test.go";
    fsd.symbols = ptrs;

    HealthAnalyzer ha;
    // The analyzer returns the FULL set (20 long-function + 20
    // high-complexity) so counts stay truthful; display truncation is the
    // caller's job via sort_and_limit_smells.
    auto smells = ha.calculate_detailed_code_smells({fsd});
    EXPECT_EQ(smells.size(), 40u);
    auto limited = HealthAnalyzer::sort_and_limit_smells(
        std::move(smells), ci_thresholds::kMaxDetailedSmells);
    EXPECT_LE(static_cast<int>(limited.size()),
              ci_thresholds::kMaxDetailedSmells);
}

// ===========================================================================
// Health analyzer - saturation fixes (D3): same-source smell counts,
// symbol-kind gates, empty-name filtering, function-based debt ratio
// ===========================================================================

TEST(HealthAnalyzer, SmellCountsAgreeWithComplexityDistribution) {
    // 8 functions with cc=25: distribution["high"] must equal the
    // high-complexity smell count — one source of truth, no truncation
    // and no divergent thresholds between the two.
    std::vector<EnhancedSymbol> syms(8);
    std::vector<const EnhancedSymbol*> ptrs;
    for (int i = 0; i < 8; i++) {
        syms[i].symbol.name = "func_" + std::to_string(i);
        syms[i].symbol.type = SymbolType::Function;
        syms[i].symbol.line = i * 10 + 1;
        syms[i].symbol.end_line = i * 10 + 5;
        syms[i].complexity = 25;
        ptrs.push_back(&syms[i]);
    }
    FileSymbolData fsd;
    fsd.path = "test.go";
    fsd.symbols = ptrs;

    HealthAnalyzer ha;
    auto cm = ha.calculate_complexity_from_files({fsd});
    ASSERT_EQ(cm.distribution["high"], 8);

    auto smells = ha.calculate_detailed_code_smells({fsd});
    auto counts = HealthAnalyzer::count_smells_by_type(smells);
    EXPECT_EQ(counts["high-complexity"], cm.distribution["high"]);
}

TEST(HealthAnalyzer, LongFunctionSmellOnlyForFunctionsAndMethods) {
    // A trait/class/type declaration spanning many lines is not a long
    // function — kind gate required (guzzle flagged ClientTrait).
    EnhancedSymbol trait_sym;
    trait_sym.symbol.name = "ClientTrait";
    trait_sym.symbol.type = SymbolType::Trait;
    trait_sym.symbol.line = 13;
    trait_sym.symbol.end_line = 400;
    trait_sym.complexity = 1;

    FileSymbolData fsd;
    fsd.path = "ClientTrait.php";
    fsd.symbols.push_back(&trait_sym);

    HealthAnalyzer ha;
    auto smells = ha.calculate_detailed_code_smells({fsd});
    for (const auto& s : smells) {
        EXPECT_NE(s.type, "long-function")
            << "trait declaration flagged as long-function";
    }
}

TEST(HealthAnalyzer, HighFanInSmellNotForFieldsOrTypeDecls) {
    // A struct field with many incoming references is normal data access,
    // not a smell candidate (pocketbase flagged struct fields).
    EnhancedSymbol field_sym;
    field_sym.symbol.name = "Id";
    field_sym.symbol.type = SymbolType::Field;
    field_sym.symbol.line = 5;
    field_sym.symbol.end_line = 5;
    field_sym.complexity = 0;
    field_sym.incoming_ref_count = 50;

    EnhancedSymbol type_sym;
    type_sym.symbol.name = "RecordId";
    type_sym.symbol.type = SymbolType::Type;
    type_sym.symbol.line = 8;
    type_sym.symbol.end_line = 8;
    type_sym.incoming_ref_count = 50;

    FileSymbolData fsd;
    fsd.path = "base.go";
    fsd.symbols = {&field_sym, &type_sym};

    HealthAnalyzer ha;
    auto smells = ha.calculate_detailed_code_smells({fsd});
    for (const auto& s : smells) {
        EXPECT_NE(s.type, "high-fan-in")
            << "field/type declaration flagged as high-fan-in: " << s.symbol;
    }
}

TEST(HealthAnalyzer, ProblematicSymbolsSkipEmptyNames) {
    // Extraction gaps produce empty-name symbols; they are not actionable
    // and must be filtered, not reported (pocketbase had two).
    EnhancedSymbol anon;
    anon.symbol.name = "";
    anon.symbol.type = SymbolType::Function;
    anon.symbol.line = 1;
    anon.symbol.end_line = 300;
    anon.complexity = 70;
    anon.incoming_ref_count = 30;
    anon.outgoing_ref_count = 30;

    FileSymbolData fsd;
    fsd.path = "gen.go";
    fsd.symbols.push_back(&anon);

    HealthAnalyzer ha;
    EXPECT_TRUE(ha.identify_problematic_symbols({fsd}).empty());
    EXPECT_TRUE(ha.calculate_detailed_code_smells({fsd}).empty());
}

TEST(HealthAnalyzer, DebtRatioMeasuresFunctionsNotAllSymbols) {
    // cc=70 repo reported debt=0.00 because thousands of non-function
    // symbols diluted the denominator. Debt is a function-level metric.
    EnhancedSymbol hot;
    hot.symbol.name = "applyHandlerOptions";
    hot.symbol.type = SymbolType::Function;
    hot.symbol.line = 1;
    hot.symbol.end_line = 200;
    hot.complexity = 70;

    std::vector<EnhancedSymbol> vars(10);
    FileSymbolData fsd;
    fsd.path = "client.php";
    fsd.symbols.push_back(&hot);
    for (int i = 0; i < 10; i++) {
        vars[i].symbol.name = "v" + std::to_string(i);
        vars[i].symbol.type = SymbolType::Variable;
        vars[i].complexity = 0;
        fsd.symbols.push_back(&vars[i]);
    }

    HealthAnalyzer ha;
    // 1 debt-carrying function out of 1 function; variables are not
    // in the population.
    EXPECT_DOUBLE_EQ(ha.calculate_tech_debt_ratio_from_files({fsd}), 1.0);
}

// ===========================================================================
// Health analyzer - symbol risk and tags
// ===========================================================================

TEST(HealthAnalyzer, SymbolRiskTags) {
    EnhancedSymbol sym;
    sym.symbol.name = "risky";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 200;
    sym.complexity = 20;
    // Add incoming refs
    sym.incoming_ref_count = static_cast<int>(20);
    sym.outgoing_ref_count = static_cast<int>(20);

    auto [tags, risk] = HealthAnalyzer::calculate_symbol_risk_and_tags(sym);
    EXPECT_EQ(risk, 9);  // 3 + 2 + 2 + 2
    EXPECT_EQ(tags.size(), 4u);
}

TEST(HealthAnalyzer, SymbolRiskMaxIs9) {
    // Max possible: 3 (complexity>15) + 2 (lines>100) + 2 (incoming>15) + 2 (outgoing>15) = 9
    EnhancedSymbol sym;
    sym.symbol.name = "very_risky";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 300;
    sym.complexity = 30;
    sym.incoming_ref_count = static_cast<int>(30);
    sym.outgoing_ref_count = static_cast<int>(30);

    auto [tags, risk] = HealthAnalyzer::calculate_symbol_risk_and_tags(sym);
    EXPECT_EQ(risk, 9);
    EXPECT_EQ(tags.size(), 4u);
}

// ===========================================================================
// Health analyzer - quality from complexity
// ===========================================================================

TEST(HealthAnalyzer, QualityFromComplexityLow) {
    ComplexityMetrics cm;
    cm.average_cc = 5.0;
    cm.distribution["low"] = 80;
    cm.distribution["high"] = 0;

    auto qm = HealthAnalyzer::calculate_quality_from_complexity(cm);
    EXPECT_DOUBLE_EQ(qm.maintainability_index, 90.0);
    EXPECT_DOUBLE_EQ(qm.technical_debt_ratio, 0.0);
}

TEST(HealthAnalyzer, QualityFromComplexityHigh) {
    ComplexityMetrics cm;
    cm.average_cc = 50.0;
    cm.distribution["low"] = 0;
    cm.distribution["high"] = 10;

    auto qm = HealthAnalyzer::calculate_quality_from_complexity(cm);
    EXPECT_DOUBLE_EQ(qm.maintainability_index, 0.0);
    EXPECT_DOUBLE_EQ(qm.technical_debt_ratio, 1.0);
}

// ===========================================================================
// Health analyzer - count smells by type
// ===========================================================================

TEST(HealthAnalyzer, CountSmellsByType) {
    std::vector<CodeSmellEntry> smells = {
        {"long-function", "a", "", "", "high", ""},
        {"long-function", "b", "", "", "medium", ""},
        {"high-complexity", "c", "", "", "high", ""},
    };
    auto counts = HealthAnalyzer::count_smells_by_type(smells);
    EXPECT_EQ(counts["long-function"], 2);
    EXPECT_EQ(counts["high-complexity"], 1);
}

// ===========================================================================
// CI Vocabulary Analyzer
// ===========================================================================

TEST(CIVocabularyAnalyzer, ClassifyAuthTermExact) {
    CIVocabularyAnalyzer va;
    auto [domain, strength] = va.classify_term_with_strength("auth");
    EXPECT_EQ(domain, "Authentication");
    EXPECT_DOUBLE_EQ(strength, 1.0);
}

TEST(CIVocabularyAnalyzer, ClassifyDatabaseTerm) {
    CIVocabularyAnalyzer va;
    EXPECT_EQ(va.classify_term("database"), "Database");
}

TEST(CIVocabularyAnalyzer, ClassifyHTTPTerm) {
    CIVocabularyAnalyzer va;
    EXPECT_EQ(va.classify_term("handler"), "HTTP/API");
}

TEST(CIVocabularyAnalyzer, ClassifyParsingTerm) {
    CIVocabularyAnalyzer va;
    EXPECT_EQ(va.classify_term("parser"), "Parsing");
}

TEST(CIVocabularyAnalyzer, ClassifyUnknownTerm) {
    CIVocabularyAnalyzer va;
    auto [domain, strength] = va.classify_term_with_strength("xyzzy");
    EXPECT_TRUE(domain.empty());
    EXPECT_DOUBLE_EQ(strength, 0.0);
}

TEST(CIVocabularyAnalyzer, ClassifyContainedTerm) {
    CIVocabularyAnalyzer va;
    auto [domain, strength] = va.classify_term_with_strength("authHandler");
    EXPECT_FALSE(domain.empty());
    EXPECT_GT(strength, 0.0);
}

TEST(CIVocabularyAnalyzer, DomainConfidenceBasic) {
    double conf = CIVocabularyAnalyzer::calculate_domain_confidence(
        1.0, 5, 20, 100);
    EXPECT_GE(conf, 0.1);
    EXPECT_LE(conf, 1.0);
}

TEST(CIVocabularyAnalyzer, DomainConfidenceMinimum) {
    double conf = CIVocabularyAnalyzer::calculate_domain_confidence(
        0.0, 0, 0, 0);
    EXPECT_DOUBLE_EQ(conf, 0.1);
}

TEST(CIVocabularyAnalyzer, ExtractDomainTermsFromFiles) {
    EnhancedSymbol auth_sym;
    auth_sym.symbol.name = "auth";
    auth_sym.symbol.type = SymbolType::Function;

    EnhancedSymbol db_sym;
    db_sym.symbol.name = "database";
    db_sym.symbol.type = SymbolType::Function;

    EnhancedSymbol unknown_sym;
    unknown_sym.symbol.name = "xyzzy";
    unknown_sym.symbol.type = SymbolType::Function;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols = {&auth_sym, &db_sym, &unknown_sym};

    CIVocabularyAnalyzer va;
    auto terms = va.extract_domain_terms_from_files({fsd});
    EXPECT_GE(terms.size(), 2u);  // auth + database

    bool found_auth = false;
    bool found_db = false;
    for (const auto& dt : terms) {
        if (dt.domain == "Authentication") found_auth = true;
        if (dt.domain == "Database") found_db = true;
    }
    EXPECT_TRUE(found_auth);
    EXPECT_TRUE(found_db);
}

// ===========================================================================
// Token budget manager
// ===========================================================================

TEST(TokenBudgetManager, DefaultBudget) {
    EXPECT_EQ(TokenBudgetManager::calculate_target_budget(nullptr), 8000);
}

TEST(TokenBudgetManager, ScaledBudget) {
    int max_results = 100;
    int budget = TokenBudgetManager::calculate_target_budget(&max_results);
    EXPECT_EQ(budget, 12000);  // 100/50 * 8000 = 16000, capped at 12000
}

TEST(TokenBudgetManager, MinBudget) {
    int max_results = 10;
    int budget = TokenBudgetManager::calculate_target_budget(&max_results);
    EXPECT_EQ(budget, 4000);  // 10/50 * 8000 = 1600, clamped to 4000
}

TEST(TokenBudgetManager, ZeroMaxResults) {
    int max_results = 0;
    EXPECT_EQ(TokenBudgetManager::calculate_target_budget(&max_results), 8000);
}

TEST(TokenBudgetManager, EstimateEmptyResponse) {
    CodebaseIntelligenceResponse response;
    int tokens = TokenBudgetManager::estimate_response_tokens(response);
    EXPECT_EQ(tokens, 200);  // metadata only
}

TEST(TokenBudgetManager, EstimateWithRepositoryMap) {
    RepositoryMap map;
    map.critical_functions.resize(10);
    map.module_boundaries.resize(5);

    CodebaseIntelligenceResponse response;
    response.repository_map = &map;

    int tokens = TokenBudgetManager::estimate_response_tokens(response);
    // 50 + 10*100 + 5*80 + 0 + 0 + 200 = 1650
    EXPECT_EQ(tokens, 1650);
}

TEST(TokenBudgetManager, EstimateWithHealthDashboard) {
    HealthDashboard health;
    health.hotspots.resize(5);

    CodebaseIntelligenceResponse response;
    response.health_dashboard = &health;

    int tokens = TokenBudgetManager::estimate_response_tokens(response);
    // 100 + 200 + 5*100 + 200 = 1000
    EXPECT_EQ(tokens, 1000);
}

TEST(TokenBudgetManager, EnforceUnderBudget) {
    CodebaseIntelligenceResponse response;
    TokenBudgetManager::enforce_budget(response, nullptr);
    // No truncation needed, should not change anything
}

TEST(TokenBudgetManager, TruncateReducesHotspots) {
    HealthDashboard health;
    health.hotspots.resize(50);

    CodebaseIntelligenceResponse response;
    response.health_dashboard = &health;

    // Force truncation to small budget
    TokenBudgetManager::truncate_to_budget(response, 500);
    EXPECT_LE(health.hotspots.size(), 10u);
}

TEST(TokenBudgetManager, EmergencyTruncation) {
    RepositoryMap map;
    map.critical_functions.resize(100);
    map.module_boundaries.resize(50);
    map.domain_terms.resize(30);
    map.entry_points.resize(20);

    HealthDashboard health;
    health.hotspots.resize(50);

    CodebaseIntelligenceResponse response;
    response.repository_map = &map;
    response.health_dashboard = &health;

    TokenBudgetManager::truncate_to_budget(response, 100);

    // Emergency truncation should clear secondary data
    EXPECT_LE(map.critical_functions.size(), 5u);
    EXPECT_TRUE(map.module_boundaries.empty());
    EXPECT_TRUE(map.domain_terms.empty());
    EXPECT_TRUE(map.entry_points.empty());
    EXPECT_LE(health.hotspots.size(), 3u);
}

// ===========================================================================
// CodebaseIntelligenceEngine - mode validation
// ===========================================================================

TEST(CIEngine, ValidModes) {
    EXPECT_TRUE(CodebaseIntelligenceEngine::is_valid_mode("overview"));
    EXPECT_TRUE(CodebaseIntelligenceEngine::is_valid_mode("detailed"));
    EXPECT_TRUE(CodebaseIntelligenceEngine::is_valid_mode("statistics"));
    EXPECT_TRUE(CodebaseIntelligenceEngine::is_valid_mode("unified"));
    EXPECT_TRUE(CodebaseIntelligenceEngine::is_valid_mode("structure"));
    EXPECT_TRUE(CodebaseIntelligenceEngine::is_valid_mode("git_analyze"));
    EXPECT_TRUE(CodebaseIntelligenceEngine::is_valid_mode("git_hotspots"));
}

TEST(CIEngine, InvalidMode) {
    EXPECT_FALSE(CodebaseIntelligenceEngine::is_valid_mode(""));
    EXPECT_FALSE(CodebaseIntelligenceEngine::is_valid_mode("unknown"));
    EXPECT_FALSE(CodebaseIntelligenceEngine::is_valid_mode("type_hierarchy"));
}

// ===========================================================================
// CodebaseIntelligenceEngine - analyze dispatch
// ===========================================================================

TEST(CIEngine, AnalyzeRejectsInvalidMode) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "invalid_mode";

    EnhancedSymbol sym;
    sym.symbol.name = "main";
    sym.symbol.type = SymbolType::Function;
    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("invalid mode"), std::string::npos);
}

TEST(CIEngine, AnalyzeRejectsEmptyFiles) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";

    auto result = engine.analyze(params, {}, 0, 0);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("no files"), std::string::npos);
}

TEST(CIEngine, AnalyzeDefaultsToOverview) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;  // mode is empty

    EnhancedSymbol sym;
    sym.symbol.name = "foo";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 10;
    sym.complexity = 3;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.response.analysis_mode, "overview");
}

TEST(CIEngine, AnalyzeSetsMetadata) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";
    params.tier = 2;

    EnhancedSymbol sym;
    sym.symbol.name = "foo";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 5;
    sym.complexity = 1;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 10, 50);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.response.tier, 2);
    EXPECT_EQ(result.response.analysis_metadata.files_analyzed, 10);
    EXPECT_EQ(result.response.analysis_metadata.index_version, "1.0");
    EXPECT_GE(result.response.analysis_metadata.analysis_time_ms, 0);
}

TEST(CIEngine, AnalyzeClampsTier) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";
    params.tier = 99;

    EnhancedSymbol sym;
    sym.symbol.name = "x";
    sym.symbol.type = SymbolType::Function;
    FileSymbolData fsd;
    fsd.path = "a.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.response.tier, 1);  // clamped to default
}

// ===========================================================================
// CodebaseIntelligenceEngine - overview mode
// ===========================================================================

TEST(CIEngine, OverviewIncludesAllSections) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";

    EnhancedSymbol sym;
    sym.symbol.name = "main";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 20;
    sym.complexity = 5;
    sym.is_exported = false;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_TRUE(result.ok());
    EXPECT_NE(result.response.repository_map, nullptr);
    EXPECT_NE(result.response.health_dashboard, nullptr);
    EXPECT_NE(result.response.entry_points, nullptr);
}

TEST(CIEngine, OverviewSelectiveInclude) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";
    params.include.repository_map = true;
    params.include.health_dashboard = false;
    params.include.entry_points = false;

    EnhancedSymbol sym;
    sym.symbol.name = "foo";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 5;
    sym.complexity = 1;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_TRUE(result.ok());
    EXPECT_NE(result.response.repository_map, nullptr);
    EXPECT_EQ(result.response.health_dashboard, nullptr);
    EXPECT_EQ(result.response.entry_points, nullptr);
}

TEST(CIEngine, OverviewCriticalFunctions) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";
    params.include.repository_map = true;
    params.include.health_dashboard = false;
    params.include.entry_points = false;

    EnhancedSymbol exported_func;
    exported_func.symbol.name = "HandleRequest";
    exported_func.symbol.type = SymbolType::Function;
    exported_func.symbol.line = 1;
    exported_func.symbol.end_line = 30;
    exported_func.complexity = 10;
    exported_func.is_exported = true;
    exported_func.incoming_ref_count = static_cast<int>(5);

    EnhancedSymbol private_func;
    private_func.symbol.name = "helper";
    private_func.symbol.type = SymbolType::Function;
    private_func.symbol.line = 31;
    private_func.symbol.end_line = 40;
    private_func.complexity = 2;
    private_func.is_exported = false;

    FileSymbolData fsd;
    fsd.path = "server.go";
    fsd.symbols = {&exported_func, &private_func};

    auto result = engine.analyze(params, {fsd}, 1, 2);
    EXPECT_TRUE(result.ok());

    const auto& funcs = result.response.repository_map->critical_functions;
    EXPECT_GE(funcs.size(), 1u);
    EXPECT_EQ(funcs[0].name, "HandleRequest");
    EXPECT_GT(funcs[0].importance_score, 0.0);
}

TEST(CIEngine, OverviewHealthDashboard) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";
    params.include.repository_map = false;
    params.include.health_dashboard = true;
    params.include.entry_points = false;

    EnhancedSymbol complex_func;
    complex_func.symbol.name = "process";
    complex_func.symbol.type = SymbolType::Function;
    complex_func.symbol.line = 1;
    complex_func.symbol.end_line = 100;
    complex_func.complexity = 25;

    FileSymbolData fsd;
    fsd.path = "worker.go";
    fsd.symbols = {&complex_func};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_TRUE(result.ok());

    auto* health = result.response.health_dashboard;
    EXPECT_NE(health, nullptr);
    EXPECT_GT(health->complexity.average_cc, 0.0);
    EXPECT_FALSE(health->hotspots.empty());
    EXPECT_GT(health->overall_score, 0.0);
    EXPECT_LE(health->overall_score, 10.0);
}

TEST(CIEngine, OverviewEntryPoints) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";
    params.include.repository_map = false;
    params.include.health_dashboard = false;
    params.include.entry_points = true;

    EnhancedSymbol main_func;
    main_func.symbol.name = "main";
    main_func.symbol.type = SymbolType::Function;
    main_func.symbol.line = 1;
    main_func.symbol.end_line = 10;
    main_func.complexity = 2;

    EnhancedSymbol exported_func;
    exported_func.symbol.name = "Serve";
    exported_func.symbol.type = SymbolType::Function;
    exported_func.symbol.line = 20;
    exported_func.symbol.end_line = 40;
    exported_func.is_exported = true;

    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols = {&main_func, &exported_func};

    auto result = engine.analyze(params, {fsd}, 1, 2);
    EXPECT_TRUE(result.ok());

    auto* ep = result.response.entry_points;
    EXPECT_NE(ep, nullptr);
    EXPECT_GE(ep->main_functions.size(), 1u);

    bool found_main = false;
    bool found_api = false;
    for (const auto& e : ep->main_functions) {
        if (e.name == "main") found_main = true;
        if (e.name == "Serve" && e.type == "api") found_api = true;
    }
    EXPECT_TRUE(found_main);
    EXPECT_TRUE(found_api);
}

// ===========================================================================
// CodebaseIntelligenceEngine - other modes dispatch correctly
// ===========================================================================

// analyze() operates on pre-collected file/symbol data with no live index, so
// it cannot supply the call-graph / project-root / file-path inputs that the
// detailed (features), statistics and structure builders need. Rather than
// emit silently-degraded sections (skipped feature clustering, an empty
// directory tree), it fails fast and directs callers to the index-backed path
// (build_detailed/build_statistics/build_structure with explicit inputs, as
// the MCP handler calls them). These modes stay valid modes; they are just not
// reachable through the index-less analyze() entry point.
TEST(CIEngine, DetailedModeRequiresIndexBackedPath) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "detailed";
    params.analysis = "modules";

    EnhancedSymbol sym;
    sym.symbol.name = "foo";
    sym.symbol.type = SymbolType::Function;
    FileSymbolData fsd;
    fsd.path = "a.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("index-backed"), std::string::npos);
}

TEST(CIEngine, StatisticsModeRequiresIndexBackedPath) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "statistics";

    EnhancedSymbol sym;
    sym.symbol.name = "compute";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 10;
    sym.complexity = 5;
    FileSymbolData fsd;
    fsd.path = "math.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("index-backed"), std::string::npos);
}

TEST(CIEngine, UnifiedModeIncludesOverview) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "unified";

    EnhancedSymbol sym;
    sym.symbol.name = "main";
    sym.symbol.type = SymbolType::Function;
    sym.symbol.line = 1;
    sym.symbol.end_line = 10;
    sym.complexity = 3;
    FileSymbolData fsd;
    fsd.path = "main.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.response.analysis_mode, "unified");
    EXPECT_NE(result.response.repository_map, nullptr);
    EXPECT_NE(result.response.health_dashboard, nullptr);
    EXPECT_NE(result.response.entry_points, nullptr);
}

TEST(CIEngine, StructureModeRequiresIndexBackedPath) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "structure";

    EnhancedSymbol sym;
    sym.symbol.name = "bar";
    sym.symbol.type = SymbolType::Function;
    FileSymbolData fsd;
    fsd.path = "lib.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("index-backed"), std::string::npos);
}

// The index-backed builders (called by the MCP handler with explicit inputs)
// must produce complete sections — this is what the removed defaults used to
// silently degrade. Guard the two degradation cases the finding named.
TEST(CIEngine, BuildDetailedFeaturesPopulatedWithCallGraph) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "detailed";
    params.analysis = "features";

    EnhancedSymbol a;
    a.symbol.name = "Login";
    a.symbol.type = SymbolType::Function;
    a.id = 1;
    EnhancedSymbol b;
    b.symbol.name = "Register";
    b.symbol.type = SymbolType::Function;
    b.id = 2;
    EnhancedSymbol c;
    c.symbol.name = "Validate";
    c.symbol.type = SymbolType::Function;
    c.id = 3;
    FileSymbolData fsd;
    fsd.path = "src/auth/auth.go";
    fsd.symbols = {&a, &b, &c};

    // A connected triangle so feature clustering has real edges to work on.
    auto callees_of = [](SymbolID id) -> std::vector<SymbolID> {
        switch (id) {
            case 1: return {2};
            case 2: return {3};
            case 3: return {1};
            default: return {};
        }
    };

    auto resp = engine.build_detailed(params, {fsd}, "", callees_of);
    // With a call graph, feature analysis is run, not silently skipped.
    ASSERT_TRUE(resp.feature_analysis.has_value());
    EXPECT_GE(resp.feature_analysis->metrics.total_features, 1);
}

TEST(CIEngine, BuildStructurePopulatedWithFilePaths) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "structure";

    EnhancedSymbol sym;
    sym.symbol.name = "bar";
    sym.symbol.type = SymbolType::Function;
    FileSymbolData fsd;
    fsd.path = "src/lib.go";
    fsd.symbols = {&sym};

    std::vector<std::string> file_paths = {"/proj/src/lib.go",
                                           "/proj/src/util.go",
                                           "/proj/cmd/main.go"};

    auto resp = engine.build_structure(params, {fsd}, file_paths, {}, "/proj");
    // With real file paths the tree is populated, not an empty dirs=0 shell.
    ASSERT_TRUE(resp.structure_analysis.has_value());
    EXPECT_EQ(resp.structure_analysis->file_count, 3);
    EXPECT_FALSE(resp.structure_analysis->top_dirs.empty());
}

// build_structure must categorize files through the canonical classify_file
// rule (1:1 FileCategory mapping), NOT loose substring matching. Regression
// guard for the review finding: the old code used rel.find("/test"), which
// wrongly counted any path under a "/testing/" directory as a test, and
// rel.find(".md") which matched a mid-path ".md". A file under "/testing/"
// with no test basename marker is source code, not a test.
TEST(CIEngine, BuildStructureCategorizesViaClassifyFile) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "structure";

    std::vector<std::string> file_paths = {
        "/proj/src/testing/helper.cpp",  // /testing/ dir -> code, NOT test
        "/proj/src/widget_test.cpp",     // _test. basename -> test
        "/proj/docs/guide.md",           // .md extension -> docs
        "/proj/config/settings.json",    // .json extension -> config
    };

    auto resp = engine.build_structure(params, {}, file_paths, {}, "/proj");
    ASSERT_TRUE(resp.structure_analysis.has_value());
    const auto& s = *resp.structure_analysis;

    // The "/testing/" file is source, so exactly one real test file counts.
    EXPECT_EQ(s.tests, 1);
    // The "/testing/" file lands in code, giving one code file.
    EXPECT_EQ(s.code, 1);
    EXPECT_EQ(s.docs, 1);
    EXPECT_EQ(s.config, 1);
}

// Go parity: extension-less files (bare README, LICENSE, Makefile, Dockerfile)
// are FileCategoryUnknown and Go's categorizeFile
// (internal/mcp/codebase_intelligence_tools.go:846) routes them to the distinct
// "other" bucket (FileCategories.Other, json:"other" — types.go:745), NOT to
// "code" and NOT to "doc". Regression guard: build_structure previously folded
// FileCategory::Unknown into the code count, inflating it. The old C++
// rel.find("README") -> docs rule was a non-Go invention and must NOT return.
TEST(CIEngine, BuildStructureRoutesUnknownToOther) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "structure";

    std::vector<std::string> file_paths = {
        "/proj/README",             // no extension -> Unknown -> other
        "/proj/LICENSE",            // no extension -> Unknown -> other
        "/proj/Makefile",           // no extension -> Unknown -> other
        "/proj/src/lib.cpp",        // .cpp -> code
    };

    auto resp = engine.build_structure(params, {}, file_paths, {}, "/proj");
    ASSERT_TRUE(resp.structure_analysis.has_value());
    const auto& s = *resp.structure_analysis;

    // The three extension-less files land in "other", matching Go's default
    // categorizeFile bucket; only the real source file counts as code.
    EXPECT_EQ(s.other, 3);
    EXPECT_EQ(s.code, 1);
    // Guard against re-inventing the removed README->docs rule.
    EXPECT_EQ(s.docs, 0);
}

// ===========================================================================
// D4 — single count census across modes (repo-qa ANALYSIS-insight-verification)
// ===========================================================================

// Structure's `dirs=` must be the FULL directory census (root plus every
// distinct ancestor directory of an indexed file), not just the number of
// top-level path segments. On real corpora the top-level-only figure
// understated wildly (chi: 4 vs ~21 real dirs; pocketbase: 12 vs 174).
TEST(CIEngine, StructureDirCountIsFullDirectoryCensus) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "structure";

    std::vector<std::string> file_paths = {
        "/proj/a.go",                 // dir: .
        "/proj/src/core/x.go",        // dirs: src, src/core
        "/proj/src/core/db/y.go",     // dir:  src/core/db
        "/proj/src/util.go",          //       (src already seen)
        "/proj/cmd/main.go",          // dir:  cmd
    };
    auto resp = engine.build_structure(params, {}, file_paths, {}, "/proj");
    ASSERT_TRUE(resp.structure_analysis.has_value());
    // Census: ".", "src", "src/core", "src/core/db", "cmd" -> 5 directories.
    EXPECT_EQ(resp.structure_analysis->dir_count, 5);
    EXPECT_EQ(resp.structure_analysis->file_count, 5);
    EXPECT_EQ(resp.structure_analysis->max_depth, 3);
}

// Structure's `symbols=` and overview's repository-map `total_symbols` must
// agree: both are the count of ALL symbols in the corpus, derived from the
// same file/symbol data. The old wiring fed structure a functions-only count
// (guzzle: structure symbols=16 vs unified symbols=1205) and even that count
// excluded methods.
TEST(CIEngine, StructureAndOverviewAgreeOnSymbolCount) {
    CodebaseIntelligenceEngine engine;

    EnhancedSymbol fn;
    fn.symbol.name = "Handle";
    fn.symbol.type = SymbolType::Function;
    EnhancedSymbol method;
    method.symbol.name = "Serve";
    method.symbol.type = SymbolType::Method;
    EnhancedSymbol cls;
    cls.symbol.name = "Server";
    cls.symbol.type = SymbolType::Class;
    FileSymbolData fsd;
    fsd.path = "src/server.go";
    fsd.symbols = {&fn, &method, &cls};
    std::vector<FileSymbolData> files = {fsd};

    CodebaseIntelligenceParams op;
    op.mode = "overview";
    op.include.repository_map = true;
    auto overview = engine.build_overview(op, files, /*file_count=*/1,
                                          /*symbol_count=*/0);
    ASSERT_NE(overview.repository_map, nullptr);

    CodebaseIntelligenceParams sp;
    sp.mode = "structure";
    std::vector<std::string> file_paths = {"/proj/src/server.go"};
    // The handler's old wiring passed a functions-only count here (1: it even
    // excluded the method). build_structure must not trust it — the symbol
    // census comes from `files`, the same source overview counts from.
    auto structure = engine.build_structure(sp, files, file_paths, {}, "/proj");
    ASSERT_TRUE(structure.structure_analysis.has_value());

    EXPECT_EQ(overview.repository_map->total_symbols, 3);
    EXPECT_EQ(structure.structure_analysis->symbol_count,
              overview.repository_map->total_symbols);
    // The function census stays available, explicitly labeled, and counts
    // methods as functions (is_function_like), matching overview's
    // total_functions.
    EXPECT_EQ(structure.structure_analysis->function_count, 2);
    EXPECT_EQ(structure.structure_analysis->function_count,
              overview.repository_map->total_functions);
}

TEST(CIEngine, GitAnalyzeModeDispatch) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "git_analyze";

    EnhancedSymbol sym;
    sym.symbol.name = "x";
    sym.symbol.type = SymbolType::Function;
    FileSymbolData fsd;
    fsd.path = "a.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.response.analysis_mode, "git_analyze");
}

TEST(CIEngine, GitHotspotsModeDispatch) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "git_hotspots";

    EnhancedSymbol sym;
    sym.symbol.name = "x";
    sym.symbol.type = SymbolType::Function;
    FileSymbolData fsd;
    fsd.path = "a.go";
    fsd.symbols = {&sym};

    auto result = engine.analyze(params, {fsd}, 1, 1);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.response.analysis_mode, "git_hotspots");
}

// ===========================================================================
// CodebaseIntelligenceEngine - importance score
// ===========================================================================

TEST(CIEngine, ImportanceScoreExportedBoost) {
    EnhancedSymbol exported_sym;
    exported_sym.symbol.name = "Process";
    exported_sym.symbol.type = SymbolType::Function;
    exported_sym.is_exported = true;
    exported_sym.incoming_ref_count = static_cast<int>(10);

    EnhancedSymbol private_sym;
    private_sym.symbol.name = "process";
    private_sym.symbol.type = SymbolType::Function;
    private_sym.is_exported = false;
    private_sym.incoming_ref_count = static_cast<int>(10);

    double exported_score =
        CodebaseIntelligenceEngine::calculate_importance_score(exported_sym);
    double private_score =
        CodebaseIntelligenceEngine::calculate_importance_score(private_sym);
    EXPECT_GT(exported_score, private_score);
}

TEST(CIEngine, ImportanceScoreMainBoost) {
    EnhancedSymbol main_sym;
    main_sym.symbol.name = "main";
    main_sym.symbol.type = SymbolType::Function;
    main_sym.incoming_ref_count = static_cast<int>(1);

    EnhancedSymbol other_sym;
    other_sym.symbol.name = "helper";
    other_sym.symbol.type = SymbolType::Function;
    other_sym.incoming_ref_count = static_cast<int>(1);

    double main_score =
        CodebaseIntelligenceEngine::calculate_importance_score(main_sym);
    double other_score =
        CodebaseIntelligenceEngine::calculate_importance_score(other_sym);
    EXPECT_GT(main_score, other_score);
}

TEST(CIEngine, ImportanceScoreHandlerBoost) {
    EnhancedSymbol handler_sym;
    handler_sym.symbol.name = "handleRequest";
    handler_sym.symbol.type = SymbolType::Function;
    handler_sym.incoming_ref_count = static_cast<int>(5);

    EnhancedSymbol plain_sym;
    plain_sym.symbol.name = "compute";
    plain_sym.symbol.type = SymbolType::Function;
    plain_sym.incoming_ref_count = static_cast<int>(5);

    double handler_score =
        CodebaseIntelligenceEngine::calculate_importance_score(handler_sym);
    double plain_score =
        CodebaseIntelligenceEngine::calculate_importance_score(plain_sym);
    EXPECT_GT(handler_score, plain_score);
}

TEST(CIEngine, ImportanceScoreComplexityBoost) {
    EnhancedSymbol complex_sym;
    complex_sym.symbol.name = "work";
    complex_sym.symbol.type = SymbolType::Function;
    complex_sym.incoming_ref_count = static_cast<int>(5);
    complex_sym.complexity = 15;

    EnhancedSymbol simple_sym;
    simple_sym.symbol.name = "work2";
    simple_sym.symbol.type = SymbolType::Function;
    simple_sym.incoming_ref_count = static_cast<int>(5);
    simple_sym.complexity = 0;

    double complex_score =
        CodebaseIntelligenceEngine::calculate_importance_score(complex_sym);
    double simple_score =
        CodebaseIntelligenceEngine::calculate_importance_score(simple_sym);
    EXPECT_GT(complex_score, simple_score);
}

TEST(CIEngine, ImportanceScoreZeroRefs) {
    EnhancedSymbol sym;
    sym.symbol.name = "unused";
    sym.symbol.type = SymbolType::Function;

    double score =
        CodebaseIntelligenceEngine::calculate_importance_score(sym);
    EXPECT_DOUBLE_EQ(score, 0.0);
}

// ===========================================================================
// CodebaseIntelligenceEngine - token budget enforcement
// ===========================================================================

TEST(CIEngine, AnalyzeEnforcesBudget) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";
    params.include.repository_map = false;
    params.include.health_dashboard = true;
    params.include.entry_points = false;
    params.max_results = 10;  // small budget

    // Create many complex functions to generate lots of hotspots
    std::vector<EnhancedSymbol> syms(100);
    std::vector<const EnhancedSymbol*> ptrs;
    for (int i = 0; i < 100; ++i) {
        syms[i].symbol.name = "func_" + std::to_string(i);
        syms[i].symbol.type = SymbolType::Function;
        syms[i].symbol.line = 1;
        syms[i].symbol.end_line = 200;
        syms[i].complexity = 30;
        ptrs.push_back(&syms[i]);
    }

    FileSymbolData fsd;
    fsd.path = "big.go";
    fsd.symbols = ptrs;

    auto result = engine.analyze(params, {fsd}, 1, 100);
    EXPECT_TRUE(result.ok());
    // Budget enforcement should have limited the response
    auto* health = result.response.health_dashboard;
    EXPECT_NE(health, nullptr);
}

// ===========================================================================
// CodebaseIntelligenceEngine - entry point API limit
// ===========================================================================

TEST(CIEngine, EntryPointsCollectedAndRanked) {
    CodebaseIntelligenceEngine engine;
    CodebaseIntelligenceParams params;
    params.mode = "overview";
    params.include.repository_map = false;
    params.include.health_dashboard = false;
    params.include.entry_points = true;

    // 20 exported functions + a main(). The engine now collects ALL entry
    // points and ranks them (main first, then importance desc); the top-N
    // display cap lives in the LCF emitter, not the engine.
    std::vector<EnhancedSymbol> syms(20);
    std::vector<const EnhancedSymbol*> ptrs;
    for (int i = 0; i < 20; ++i) {
        syms[i].symbol.name = "Handler" + std::to_string(i);
        syms[i].symbol.type = SymbolType::Function;
        syms[i].symbol.line = i * 10 + 1;
        syms[i].symbol.end_line = i * 10 + 9;
        syms[i].is_exported = true;
        ptrs.push_back(&syms[i]);
    }
    EnhancedSymbol main_sym;
    main_sym.symbol.name = "main";
    main_sym.symbol.type = SymbolType::Function;
    main_sym.symbol.line = 1;
    ptrs.push_back(&main_sym);

    FileSymbolData fsd;
    fsd.path = "handlers.go";
    fsd.symbols = ptrs;

    auto result = engine.analyze(params, {fsd}, 1, 21);
    EXPECT_TRUE(result.ok());

    const auto& eps = result.response.entry_points->main_functions;
    EXPECT_EQ(eps.size(), 21u);  // all collected, not capped at the engine
    ASSERT_FALSE(eps.empty());
    EXPECT_EQ(eps.front().type, "main");  // main ranked first
}

}  // namespace
}  // namespace lci
