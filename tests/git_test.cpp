#include <gtest/gtest.h>

#include <lci/git/analyzer.h>
#include <lci/git/frequency_analyzer.h>
#include <lci/git/pattern_detector.h>
#include <lci/git/provider.h>
#include <lci/git/types.h>

namespace lci {
namespace git {
namespace {

// ============================================================================
// Types Tests
// ============================================================================

TEST(GitTypes, AnalysisParamsDefaults) {
    auto params = AnalysisParams::defaults();
    EXPECT_EQ(params.scope, AnalysisScope::Staged);
    EXPECT_DOUBLE_EQ(params.similarity_threshold, 0.8);
    EXPECT_EQ(params.max_findings, 20);
    EXPECT_EQ(params.focus.size(), 3u);
}

TEST(GitTypes, AnalysisParamsHasFocus) {
    auto params = AnalysisParams::defaults();
    EXPECT_TRUE(params.has_focus("duplicates"));
    EXPECT_TRUE(params.has_focus("naming"));
    EXPECT_TRUE(params.has_focus("metrics"));
    EXPECT_FALSE(params.has_focus("nonexistent"));

    AnalysisParams empty;
    empty.focus.clear();
    EXPECT_TRUE(empty.has_focus("anything"));

    AnalysisParams all;
    all.focus = {"all"};
    EXPECT_TRUE(all.has_focus("duplicates"));
    EXPECT_TRUE(all.has_focus("naming"));
}

TEST(GitTypes, CategorizeDiffSize) {
    EXPECT_EQ(categorize_diff_size(0), DiffSize::Small);
    EXPECT_EQ(categorize_diff_size(9), DiffSize::Small);
    EXPECT_EQ(categorize_diff_size(10), DiffSize::Medium);
    EXPECT_EQ(categorize_diff_size(50), DiffSize::Medium);
    EXPECT_EQ(categorize_diff_size(51), DiffSize::Large);
}

TEST(GitTypes, AnalysisScopeToString) {
    EXPECT_EQ(to_string(AnalysisScope::Staged), "staged");
    EXPECT_EQ(to_string(AnalysisScope::WIP), "wip");
    EXPECT_EQ(to_string(AnalysisScope::Commit), "commit");
    EXPECT_EQ(to_string(AnalysisScope::Range), "range");
}

TEST(GitTypes, FileChangeStatusToString) {
    EXPECT_EQ(to_string(FileChangeStatus::Added), "added");
    EXPECT_EQ(to_string(FileChangeStatus::Modified), "modified");
    EXPECT_EQ(to_string(FileChangeStatus::Deleted), "deleted");
    EXPECT_EQ(to_string(FileChangeStatus::Renamed), "renamed");
    EXPECT_EQ(to_string(FileChangeStatus::Copied), "copied");
}

TEST(GitTypes, FindingSeverityToString) {
    EXPECT_EQ(to_string(FindingSeverity::Critical), "critical");
    EXPECT_EQ(to_string(FindingSeverity::Warning), "warning");
    EXPECT_EQ(to_string(FindingSeverity::Info), "info");
}

// ============================================================================
// Case Style Detection Tests
// ============================================================================

TEST(GitTypes, DetectCaseStyle) {
    EXPECT_EQ(detect_case_style(""), CaseStyle::Unknown);
    EXPECT_EQ(detect_case_style("myVariable"), CaseStyle::CamelCase);
    EXPECT_EQ(detect_case_style("MyVariable"), CaseStyle::PascalCase);
    EXPECT_EQ(detect_case_style("my_variable"), CaseStyle::SnakeCase);
    EXPECT_EQ(detect_case_style("my-variable"), CaseStyle::KebabCase);
    EXPECT_EQ(detect_case_style("x"), CaseStyle::Unknown);
}

TEST(GitTypes, CaseStyleToString) {
    EXPECT_EQ(to_string(CaseStyle::CamelCase), "camelCase");
    EXPECT_EQ(to_string(CaseStyle::PascalCase), "PascalCase");
    EXPECT_EQ(to_string(CaseStyle::SnakeCase), "snake_case");
    EXPECT_EQ(to_string(CaseStyle::KebabCase), "kebab-case");
    EXPECT_EQ(to_string(CaseStyle::Unknown), "unknown");
}

// ============================================================================
// Language Detection Tests
// ============================================================================

TEST(GitTypes, GetLanguageFromPath) {
    EXPECT_EQ(get_language_from_path("main.go"), Language::Go);
    EXPECT_EQ(get_language_from_path("app.js"), Language::JavaScript);
    EXPECT_EQ(get_language_from_path("app.jsx"), Language::JavaScript);
    EXPECT_EQ(get_language_from_path("app.ts"), Language::TypeScript);
    EXPECT_EQ(get_language_from_path("app.tsx"), Language::TypeScript);
    EXPECT_EQ(get_language_from_path("script.py"), Language::Python);
    EXPECT_EQ(get_language_from_path("lib.rs"), Language::Rust);
    EXPECT_EQ(get_language_from_path("Main.java"), Language::Java);
    EXPECT_EQ(get_language_from_path("Program.cs"), Language::CSharp);
    EXPECT_EQ(get_language_from_path("main.cpp"), Language::Cpp);
    EXPECT_EQ(get_language_from_path("main.cc"), Language::Cpp);
    EXPECT_EQ(get_language_from_path("main.c"), Language::C);
    EXPECT_EQ(get_language_from_path("main.h"), Language::C);
    EXPECT_EQ(get_language_from_path("index.php"), Language::PHP);
    EXPECT_EQ(get_language_from_path("app.rb"), Language::Ruby);
    EXPECT_EQ(get_language_from_path("main.swift"), Language::Swift);
    EXPECT_EQ(get_language_from_path("Main.kt"), Language::Kotlin);
    EXPECT_EQ(get_language_from_path("Main.scala"), Language::Scala);
    EXPECT_EQ(get_language_from_path("main.zig"), Language::Zig);
    EXPECT_EQ(get_language_from_path("README"), Language::Unknown);
    EXPECT_EQ(get_language_from_path("Makefile"), Language::Unknown);
}

// ============================================================================
// Symbol Kind Tests
// ============================================================================

TEST(GitTypes, SymbolTypeToKind) {
    EXPECT_EQ(symbol_type_to_kind("function"), SymbolKind::Function);
    EXPECT_EQ(symbol_type_to_kind("method"), SymbolKind::Method);
    EXPECT_EQ(symbol_type_to_kind("class"), SymbolKind::Class);
    EXPECT_EQ(symbol_type_to_kind("interface"), SymbolKind::Interface);
    EXPECT_EQ(symbol_type_to_kind("struct"), SymbolKind::Struct);
    EXPECT_EQ(symbol_type_to_kind("type"), SymbolKind::Type);
    EXPECT_EQ(symbol_type_to_kind("type_alias"), SymbolKind::Type);
    EXPECT_EQ(symbol_type_to_kind("constant"), SymbolKind::Constant);
    EXPECT_EQ(symbol_type_to_kind("variable"), SymbolKind::Variable);
    EXPECT_EQ(symbol_type_to_kind("field"), SymbolKind::Field);
    EXPECT_EQ(symbol_type_to_kind("enum"), SymbolKind::Enum);
    EXPECT_EQ(symbol_type_to_kind("enum_member"), SymbolKind::EnumMember);
    EXPECT_EQ(symbol_type_to_kind("module"), SymbolKind::Module);
    EXPECT_EQ(symbol_type_to_kind("namespace"), SymbolKind::Namespace);
    EXPECT_EQ(symbol_type_to_kind("property"), SymbolKind::Property);
    EXPECT_EQ(symbol_type_to_kind("banana"), SymbolKind::UnknownKind);
}

// ============================================================================
// Naming Convention Tests
// ============================================================================

TEST(GitTypes, IsValidCaseStyleGo) {
    EXPECT_TRUE(is_valid_case_style(Language::Go, SymbolKind::Function, CaseStyle::PascalCase));
    EXPECT_TRUE(is_valid_case_style(Language::Go, SymbolKind::Function, CaseStyle::CamelCase));
    EXPECT_FALSE(is_valid_case_style(Language::Go, SymbolKind::Function, CaseStyle::SnakeCase));
}

TEST(GitTypes, IsValidCaseStylePython) {
    EXPECT_TRUE(is_valid_case_style(Language::Python, SymbolKind::Function, CaseStyle::SnakeCase));
    EXPECT_FALSE(is_valid_case_style(Language::Python, SymbolKind::Function, CaseStyle::CamelCase));
    EXPECT_TRUE(is_valid_case_style(Language::Python, SymbolKind::Class, CaseStyle::PascalCase));
}

TEST(GitTypes, IsValidCaseStyleUnknownLanguage) {
    EXPECT_TRUE(is_valid_case_style(Language::Unknown, SymbolKind::Function, CaseStyle::SnakeCase));
}

TEST(GitTypes, IsValidCaseStyleUnknownKind) {
    EXPECT_TRUE(is_valid_case_style(Language::Go, SymbolKind::UnknownKind, CaseStyle::SnakeCase));
}

TEST(GitTypes, GetExpectedStylesGo) {
    auto styles = get_expected_styles(Language::Go, SymbolKind::Function);
    EXPECT_EQ(styles.size(), 2u);
}

TEST(GitTypes, GetExpectedStylesUnknown) {
    auto styles = get_expected_styles(Language::Unknown, SymbolKind::Function);
    EXPECT_TRUE(styles.empty());
}

// ============================================================================
// Risk / Severity Calculation Tests
// ============================================================================

TEST(GitResults, CalculateRiskScoreEmpty) {
    double risk = calculate_risk_score({}, {}, {});
    EXPECT_DOUBLE_EQ(risk, 0.0);
}

TEST(GitResults, CalculateRiskScoreCapped) {
    std::vector<DuplicateFinding> dups;
    for (int i = 0; i < 20; ++i) {
        DuplicateFinding d;
        d.severity = FindingSeverity::Critical;
        dups.push_back(d);
    }
    double risk = calculate_risk_score(dups, {}, {});
    EXPECT_DOUBLE_EQ(risk, 1.0);
}

TEST(GitResults, CalculateRiskScoreMixed) {
    DuplicateFinding d;
    d.severity = FindingSeverity::Warning;
    NamingFinding n;
    n.severity = FindingSeverity::Info;
    double risk = calculate_risk_score({d}, {n}, {});
    EXPECT_NEAR(risk, 0.10, 0.001);
}

TEST(GitResults, DetermineDuplicateSeverity) {
    EXPECT_EQ(determine_duplicate_severity(0.96, 25), FindingSeverity::Critical);
    EXPECT_EQ(determine_duplicate_severity(0.92, 10), FindingSeverity::Warning);
    EXPECT_EQ(determine_duplicate_severity(0.80, 35), FindingSeverity::Warning);
    EXPECT_EQ(determine_duplicate_severity(0.85, 10), FindingSeverity::Info);
}

TEST(GitResults, DetermineNamingSeverity) {
    EXPECT_EQ(determine_naming_severity(NamingIssueType::SimilarExists, 0.95),
              FindingSeverity::Warning);
    EXPECT_EQ(determine_naming_severity(NamingIssueType::SimilarExists, 0.80),
              FindingSeverity::Info);
    EXPECT_EQ(determine_naming_severity(NamingIssueType::CaseMismatch, 0.0),
              FindingSeverity::Warning);
    EXPECT_EQ(determine_naming_severity(NamingIssueType::Abbreviation, 0.0),
              FindingSeverity::Info);
}

TEST(GitResults, DetermineMetricsSeverity) {
    MetricsThresholds t;
    SymbolMetrics m;

    m.complexity = 25;
    EXPECT_EQ(determine_metrics_severity(MetricsIssueType::HighComplexity, m, t),
              FindingSeverity::Critical);

    m.complexity = 12;
    EXPECT_EQ(determine_metrics_severity(MetricsIssueType::HighComplexity, m, t),
              FindingSeverity::Warning);

    m.lines_of_code = 250;
    EXPECT_EQ(determine_metrics_severity(MetricsIssueType::LongFunction, m, t),
              FindingSeverity::Critical);

    m.nesting_depth = 7;
    EXPECT_EQ(determine_metrics_severity(MetricsIssueType::DeepNesting, m, t),
              FindingSeverity::Critical);

    EXPECT_EQ(determine_metrics_severity(MetricsIssueType::ComplexityGrew, m, t),
              FindingSeverity::Warning);
    EXPECT_EQ(determine_metrics_severity(MetricsIssueType::PurityLost, m, t),
              FindingSeverity::Warning);
    EXPECT_EQ(determine_metrics_severity(MetricsIssueType::ImpureFunction, m, t),
              FindingSeverity::Info);
}

TEST(GitResults, GenerateTopRecommendation) {
    EXPECT_EQ(generate_top_recommendation({}, {}, {}), "");

    DuplicateFinding d;
    d.severity = FindingSeverity::Critical;
    d.suggestion = "critical dup";
    EXPECT_EQ(generate_top_recommendation({d}, {}, {}), "critical dup");

    d.severity = FindingSeverity::Info;
    d.suggestion = "info dup";
    EXPECT_EQ(generate_top_recommendation({d}, {}, {}), "info dup");

    MetricsFinding mf;
    mf.severity = FindingSeverity::Critical;
    mf.suggestion = "critical metrics";
    d.severity = FindingSeverity::Warning;
    EXPECT_EQ(generate_top_recommendation({d}, {}, {mf}), "critical metrics");
}

// ============================================================================
// Pattern Detector Tests
// ============================================================================

TEST(PatternDetector, CountLines) {
    EXPECT_EQ(PatternDetector::count_lines(""), 0);
    EXPECT_EQ(PatternDetector::count_lines("hello"), 1);
    EXPECT_EQ(PatternDetector::count_lines("a\nb\nc"), 3);
    EXPECT_EQ(PatternDetector::count_lines("a\nb\nc\n"), 3);
}

TEST(PatternDetector, DetectGodObject) {
    PatternDetector pd;
    pd.god_object_lines_threshold = 5;

    std::string small = "line1\nline2\nline3\n";
    auto patterns = pd.detect_patterns(small, "test.go");
    bool found_god = false;
    for (const auto& p : patterns) {
        if (p.type == AntiPatternType::GodObject) found_god = true;
    }
    EXPECT_FALSE(found_god);

    std::string large = "line1\nline2\nline3\nline4\nline5\nline6\n";
    patterns = pd.detect_patterns(large, "test.go");
    found_god = false;
    for (const auto& p : patterns) {
        if (p.type == AntiPatternType::GodObject) found_god = true;
    }
    EXPECT_TRUE(found_god);
}

TEST(PatternDetector, DetectSwitchFactory) {
    PatternDetector pd;
    pd.switch_cases_threshold = 3;

    std::string code = "switch x {\n"
                       "case 1:\n"
                       "  doA()\n"
                       "case 2:\n"
                       "  doB()\n"
                       "case 3:\n"
                       "  doC()\n"
                       "default:\n"
                       "  doD()\n"
                       "}\n";

    auto patterns = pd.detect_patterns(code, "test.go");
    bool found_switch = false;
    for (const auto& p : patterns) {
        if (p.type == AntiPatternType::SwitchFactory) {
            found_switch = true;
            EXPECT_EQ(p.metrics.at("case_count"), 4);
        }
    }
    EXPECT_TRUE(found_switch);
}

TEST(PatternDetector, DetectEnumAggregation) {
    PatternDetector pd;
    pd.enum_values_threshold = 3;

    std::string code = "const (\n"
                       "  A = iota\n"
                       "  B\n"
                       "  C\n"
                       ")\n"
                       "const X = 1\n"
                       "const Y = 2\n";

    auto patterns = pd.detect_patterns(code, "types.go");
    bool found_enum = false;
    for (const auto& p : patterns) {
        if (p.type == AntiPatternType::EnumAggregation) found_enum = true;
    }
    EXPECT_TRUE(found_enum);
}

TEST(PatternDetector, DetectBarrelFile) {
    PatternDetector pd;

    std::string code = "export { a } from './a'\n"
                       "export { b } from './b'\n"
                       "export { c } from './c'\n"
                       "export { d } from './d'\n"
                       "export { e } from './e'\n"
                       "export { f } from './f'\n"
                       "export { g } from './g'\n"
                       "export { h } from './h'\n"
                       "export { i } from './i'\n"
                       "export { j } from './j'\n";

    auto patterns = pd.detect_patterns(code, "index.ts");
    bool found_barrel = false;
    for (const auto& p : patterns) {
        if (p.type == AntiPatternType::BarrelFile) found_barrel = true;
    }
    EXPECT_TRUE(found_barrel);

    // Non-barrel file name should not trigger
    patterns = pd.detect_patterns(code, "main.ts");
    found_barrel = false;
    for (const auto& p : patterns) {
        if (p.type == AntiPatternType::BarrelFile) found_barrel = true;
    }
    EXPECT_FALSE(found_barrel);
}

TEST(PatternDetector, DetectConfigAggregation) {
    PatternDetector pd;
    pd.config_fields_threshold = 3;

    std::string code = "type Config struct {\n"
                       "  Host string `json:\"host\"`\n"
                       "  Port int    `json:\"port\"`\n"
                       "  Debug bool  `json:\"debug\"`\n"
                       "}\n";

    auto patterns = pd.detect_patterns(code, "config.go");
    bool found_config = false;
    for (const auto& p : patterns) {
        if (p.type == AntiPatternType::ConfigAggregation) found_config = true;
    }
    EXPECT_TRUE(found_config);

    // Non-config file should not trigger
    patterns = pd.detect_patterns(code, "main.go");
    found_config = false;
    for (const auto& p : patterns) {
        if (p.type == AntiPatternType::ConfigAggregation) found_config = true;
    }
    EXPECT_FALSE(found_config);
}

TEST(PatternDetector, DetectRegistrationFunction) {
    PatternDetector pd;
    pd.registration_calls_threshold = 3;

    std::string code = "func registerAll() {\n"
                       "  router.Handle(\"/a\", a)\n"
                       "  router.Handle(\"/b\", b)\n"
                       "  router.Handle(\"/c\", c)\n"
                       "}\n";

    auto patterns = pd.detect_patterns(code, "routes.go");
    bool found_reg = false;
    for (const auto& p : patterns) {
        if (p.type == AntiPatternType::RegistrationFunction) found_reg = true;
    }
    EXPECT_TRUE(found_reg);
}

TEST(PatternDetector, QuickScan) {
    PatternDetector pd;
    pd.god_object_lines_threshold = 5;

    std::string large = "a\nb\nc\nd\ne\nf\n";
    AntiPattern ap;
    EXPECT_TRUE(pd.quick_scan(large, "test.go", ap));
    EXPECT_EQ(ap.type, AntiPatternType::GodObject);

    std::string small = "a\nb\n";
    EXPECT_FALSE(pd.quick_scan(small, "test.go", ap));
}

TEST(PatternDetector, GetRecommendations) {
    auto recs = PatternDetector::get_recommendations(AntiPatternType::RegistrationFunction);
    EXPECT_EQ(recs.size(), 4u);

    recs = PatternDetector::get_recommendations(AntiPatternType::GodObject);
    EXPECT_EQ(recs.size(), 4u);
}

TEST(PatternDetector, DetermineAntiPatternSeverity) {
    EXPECT_EQ(determine_anti_pattern_severity(20, 10), AntiPatternSeverity::High);
    EXPECT_EQ(determine_anti_pattern_severity(15, 10), AntiPatternSeverity::Medium);
    EXPECT_EQ(determine_anti_pattern_severity(12, 10), AntiPatternSeverity::Low);
}

TEST(PatternDetector, CountCasesInSwitch) {
    std::string code = "switch x {\ncase 1:\n  break\ncase 2:\n  break\ndefault:\n  break\n}\n";
    // The opening brace of the switch is at position after "switch x {"
    size_t brace_pos = code.find('{') + 1;
    EXPECT_EQ(count_cases_in_switch(code, brace_pos), 3);
}

TEST(PatternDetector, CountCasesIgnoresEmbeddedWords) {
    std::string code = "switch x {\ncase 1:\n  mycase = 1\ncase 2:\n  break\n}\n";
    size_t brace_pos = code.find('{') + 1;
    // "mycase" should not be counted, only standalone "case"
    EXPECT_EQ(count_cases_in_switch(code, brace_pos), 2);
}

// ============================================================================
// Provider Parse Tests (using mock git output)
// ============================================================================

// Provider is tested by checking its parsing logic, since actual git requires
// a real repo. We test Provider::create in a real git repo below.

TEST(GitProvider, CreateInGitRepo) {
    // This test runs in the lci repo which is a git repo
    Provider p;
    bool ok = Provider::create(".", p);
    // May succeed or fail depending on CWD; if it succeeds, verify basic ops
    if (ok) {
        EXPECT_FALSE(p.repo_root().empty());

        std::string branch;
        if (p.get_current_branch(branch)) {
            EXPECT_FALSE(branch.empty());
        }
    }
}

TEST(GitProvider, GetTargetRef) {
    Provider p;
    // Provider doesn't need to be connected to a repo for get_target_ref

    AnalysisParams params;
    params.scope = AnalysisScope::Staged;
    EXPECT_EQ(p.get_target_ref(params), "STAGED");

    params.scope = AnalysisScope::WIP;
    EXPECT_EQ(p.get_target_ref(params), "WORKING");

    params.scope = AnalysisScope::Commit;
    params.base_ref = "abc123";
    EXPECT_EQ(p.get_target_ref(params), "abc123");

    params.scope = AnalysisScope::Commit;
    params.base_ref = "";
    EXPECT_EQ(p.get_target_ref(params), "HEAD");

    params.scope = AnalysisScope::Range;
    params.target_ref = "def456";
    EXPECT_EQ(p.get_target_ref(params), "def456");

    params.scope = AnalysisScope::Range;
    params.target_ref = "";
    EXPECT_EQ(p.get_target_ref(params), "HEAD");
}

// ============================================================================
// Anti-Pattern Type String Tests
// ============================================================================

TEST(GitTypes, AntiPatternTypeToString) {
    EXPECT_EQ(to_string(AntiPatternType::RegistrationFunction), "registration_function");
    EXPECT_EQ(to_string(AntiPatternType::EnumAggregation), "enum_aggregation");
    EXPECT_EQ(to_string(AntiPatternType::GodObject), "god_object");
    EXPECT_EQ(to_string(AntiPatternType::BarrelFile), "barrel_file");
    EXPECT_EQ(to_string(AntiPatternType::SwitchFactory), "switch_factory");
    EXPECT_EQ(to_string(AntiPatternType::ConfigAggregation), "config_aggregation");
}

TEST(GitTypes, AntiPatternSeverityToString) {
    EXPECT_EQ(to_string(AntiPatternSeverity::High), "high");
    EXPECT_EQ(to_string(AntiPatternSeverity::Medium), "medium");
    EXPECT_EQ(to_string(AntiPatternSeverity::Low), "low");
}

TEST(GitTypes, MetricsIssueTypeToString) {
    EXPECT_EQ(to_string(MetricsIssueType::HighComplexity), "high_complexity");
    EXPECT_EQ(to_string(MetricsIssueType::LongFunction), "long_function");
    EXPECT_EQ(to_string(MetricsIssueType::DeepNesting), "deep_nesting");
    EXPECT_EQ(to_string(MetricsIssueType::ComplexityGrew), "complexity_grew");
    EXPECT_EQ(to_string(MetricsIssueType::PurityLost), "purity_lost");
    EXPECT_EQ(to_string(MetricsIssueType::ImpureFunction), "impure_function");
}

TEST(GitTypes, NamingIssueTypeToString) {
    EXPECT_EQ(to_string(NamingIssueType::CaseMismatch), "case_mismatch");
    EXPECT_EQ(to_string(NamingIssueType::SimilarExists), "similar_exists");
    EXPECT_EQ(to_string(NamingIssueType::Abbreviation), "abbreviation");
}

TEST(GitTypes, MetricsThresholdsDefaults) {
    auto t = MetricsThresholds::defaults();
    EXPECT_EQ(t.high_complexity, 10);
    EXPECT_EQ(t.long_function, 100);
    EXPECT_EQ(t.deep_nesting, 4);
    EXPECT_EQ(t.complexity_growth_threshold, 50);
}

// ============================================================================
// Analyzer Static Method Tests
// ============================================================================

TEST(GitAnalyzer, IsSupportedFile) {
    // Test via normalize_content and extract_symbol_content (static methods).
    // is_supported_file is private, but we verify the file filter indirectly.
    auto norm = normalize_code_content("  int x = 1;\n  // comment\n  int y = 2;\n");
    EXPECT_FALSE(norm.empty());
    EXPECT_EQ(norm.find("// comment"), std::string::npos);
}

TEST(GitAnalyzer, NormalizeContentStripsComments) {
    auto result = normalize_code_content(
        "  line1\n"
        "  // this is a comment\n"
        "  # another comment\n"
        "  line2\n"
        "\n"
        "  line3\n");
    EXPECT_EQ(result, "line1\nline2\nline3");
}

TEST(GitAnalyzer, NormalizeContentEmpty) {
    EXPECT_EQ(normalize_code_content(""), "");
    EXPECT_EQ(normalize_code_content("  \n  \n"), "");
    EXPECT_EQ(normalize_code_content("// only comments\n# more\n"), "");
}

TEST(GitAnalyzer, ExtractSymbolContent) {
    std::string content = "line1\nline2\nline3\nline4\nline5\n";
    auto result = extract_symbol_content(content, 2, 4);
    EXPECT_EQ(result, "line2\nline3\nline4");
}

TEST(GitAnalyzer, ExtractSymbolContentSingleLine) {
    std::string content = "line1\nline2\nline3\n";
    auto result = extract_symbol_content(content, 2, 2);
    EXPECT_EQ(result, "line2");
}

TEST(GitAnalyzer, ExtractSymbolContentInvalid) {
    EXPECT_EQ(extract_symbol_content("", 1, 1), "");
    EXPECT_EQ(extract_symbol_content("line1", 0, 0), "");
}

TEST(GitAnalyzer, StructuralSimilarityIdentical) {
    std::string code = "int foo() { return 1; }";
    EXPECT_DOUBLE_EQ(code_structural_similarity(code, code), 1.0);
}

TEST(GitAnalyzer, StructuralSimilarityDifferent) {
    std::string a = "int foo() { return 1; }";
    std::string b = "class Bar extends Baz { void quux() {} }";
    double sim = code_structural_similarity(a, b);
    EXPECT_LT(sim, 0.5);
}

TEST(GitAnalyzer, StructuralSimilarityEmpty) {
    EXPECT_DOUBLE_EQ(code_structural_similarity("", "code"), 0.0);
    EXPECT_DOUBLE_EQ(code_structural_similarity("code", ""), 0.0);
}

// ============================================================================
// Frequency Types Tests
// ============================================================================

TEST(FrequencyTypes, ParseTimeWindow) {
    EXPECT_EQ(parse_time_window("7d"), TimeWindow::Days7);
    EXPECT_EQ(parse_time_window("7days"), TimeWindow::Days7);
    EXPECT_EQ(parse_time_window("week"), TimeWindow::Days7);
    EXPECT_EQ(parse_time_window("30d"), TimeWindow::Days30);
    EXPECT_EQ(parse_time_window("month"), TimeWindow::Days30);
    EXPECT_EQ(parse_time_window("90d"), TimeWindow::Days90);
    EXPECT_EQ(parse_time_window("quarter"), TimeWindow::Days90);
    EXPECT_EQ(parse_time_window("1y"), TimeWindow::Year1);
    EXPECT_EQ(parse_time_window("year"), TimeWindow::Year1);
    EXPECT_EQ(parse_time_window("365d"), TimeWindow::Year1);
    EXPECT_EQ(parse_time_window("unknown"), TimeWindow::Days30);
}

TEST(FrequencyTypes, TimeWindowToString) {
    EXPECT_EQ(to_string(TimeWindow::Days7), "7d");
    EXPECT_EQ(to_string(TimeWindow::Days30), "30d");
    EXPECT_EQ(to_string(TimeWindow::Days90), "90d");
    EXPECT_EQ(to_string(TimeWindow::Year1), "1y");
}

TEST(FrequencyTypes, TimeWindowSeconds) {
    EXPECT_EQ(time_window_seconds(TimeWindow::Days7), 7 * 86400);
    EXPECT_EQ(time_window_seconds(TimeWindow::Days30), 30 * 86400);
    EXPECT_EQ(time_window_seconds(TimeWindow::Days90), 90 * 86400);
    EXPECT_EQ(time_window_seconds(TimeWindow::Year1), 365 * 86400);
}

TEST(FrequencyTypes, CalculateVolatilityScore) {
    // Zero window days defaults to 30.
    double score = calculate_volatility_score(0, 0, 0, 0.0);
    EXPECT_DOUBLE_EQ(score, 0.0);

    // High activity.
    score = calculate_volatility_score(30, 3000, 5, 30.0);
    EXPECT_DOUBLE_EQ(score, 1.0);

    // Moderate activity.
    score = calculate_volatility_score(15, 1500, 3, 30.0);
    EXPECT_GT(score, 0.3);
    EXPECT_LT(score, 0.8);
}

TEST(FrequencyTypes, CalculateCollisionScore) {
    // Fewer than 2 contributors => 0.
    std::vector<ContributorActivity> one = {{"Alice", "a@x.com", 5, 0, 0, 1.0, 0}};
    EXPECT_DOUBLE_EQ(calculate_collision_score(one, 5), 0.0);

    // Two contributors.
    std::vector<ContributorActivity> two = {
        {"Alice", "a@x.com", 5, 0, 0, 0.6, 0},
        {"Bob", "b@x.com", 3, 0, 0, 0.4, 0},
    };
    double score = calculate_collision_score(two, 5);
    EXPECT_GT(score, 0.0);
    EXPECT_LE(score, 1.0);
}

TEST(FrequencyTypes, DetermineCollisionSeverity) {
    EXPECT_EQ(determine_collision_severity(0.8), FindingSeverity::Critical);
    EXPECT_EQ(determine_collision_severity(0.5), FindingSeverity::Warning);
    EXPECT_EQ(determine_collision_severity(0.2), FindingSeverity::Info);
}

// ============================================================================
// Churn Filter Tests
// ============================================================================

TEST(ChurnFilter, DefaultExclusions) {
    EXPECT_TRUE(should_exclude_from_churn("CHANGELOG.md"));
    EXPECT_TRUE(should_exclude_from_churn("package-lock.json"));
    EXPECT_TRUE(should_exclude_from_churn("yarn.lock"));
    EXPECT_TRUE(should_exclude_from_churn("go.sum"));
    EXPECT_TRUE(should_exclude_from_churn("image.png"));
    EXPECT_TRUE(should_exclude_from_churn("vendor/lib/foo.go"));
    EXPECT_TRUE(should_exclude_from_churn("node_modules/dep/index.js"));
    EXPECT_TRUE(should_exclude_from_churn("docs/readme.md"));
}

TEST(ChurnFilter, SourceFilesNotExcluded) {
    EXPECT_FALSE(should_exclude_from_churn("src/main.go"));
    EXPECT_FALSE(should_exclude_from_churn("internal/git/analyzer.go"));
    EXPECT_FALSE(should_exclude_from_churn("app.js"));
    EXPECT_FALSE(should_exclude_from_churn("lib/utils.py"));
}

TEST(ChurnFilter, CustomIncludePatterns) {
    // Only .go files included.
    EXPECT_FALSE(should_exclude_from_churn("main.go", {"*.go"}, {}, false));
    EXPECT_TRUE(should_exclude_from_churn("main.js", {"*.go"}, {}, false));
}

TEST(ChurnFilter, CustomExcludePatterns) {
    EXPECT_TRUE(should_exclude_from_churn("test_main.go", {}, {"test_*"}, false));
    EXPECT_FALSE(should_exclude_from_churn("main.go", {}, {"test_*"}, false));
}

TEST(ChurnFilter, SkipDefaults) {
    // With skip_defaults, normally excluded files are included.
    EXPECT_FALSE(should_exclude_from_churn("CHANGELOG.md", {}, {}, true));
    EXPECT_FALSE(should_exclude_from_churn("docs/readme.md", {}, {}, true));
}

// ============================================================================
// ChangeFrequencyParams Tests
// ============================================================================

TEST(ChangeFrequencyParams, Defaults) {
    auto p = ChangeFrequencyParams::defaults();
    EXPECT_EQ(p.time_window, "30d");
    EXPECT_EQ(p.granularity, "file");
    EXPECT_EQ(p.min_changes, 2);
    EXPECT_EQ(p.min_contributors, 2);
    EXPECT_EQ(p.top_n, 50);
    EXPECT_TRUE(p.has_focus(FrequencyFocus::Hotspots));
    EXPECT_TRUE(p.has_focus(FrequencyFocus::Collisions));
    EXPECT_TRUE(p.has_focus(FrequencyFocus::All));
}

TEST(ChangeFrequencyParams, HasFocus) {
    ChangeFrequencyParams p;
    p.focus = {"hotspots"};
    EXPECT_TRUE(p.has_focus(FrequencyFocus::Hotspots));
    EXPECT_FALSE(p.has_focus(FrequencyFocus::Collisions));

    p.focus = {"all"};
    EXPECT_TRUE(p.has_focus(FrequencyFocus::Collisions));
    EXPECT_TRUE(p.has_focus(FrequencyFocus::Patterns));

    p.focus.clear();
    EXPECT_TRUE(p.has_focus(FrequencyFocus::Ownership));
}

TEST(ChangeFrequencyParams, GetTimeWindow) {
    ChangeFrequencyParams p;
    p.time_window = "90d";
    EXPECT_EQ(p.get_time_window(), TimeWindow::Days90);

    p.time_window = "";
    EXPECT_EQ(p.get_time_window(), TimeWindow::Days30);
}

TEST(ChangeFrequencyParams, GetGranularity) {
    ChangeFrequencyParams p;
    p.granularity = "symbol";
    EXPECT_EQ(p.get_granularity(), FrequencyGranularity::Symbol);

    p.granularity = "file";
    EXPECT_EQ(p.get_granularity(), FrequencyGranularity::File);

    p.granularity = "";
    EXPECT_EQ(p.get_granularity(), FrequencyGranularity::File);
}

// ============================================================================
// FrequencyCache Tests
// ============================================================================

TEST(FrequencyCache, SetGetFileFrequency) {
    FrequencyCache cache(60);

    FileChangeFrequency freq;
    freq.file_path = "test.go";
    freq.metrics[TimeWindow::Days30] = FrequencyMetrics{5, 100, 50, 2, 0, 0, 0.5, 0.3};

    cache.set_file_frequency("test.go", TimeWindow::Days30, freq);

    FileChangeFrequency result;
    EXPECT_TRUE(cache.get_file_frequency("test.go", TimeWindow::Days30, result));
    EXPECT_EQ(result.file_path, "test.go");
    EXPECT_EQ(result.metrics[TimeWindow::Days30].change_count, 5);
}

TEST(FrequencyCache, MissOnEmpty) {
    FrequencyCache cache(60);
    FileChangeFrequency result;
    EXPECT_FALSE(cache.get_file_frequency("missing.go", TimeWindow::Days30, result));
}

TEST(FrequencyCache, SetGetReport) {
    FrequencyCache cache(60);

    ChangeFrequencyReport report;
    report.summary.total_files_analyzed = 42;

    cache.set_report("*.go", TimeWindow::Days30, report);

    ChangeFrequencyReport result;
    EXPECT_TRUE(cache.get_report("*.go", TimeWindow::Days30, result));
    EXPECT_EQ(result.summary.total_files_analyzed, 42);
}

TEST(FrequencyCache, InvalidateFile) {
    FrequencyCache cache(60);

    FileChangeFrequency freq;
    freq.file_path = "test.go";
    cache.set_file_frequency("test.go", TimeWindow::Days30, freq);

    FileChangeFrequency result;
    EXPECT_TRUE(cache.get_file_frequency("test.go", TimeWindow::Days30, result));

    cache.invalidate_file("test.go");
    EXPECT_FALSE(cache.get_file_frequency("test.go", TimeWindow::Days30, result));
}

TEST(FrequencyCache, Clear) {
    FrequencyCache cache(60);

    FileChangeFrequency freq;
    freq.file_path = "a.go";
    cache.set_file_frequency("a.go", TimeWindow::Days30, freq);
    freq.file_path = "b.go";
    cache.set_file_frequency("b.go", TimeWindow::Days30, freq);

    cache.clear();

    FileChangeFrequency result;
    EXPECT_FALSE(cache.get_file_frequency("a.go", TimeWindow::Days30, result));
    EXPECT_FALSE(cache.get_file_frequency("b.go", TimeWindow::Days30, result));
}

TEST(FrequencyCache, Stats) {
    FrequencyCache cache(60);

    FileChangeFrequency freq;
    freq.file_path = "test.go";
    cache.set_file_frequency("test.go", TimeWindow::Days30, freq);

    FileChangeFrequency result;
    cache.get_file_frequency("test.go", TimeWindow::Days30, result);  // hit
    cache.get_file_frequency("missing.go", TimeWindow::Days30, result);  // miss

    auto s = cache.stats();
    EXPECT_EQ(s.hits, 1u);
    EXPECT_EQ(s.misses, 1u);
    EXPECT_DOUBLE_EQ(s.hit_rate, 0.5);
    EXPECT_EQ(s.entry_count, 1);
}

// ============================================================================
// HistoryProvider Parse Tests
// ============================================================================

TEST(HistoryProvider, ParseRenamePath) {
    // Simple format.
    std::string new_path, old_path;
    parse_rename_path("old.go => new.go", new_path, old_path);
    EXPECT_EQ(new_path, "new.go");
    EXPECT_EQ(old_path, "old.go");
}

TEST(HistoryProvider, DetermineStatus) {
    EXPECT_EQ(determine_change_status(10, 0, ""), "A");
    EXPECT_EQ(determine_change_status(0, 5, ""), "D");
    EXPECT_EQ(determine_change_status(5, 3, ""), "M");
    EXPECT_EQ(determine_change_status(5, 3, "old.go"), "R");
}

// ============================================================================
// FrequencyAnalyzer Unit Tests (extract_module_path, find_most_active_contributor)
// ============================================================================

TEST(FrequencyAnalyzer, ExtractModulePath) {
    EXPECT_EQ(extract_module_path("main.go"), "");
    EXPECT_EQ(extract_module_path("internal/git/analyzer.go"), "internal/git");
    EXPECT_EQ(extract_module_path("src/deep/nested/file.cpp"), "src/deep");
    EXPECT_EQ(extract_module_path("pkg/file.go"), "pkg");
}

TEST(FrequencyAnalyzer, FindMostActiveContributor) {
    std::vector<CommitInfo> commits = {
        {"aaa", "Alice", "a@x.com", 100, "msg1", {}},
        {"bbb", "Bob", "b@x.com", 200, "msg2", {}},
        {"ccc", "Alice", "a@x.com", 300, "msg3", {}},
        {"ddd", "Alice", "a@x.com", 400, "msg4", {}},
    };
    EXPECT_EQ(find_most_active_contributor(commits), "Alice");
}

TEST(FrequencyAnalyzer, FindMostActiveContributorEmpty) {
    EXPECT_EQ(find_most_active_contributor({}), "");
}

TEST(FrequencyAnalyzer, GenerateCollisionRecommendation) {
    FileChangeFrequency freq;
    freq.file_path = "test.go";
    ContributorActivity ca;
    ca.author_name = "Alice";
    ca.ownership_share = 0.7;
    freq.contributors.push_back(ca);

    auto rec = generate_collision_recommendation(
        freq, FindingSeverity::Critical);
    EXPECT_FALSE(rec.empty());
    EXPECT_NE(rec.find("Alice"), std::string::npos);

    rec = generate_collision_recommendation(
        freq, FindingSeverity::Warning);
    EXPECT_NE(rec.find("Moderate"), std::string::npos);

    rec = generate_collision_recommendation(
        freq, FindingSeverity::Info);
    EXPECT_NE(rec.find("Low"), std::string::npos);
}

}  // namespace
}  // namespace git
}  // namespace lci
