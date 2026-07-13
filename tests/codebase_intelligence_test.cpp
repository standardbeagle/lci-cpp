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

    // score starts at 10, low_ratio=1.0 > 0.8 so +1.0, capped at 10
    double score = HealthAnalyzer::calculate_overall_health_score(cm, 10);
    EXPECT_DOUBLE_EQ(score, 10.0);
}

TEST(HealthAnalyzer, HealthScoreAllHighComplexity) {
    ComplexityMetrics cm;
    cm.average_cc = 25.0;
    cm.distribution["low"] = 0;
    cm.distribution["medium"] = 0;
    cm.distribution["high"] = 100;

    double score = HealthAnalyzer::calculate_overall_health_score(cm, 10);
    // 10 - (1.0 * 4.0) - ((25-10)*0.15 = 2.25) = 3.75
    EXPECT_NEAR(score, 3.75, 0.01);
}

TEST(HealthAnalyzer, HealthScoreMixedComplexity) {
    ComplexityMetrics cm;
    cm.average_cc = 8.0;
    cm.distribution["low"] = 60;
    cm.distribution["medium"] = 30;
    cm.distribution["high"] = 10;

    double score = HealthAnalyzer::calculate_overall_health_score(cm, 10);
    // high_ratio=0.1 => -0.4, med_ratio=0.3 => -0.45
    // avg=8 <= 10 => no deduction
    // low_ratio=0.6 not strictly > 0.6 => no bonus
    // 10 - 0.4 - 0.45 = 9.15
    EXPECT_NEAR(score, 9.15, 0.01);
}

TEST(HealthAnalyzer, HealthScoreClampedToZero) {
    ComplexityMetrics cm;
    cm.average_cc = 50.0;
    cm.distribution["low"] = 0;
    cm.distribution["medium"] = 0;
    cm.distribution["high"] = 100;

    double score = HealthAnalyzer::calculate_overall_health_score(cm, 10);
    // 10 - 4.0 - 3.0 (capped) = 3.0
    EXPECT_NEAR(score, 3.0, 0.01);
}

TEST(HealthAnalyzer, HealthScoreEmptyDistribution) {
    ComplexityMetrics cm;
    cm.average_cc = 0.0;
    double score = HealthAnalyzer::calculate_overall_health_score(cm, 0);
    EXPECT_DOUBLE_EQ(score, 10.0);
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
            EXPECT_EQ(s.severity, "high");
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
    auto smells = ha.calculate_detailed_code_smells({fsd});
    EXPECT_LE(static_cast<int>(smells.size()),
              ci_thresholds::kMaxDetailedSmells);
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
    sym.incoming_refs.resize(20);
    sym.outgoing_refs.resize(20);

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
    sym.incoming_refs.resize(30);
    sym.outgoing_refs.resize(30);

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
    exported_func.incoming_refs.resize(5);

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
    exported_sym.incoming_refs.resize(10);

    EnhancedSymbol private_sym;
    private_sym.symbol.name = "process";
    private_sym.symbol.type = SymbolType::Function;
    private_sym.is_exported = false;
    private_sym.incoming_refs.resize(10);

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
    main_sym.incoming_refs.resize(1);

    EnhancedSymbol other_sym;
    other_sym.symbol.name = "helper";
    other_sym.symbol.type = SymbolType::Function;
    other_sym.incoming_refs.resize(1);

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
    handler_sym.incoming_refs.resize(5);

    EnhancedSymbol plain_sym;
    plain_sym.symbol.name = "compute";
    plain_sym.symbol.type = SymbolType::Function;
    plain_sym.incoming_refs.resize(5);

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
    complex_sym.incoming_refs.resize(5);
    complex_sym.complexity = 15;

    EnhancedSymbol simple_sym;
    simple_sym.symbol.name = "work2";
    simple_sym.symbol.type = SymbolType::Function;
    simple_sym.incoming_refs.resize(5);
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
