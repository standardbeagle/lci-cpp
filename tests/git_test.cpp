#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

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
    // Ambiguous C/C++ header: the centralized extension table (language_map.h)
    // classifies .h as Cpp so it parses with the C++ grammar superset, matching
    // the parser, the language summary, and the Go reference which groups
    // .c/.h/.hpp as "cpp". Reconciles the pre-existing drift where only the
    // naming-convention site called .h "C".
    EXPECT_EQ(get_language_from_path("main.h"), Language::Cpp);
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

// ============================================================================
// report_to_json: serialization shape, types, and path normalization
// ============================================================================

namespace {

AnalysisReport make_report_with_one_duplicate(const std::string& abs_project_root) {
    AnalysisReport r;
    r.metadata.base_ref = "HEAD";
    r.metadata.target_ref = "WORKING";
    r.metadata.scope = AnalysisScope::WIP;
    r.metadata.analysis_time_ms = 7;
    r.metadata.analyzed_at = std::chrono::system_clock::time_point{};

    r.summary.files_changed = 1;
    r.summary.symbols_added = 2;
    r.summary.symbols_modified = 0;
    r.summary.symbols_deleted = 0;
    r.summary.duplicates_found = 1;
    r.summary.naming_issues_found = 0;
    r.summary.metrics_issues_found = 0;
    r.summary.risk_score = 0.42;
    r.summary.top_recommendation = "Extract common logic";

    DuplicateFinding d;
    d.severity = FindingSeverity::Warning;
    d.description = "Exact duplicate of foo";
    // new_code uses a relative path (as the git changed-files path arrives).
    d.new_code = {"src/foo.cpp", 10, 20, "foo", {}};
    // existing_code uses an absolute path (as the index stores it).
    d.existing_code = {abs_project_root + "/src/foo.cpp", 5, 15, "foo", {}};
    d.similarity = 0.95;
    d.type = "structural";
    d.suggestion = "Extract common code into a shared function";
    r.duplicates.push_back(std::move(d));
    return r;
}

}  // namespace

TEST(GitReportToJson, EmitsExpectedTopLevelKeys) {
    AnalysisReport r;
    r.metadata.scope = AnalysisScope::WIP;
    auto j = report_to_json(r, "/tmp/proj");
    ASSERT_TRUE(j.contains("summary"));
    ASSERT_TRUE(j.contains("metadata"));
    EXPECT_FALSE(j.contains("naming_issues"));
    EXPECT_FALSE(j.contains("duplicates"));
    EXPECT_FALSE(j.contains("metrics_issues"));
}

TEST(GitReportToJson, MetadataFieldsAndTypes) {
    AnalysisReport r;
    r.metadata.base_ref = "main";
    r.metadata.target_ref = "feature";
    r.metadata.scope = AnalysisScope::Staged;
    r.metadata.analysis_time_ms = 123;
    auto j = report_to_json(r, "/tmp/proj");
    auto& md = j["metadata"];
    EXPECT_EQ(md["base_ref"].get<std::string>(), "main");
    EXPECT_EQ(md["target_ref"].get<std::string>(), "feature");
    EXPECT_EQ(md["scope"].get<std::string>(), "staged");
    EXPECT_EQ(md["analysis_time_ms"].get<int64_t>(), 123);
    ASSERT_TRUE(md.contains("analyzed_at"));
    EXPECT_TRUE(md["analyzed_at"].is_string());
    EXPECT_GE(md["analyzed_at"].get<std::string>().size(), 20u);
    EXPECT_NE(md["analyzed_at"].get<std::string>().find('T'), std::string::npos);
    EXPECT_EQ(md["analyzed_at"].get<std::string>().back(), 'Z');
}

TEST(GitReportToJson, SummaryFieldsAndTypes) {
    AnalysisReport r;
    r.metadata.scope = AnalysisScope::WIP;
    r.summary.files_changed = 3;
    r.summary.symbols_added = 5;
    r.summary.symbols_modified = 1;
    r.summary.symbols_deleted = 2;
    r.summary.duplicates_found = 4;
    r.summary.naming_issues_found = 7;
    r.summary.metrics_issues_found = 6;
    r.summary.risk_score = 0.75;
    r.summary.top_recommendation = "Fix naming";

    auto j = report_to_json(r, "/tmp/proj");
    auto& s = j["summary"];
    EXPECT_EQ(s["files_changed"].get<int>(), 3);
    EXPECT_EQ(s["symbols_added"].get<int>(), 5);
    EXPECT_EQ(s["symbols_modified"].get<int>(), 1);
    EXPECT_EQ(s["symbols_deleted"].get<int>(), 2);
    EXPECT_EQ(s["duplicates_found"].get<int>(), 4);
    EXPECT_EQ(s["naming_issues_found"].get<int>(), 7);
    EXPECT_EQ(s["metrics_issues_found"].get<int>(), 6);
    EXPECT_DOUBLE_EQ(s["risk_score"].get<double>(), 0.75);
    EXPECT_EQ(s["top_recommendation"].get<std::string>(), "Fix naming");

    // Critical: similarity-shaped numeric fields must be numbers, not strings.
    EXPECT_TRUE(s["risk_score"].is_number());
    EXPECT_FALSE(s["risk_score"].is_string());
}

TEST(GitReportToJson, DuplicateSimilarityIsFloatNotString) {
    auto r = make_report_with_one_duplicate("/tmp/proj");
    auto j = report_to_json(r, "/tmp/proj");
    ASSERT_TRUE(j.contains("duplicates"));
    ASSERT_EQ(j["duplicates"].size(), 1u);
    auto& d = j["duplicates"][0];
    ASSERT_TRUE(d.contains("similarity"));
    // Bug regression guard: similarity must serialize as JSON number.
    EXPECT_TRUE(d["similarity"].is_number());
    EXPECT_FALSE(d["similarity"].is_string());
    EXPECT_DOUBLE_EQ(d["similarity"].get<double>(), 0.95);
}

TEST(GitReportToJson, DuplicatePathsNormalizedToRelative) {
    const std::string root = "/tmp/proj";
    auto r = make_report_with_one_duplicate(root);
    auto j = report_to_json(r, root);
    auto& d = j["duplicates"][0];

    auto new_path = d["new_code"]["file_path"].get<std::string>();
    auto existing_path = d["existing_code"]["file_path"].get<std::string>();

    // Both must be relative; neither must begin with a slash.
    ASSERT_FALSE(new_path.empty());
    ASSERT_FALSE(existing_path.empty());
    EXPECT_NE(new_path.front(), '/') << "new_code.file_path is absolute: " << new_path;
    EXPECT_NE(existing_path.front(), '/') << "existing_code.file_path is absolute: " << existing_path;

    // Both must be the SAME relative path (point at the same file).
    EXPECT_EQ(new_path, existing_path);
    EXPECT_EQ(new_path, "src/foo.cpp");
}

TEST(GitReportToJson, DuplicateLocationContainsAllRequiredFields) {
    auto r = make_report_with_one_duplicate("/tmp/proj");
    auto j = report_to_json(r, "/tmp/proj");
    auto& d = j["duplicates"][0];
    for (const auto* loc_key : {"new_code", "existing_code"}) {
        ASSERT_TRUE(d.contains(loc_key)) << "missing " << loc_key;
        auto& loc = d[loc_key];
        EXPECT_TRUE(loc.contains("file_path"));
        EXPECT_TRUE(loc.contains("start_line"));
        EXPECT_TRUE(loc.contains("end_line"));
        EXPECT_TRUE(loc.contains("symbol_name"));
        EXPECT_TRUE(loc["start_line"].is_number_integer());
        EXPECT_TRUE(loc["end_line"].is_number_integer());
        EXPECT_TRUE(loc["symbol_name"].is_string());
    }

    EXPECT_EQ(d["new_code"]["start_line"].get<int>(), 10);
    EXPECT_EQ(d["new_code"]["end_line"].get<int>(), 20);
    EXPECT_EQ(d["existing_code"]["start_line"].get<int>(), 5);
    EXPECT_EQ(d["existing_code"]["end_line"].get<int>(), 15);
    EXPECT_EQ(d["new_code"]["symbol_name"].get<std::string>(), "foo");
    EXPECT_EQ(d["existing_code"]["symbol_name"].get<std::string>(), "foo");
}

TEST(GitReportToJson, DuplicateHasAllTopFields) {
    auto r = make_report_with_one_duplicate("/tmp/proj");
    auto j = report_to_json(r, "/tmp/proj");
    auto& d = j["duplicates"][0];
    EXPECT_EQ(d["severity"].get<std::string>(), "warning");
    EXPECT_EQ(d["description"].get<std::string>(), "Exact duplicate of foo");
    EXPECT_EQ(d["type"].get<std::string>(), "structural");
    EXPECT_EQ(d["suggestion"].get<std::string>(),
              "Extract common code into a shared function");
}

TEST(GitReportToJson, NamingIssuesEmittedWhenPresent) {
    AnalysisReport r;
    r.metadata.scope = AnalysisScope::WIP;
    NamingFinding n;
    n.severity = FindingSeverity::Warning;
    n.description = "case mismatch";
    n.new_symbol.name = "MyFunc";
    n.new_symbol.type = "function";
    n.new_symbol.file_path = "/tmp/proj/src/x.go";
    n.new_symbol.line = 3;
    n.new_symbol.end_line = 5;
    n.new_symbol.complexity = 1;
    n.new_symbol.lines_of_code = 3;
    n.issue_type = NamingIssueType::CaseMismatch;
    n.issue = "Uses PascalCase";
    n.suggestion = "snake_case";
    r.naming_issues.push_back(std::move(n));

    auto j = report_to_json(r, "/tmp/proj");
    ASSERT_TRUE(j.contains("naming_issues"));
    ASSERT_EQ(j["naming_issues"].size(), 1u);
    auto& it = j["naming_issues"][0];
    EXPECT_EQ(it["severity"].get<std::string>(), "warning");
    EXPECT_EQ(it["issue_type"].get<std::string>(), "case_mismatch");
    EXPECT_EQ(it["issue"].get<std::string>(), "Uses PascalCase");
    EXPECT_EQ(it["suggestion"].get<std::string>(), "snake_case");
    EXPECT_TRUE(it["similar_names"].is_null());

    // new_symbol path must be normalized to relative.
    EXPECT_EQ(it["new_symbol"]["file_path"].get<std::string>(), "src/x.go");
    EXPECT_EQ(it["new_symbol"]["name"].get<std::string>(), "MyFunc");
    EXPECT_EQ(it["new_symbol"]["line"].get<int>(), 3);
    EXPECT_EQ(it["new_symbol"]["end_line"].get<int>(), 5);
}

TEST(GitReportToJson, MetricsIssuesEmittedWhenPresent) {
    AnalysisReport r;
    r.metadata.scope = AnalysisScope::WIP;
    MetricsFinding m;
    m.severity = FindingSeverity::Critical;
    m.description = "complex";
    m.symbol.name = "doStuff";
    m.symbol.type = "function";
    m.symbol.file_path = "/tmp/proj/src/y.go";
    m.symbol.line = 1;
    m.symbol.end_line = 100;
    m.symbol.complexity = 25;
    m.symbol.lines_of_code = 99;
    m.symbol.nesting_depth = 8;
    m.issue_type = MetricsIssueType::HighComplexity;
    m.issue = "Cyclomatic complexity 25";
    m.suggestion = "Refactor";
    r.metrics_issues.push_back(std::move(m));

    auto j = report_to_json(r, "/tmp/proj");
    ASSERT_TRUE(j.contains("metrics_issues"));
    ASSERT_EQ(j["metrics_issues"].size(), 1u);
    auto& it = j["metrics_issues"][0];
    EXPECT_EQ(it["severity"].get<std::string>(), "critical");
    EXPECT_EQ(it["issue_type"].get<std::string>(), "high_complexity");
    EXPECT_EQ(it["symbol"]["file_path"].get<std::string>(), "src/y.go");
    EXPECT_EQ(it["symbol"]["complexity"].get<int>(), 25);
    EXPECT_EQ(it["symbol"]["lines_of_code"].get<int>(), 99);
    EXPECT_EQ(it["symbol"]["nesting_depth"].get<int>(), 8);

    // new_metrics block falls back to symbol counters when no separate
    // metrics pointer was supplied.
    auto& nm = it["new_metrics"];
    EXPECT_EQ(nm["complexity"].get<int>(), 25);
    EXPECT_EQ(nm["lines_of_code"].get<int>(), 99);
    EXPECT_EQ(nm["nesting_depth"].get<int>(), 8);
}

TEST(GitReportToJson, RelativePathInputPassesThrough) {
    // When file_path arrives already relative (e.g. from new_symbols
    // populated by the git iterator), it must not be touched.
    AnalysisReport r;
    r.metadata.scope = AnalysisScope::WIP;
    DuplicateFinding d;
    d.new_code = {"src/foo.cpp", 1, 2, "foo", {}};
    d.existing_code = {"src/foo.cpp", 3, 4, "foo", {}};
    d.similarity = 1.0;
    d.type = "exact";
    r.duplicates.push_back(std::move(d));

    auto j = report_to_json(r, "/some/root");
    EXPECT_EQ(j["duplicates"][0]["new_code"]["file_path"].get<std::string>(),
              "src/foo.cpp");
    EXPECT_EQ(j["duplicates"][0]["existing_code"]["file_path"].get<std::string>(),
              "src/foo.cpp");
}

TEST(GitReportToJson, AbsolutePathOutsideProjectRootIsPreserved) {
    // If the absolute path doesn't live under project_root, normalization
    // would produce a ".." chain. Keep the original path rather than
    // emitting a misleading relative path.
    AnalysisReport r;
    r.metadata.scope = AnalysisScope::WIP;
    DuplicateFinding d;
    d.new_code = {"/var/elsewhere/x.cpp", 1, 2, "x", {}};
    d.existing_code = {"/var/elsewhere/x.cpp", 1, 2, "x", {}};
    d.similarity = 1.0;
    d.type = "exact";
    r.duplicates.push_back(std::move(d));

    auto j = report_to_json(r, "/tmp/proj");
    auto path = j["duplicates"][0]["new_code"]["file_path"].get<std::string>();
    // Either a clean relative (with ..) or original abs is acceptable;
    // what's NOT acceptable is silently dropping the path.
    EXPECT_FALSE(path.empty());
    EXPECT_NE(path.find("x.cpp"), std::string::npos);
}

}  // namespace
}  // namespace git
}  // namespace lci
