#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "helpers/test_git.h"

#include <nlohmann/json.hpp>

#include <lci/core/subprocess.h>
#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/git/analyzer.h>
#include <lci/git/frequency_analyzer.h>
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
    EXPECT_EQ(determine_naming_severity(NamingIssueType::CaseMismatch),
              FindingSeverity::Warning);
    EXPECT_EQ(determine_naming_severity(NamingIssueType::SynonymSplit),
              FindingSeverity::Warning);
    EXPECT_EQ(determine_naming_severity(NamingIssueType::AmbiguousName),
              FindingSeverity::Warning);
    EXPECT_EQ(determine_naming_severity(NamingIssueType::VagueName),
              FindingSeverity::Info);
    EXPECT_EQ(determine_naming_severity(NamingIssueType::VocabularyOutlier),
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

// A repository whose HEAD is a root commit (single commit, no parent) must
// still report that commit's files: `git diff-tree <ref>` prints nothing for a
// root commit without --root, which made scope=commit return files_changed=0
// on single-commit corpora (2026-08-30 validation sweep) — a false zero, the
// exact silent-fallback class Karpathy rule 6 forbids.
TEST(GitProvider, CommitFilesOnRootCommit) {
    namespace fs = std::filesystem;
    fs::path repo = fs::temp_directory_path() /
                    ("lci_git_root_commit_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
    fs::create_directories(repo);
    {
        std::ofstream(repo / "a.go") << "package main\nfunc A() {}\n";
        std::ofstream(repo / "b.go") << "package main\nfunc B() {}\n";
    }
    ASSERT_TRUE(lci::test::run_git(repo, "init -q"));
    ASSERT_TRUE(lci::test::run_git(repo, "add -A"));
    ASSERT_TRUE(lci::test::run_git(
        repo,
        "-c user.email=fixture@lci.test -c user.name=lci-fixture "
        "-c commit.gpgsign=false commit -q -m fixture"));

    Provider p;
    ASSERT_TRUE(Provider::create(repo.string(), p));

    AnalysisParams params = AnalysisParams::defaults();
    params.scope = AnalysisScope::Commit;

    std::vector<ChangedFile> files;
    ASSERT_TRUE(p.get_changed_files(params, files));
    EXPECT_EQ(files.size(), 2u)
        << "root commit's files must be reported (diff-tree needs --root)";

    ScopeSet scope;
    ASSERT_TRUE(p.get_changed_scope(params, scope));
    EXPECT_FALSE(scope.empty())
        << "commit-scope line ranges must be non-empty for a root commit";

    fs::remove_all(repo);
}

// Same root-commit hole as CommitFilesOnRootCommit, one layer down: the
// numstat branch of diff-tree also lacks --root, so a single-commit repo's
// diff stats came back all zero.
TEST(GitProvider, DiffStatsNonZeroOnRootCommit) {
    namespace fs = std::filesystem;
    fs::path repo = fs::temp_directory_path() /
                    ("lci_git_root_stats_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
    fs::create_directories(repo);
    // 2 lines + 3 lines: the reference counts come from the fixture bytes,
    // not from any code under test.
    std::ofstream(repo / "a.go") << "package main\nfunc A() {}\n";
    std::ofstream(repo / "b.go") << "package main\nfunc B() {}\nfunc C() {}\n";
    ASSERT_TRUE(lci::test::run_git(repo, "init -q"));
    ASSERT_TRUE(lci::test::run_git(repo, "add -A"));
    ASSERT_TRUE(lci::test::run_git(
        repo,
        "-c user.email=fixture@lci.test -c user.name=lci-fixture "
        "-c commit.gpgsign=false commit -q -m fixture"));

    Provider p;
    ASSERT_TRUE(Provider::create(repo.string(), p));

    AnalysisParams params = AnalysisParams::defaults();
    params.scope = AnalysisScope::Commit;
    DiffStats stats;
    ASSERT_TRUE(p.get_diff_stats(params, stats));
    EXPECT_EQ(stats.total_added, 5);
    EXPECT_EQ(stats.total_deleted, 0);

    fs::remove_all(repo);
}

// The base ref of a root commit is the empty tree, whose object id depends on
// the repo's object format. The hardcoded SHA-1 id
// (4b825dc642cb6eb9a060e54bf8d69288fbee4904) does not exist in a SHA-256
// repository, so every base-side content read against it failed. Reference:
// `git hash-object -t tree /dev/null` in the fixture repo itself.
TEST(GitProvider, RootCommitBaseRefMatchesRepoObjectFormat) {
    namespace fs = std::filesystem;
    fs::path repo = fs::temp_directory_path() /
                    ("lci_git_sha256_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
    fs::create_directories(repo);
    ASSERT_TRUE(lci::test::run_git(repo, "init -q --object-format=sha256"));
    std::ofstream(repo / "a.go") << "package main\nfunc A() {}\n";
    ASSERT_TRUE(lci::test::run_git(repo, "add -A"));
    ASSERT_TRUE(lci::test::run_git(
        repo,
        "-c user.email=fixture@lci.test -c user.name=lci-fixture "
        "-c commit.gpgsign=false commit -q -m fixture"));

    // Independent reference: raw git, asked for the empty tree id in THIS
    // repo's object format.
    std::string empty_tree;
    ASSERT_TRUE(lci::subprocess::run_capture(
        {"git", "-C", repo.string(), "hash-object", "-t", "tree",
         "/dev/null"},
        "", empty_tree));
    while (!empty_tree.empty() &&
           (empty_tree.back() == '\n' || empty_tree.back() == '\r')) {
        empty_tree.pop_back();
    }
    ASSERT_EQ(empty_tree.size(), 64u) << "SHA-256 empty tree id expected";

    Provider p;
    ASSERT_TRUE(Provider::create(repo.string(), p));
    AnalysisParams params = AnalysisParams::defaults();
    params.scope = AnalysisScope::Commit;
    std::string base;
    ASSERT_TRUE(p.get_base_ref(params, base));
    EXPECT_EQ(base, empty_tree);

    // And the base ref must be usable: reading the file's base content at
    // the empty tree fails because the file does not exist there, but the
    // tree itself must resolve (rev-parse).
    std::string resolved;
    EXPECT_TRUE(lci::subprocess::run_capture(
        {"git", "-C", repo.string(), "rev-parse", "--verify", "--quiet",
         base + "^{tree}}"},
        "", resolved));

    fs::remove_all(repo);
}

// --since was formatted in UTC WITHOUT the trailing Z, and git parses a
// zone-less ISO date in LOCAL time: under TZ=UTC+14 the cutoff slid 14h and
// commits inside the window vanished / outside it appeared. Reference: the
// fixture's commit date is fixed via GIT_COMMITTER_DATE, so the expected
// membership of each window is known exactly.
TEST(GitFrequency, CommitHistoryParsesPipeInAuthorAndQuotedPath) {
    // Reference values are the fixture's raw bytes: the author name we
    // configured ("A|B" — a literal pipe), the pinned commit epoch from
    // GIT_COMMITTER_DATE, and the UTF-8 file name we wrote. A `|`-separated
    // --format shifts every field when the author contains a pipe (the
    // timestamp parse then fails to 0), and without -z git C-quotes the
    // non-ASCII path so it never equals the real file name.
    namespace fs = std::filesystem;
    fs::path repo = fs::temp_directory_path() /
                    ("lci_git_pipe_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
    fs::create_directories(repo);
    std::ofstream(repo / "caf\xC3\xA9" ".go")
        << "package main\nfunc A() {}\n";
    ASSERT_TRUE(lci::test::run_git(repo, "init -q"));
    ASSERT_TRUE(lci::test::run_git(repo, "add -A"));
    setenv("GIT_AUTHOR_DATE", "1768478400 +0000", 1);
    setenv("GIT_COMMITTER_DATE", "1768478400 +0000", 1);
    ASSERT_TRUE(lci::test::run_git(
        repo,
        "-c user.email=a@b -c 'user.name=A|B' "
        "-c commit.gpgsign=false commit -q -m msg1"));
    ASSERT_TRUE(lci::test::run_git(repo, "mv caf\xC3\xA9.go na\xC3\xAFve.go"));
    ASSERT_TRUE(lci::test::run_git(
        repo,
        "-c user.email=a@b -c 'user.name=A|B' "
        "-c commit.gpgsign=false commit -q -m msg2"));
    unsetenv("GIT_AUTHOR_DATE");
    unsetenv("GIT_COMMITTER_DATE");

    // Independent reference: raw git for the head hash.
    std::string head;
    ASSERT_TRUE(lci::subprocess::run_capture(
        {"git", "-C", repo.string(), "rev-parse", "HEAD"}, "", head));
    while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) {
        head.pop_back();
    }

    Provider p;
    ASSERT_TRUE(Provider::create(repo.string(), p));
    HistoryProvider history(p);
    std::vector<CommitInfo> commits;
    ASSERT_TRUE(history.get_commit_history(0, commits));
    ASSERT_EQ(commits.size(), 2u);

    const CommitInfo& c0 = commits[0];  // newest first
    EXPECT_EQ(c0.hash, head);
    EXPECT_EQ(c0.author_name, "A|B");
    EXPECT_EQ(c0.author_email, "a@b");
    EXPECT_EQ(c0.timestamp_epoch, 1768478400);
    EXPECT_EQ(c0.message, "msg2");
    ASSERT_EQ(c0.file_changes.size(), 1u);
    EXPECT_EQ(c0.file_changes[0].path, "na\xC3\xAFve.go");
    EXPECT_EQ(c0.file_changes[0].old_path, "caf\xC3\xA9.go");

    const CommitInfo& c1 = commits[1];
    EXPECT_EQ(c1.timestamp_epoch, 1768478400);
    ASSERT_EQ(c1.file_changes.size(), 1u);
    EXPECT_EQ(c1.file_changes[0].path, "caf\xC3\xA9.go");

    fs::remove_all(repo);
}

// diff.mnemonicPrefix repaints the ---/+++ headers as i//w//c/ (never b/),
// and core.quotePath C-quotes the non-ASCII name — so the +++ path matched
// neither the -z name-status path nor any parsed symbol's file_path, the
// change-scope filter dropped every symbol, and git_analysis reported zero.
TEST(GitAnalysis, MnemonicPrefixAndQuotedPathStillScopesChangedSymbols) {
    namespace fs = std::filesystem;
    fs::path repo = fs::temp_directory_path() /
                    ("lci_git_mnemonic_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
    fs::create_directories(repo);
    ASSERT_TRUE(lci::test::run_git(repo, "init -q"));
    ASSERT_TRUE(lci::test::run_git(repo, "config diff.mnemonicPrefix true"));
    std::ofstream(repo / "caf\xC3\xA9" ".go")
        << "package main\nfunc Original() int { return 1 }\n";
    ASSERT_TRUE(lci::test::run_git(repo, "add -A"));
    ASSERT_TRUE(lci::test::run_git(
        repo,
        "-c user.email=fixture@lci.test -c user.name=lci-fixture "
        "-c commit.gpgsign=false commit -q -m one"));
    std::ofstream(repo / "caf\xC3\xA9" ".go")
        << "package main\nfunc Original() int { return 2 }\n";
    ASSERT_TRUE(lci::test::run_git(repo, "add -A"));
    ASSERT_TRUE(lci::test::run_git(
        repo,
        "-c user.email=fixture@lci.test -c user.name=lci-fixture "
        "-c commit.gpgsign=false commit -q -m two"));

    // Independent reference: raw git says HEAD's diff touches exactly the
    // UTF-8 path, and its diff headers really are mnemonic/quoted (the
    // defect's precondition — otherwise this test discriminates nothing).
    std::string name_status;
    ASSERT_TRUE(lci::subprocess::run_capture(
        {"git", "-C", repo.string(), "diff-tree", "--root",
         "--no-commit-id", "--name-status", "-z", "-r", "HEAD", "--"},
        "", name_status));
    ASSERT_NE(name_status.find("caf\xC3\xA9.go"), std::string::npos);
    std::string raw_diff;
    ASSERT_TRUE(lci::subprocess::run_capture(
        {"git", "-C", repo.string(), "diff", "HEAD~1..HEAD", "--"},
        "", raw_diff));
    ASSERT_EQ(raw_diff.find("+++ b/"), std::string::npos)
        << "diff.mnemonicPrefix must be in effect for this test to bite";

    Provider p;
    ASSERT_TRUE(Provider::create(repo.string(), p));
    Config cfg = make_default_config();
    cfg.project.root = repo.string();
    MasterIndex index(cfg);
    Analyzer analyzer(p, index);

    AnalysisParams params = AnalysisParams::defaults();
    params.scope = AnalysisScope::Commit;  // base_ref empty -> HEAD
    AnalysisReport report;
    ASSERT_TRUE(analyzer.analyze(params, report));
    EXPECT_EQ(report.summary.symbols_modified, 1)
        << "the changed function in café.go must survive change-scoping";
    EXPECT_EQ(report.summary.files_changed, 1);

    fs::remove_all(repo);
}

// file_pattern used to be expanded through ls-files into one argv entry per
// matching file: a large repo's "*.ts" overflowed ARG_MAX (E2BIG; 32KB on
// Windows) and the whole hotspots report failed. The pattern must reach git
// as a single native pathspec. Reference: a fake git earlier in PATH records
// the exact argv the KERNEL delivered — not anything the code under test
// computed.
TEST(GitFrequency, FilePatternReachesGitAsOnePathspecArg) {
    namespace fs = std::filesystem;
    auto stamp = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path base = fs::temp_directory_path() / ("lci_git_pathspec_" + stamp);
    fs::path repo = base / "repo";
    fs::create_directories(repo);
    ASSERT_TRUE(lci::test::run_git(repo, "init -q"));
    // 5000 tracked .go files: the old expansion put 5000 entries in argv.
    for (int i = 0; i < 5000; ++i) {
        std::ofstream(repo / ("f" + std::to_string(i) + ".go"))
            << "package main\n";
    }
    ASSERT_TRUE(lci::test::run_git(repo, "add -A"));
    ASSERT_TRUE(lci::test::run_git(
        repo,
        "-c user.email=fixture@lci.test -c user.name=lci-fixture "
        "-c commit.gpgsign=false commit -q -m fixture"));

    // Fake git: logs subcommand + argc + every argument, then execs the
    // real git so results stay real.
    std::string real_git;
    ASSERT_TRUE(lci::subprocess::run_capture({"which", "git"}, "", real_git));
    while (!real_git.empty() &&
           (real_git.back() == '\n' || real_git.back() == '\r')) {
        real_git.pop_back();
    }
    fs::path bindir = base / "bin";
    fs::create_directories(bindir);
    fs::path argv_log = base / "argv.log";
    {
        std::ofstream shim(bindir / "git");
        shim << "#!/bin/sh\n"
             << "printf '%s' \"$1\" >> \"" << argv_log.string() << "\"\n"
             << "printf ' argc=%s' \"$#\" >> \"" << argv_log.string() << "\"\n"
             << "for a in \"$@\"; do printf ' [%s]' \"$a\"; done >> \""
             << argv_log.string() << "\"\n"
             << "printf '\\n' >> \"" << argv_log.string() << "\"\n"
             << "exec " << real_git << " \"$@\"\n";
        fs::permissions(bindir / "git", fs::perms::owner_all,
                        fs::perm_options::add);
    }
    std::string saved_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    setenv("PATH", (bindir.string() + ":" + saved_path).c_str(), 1);

    Provider p;
    bool created = Provider::create(repo.string(), p);
    std::vector<CommitInfo> commits;
    std::string notice;
    bool ok = false;
    if (created) {
        HistoryProvider history(p);
        ok = history.get_repo_history(0, "*.go", commits, &notice);
    }

    setenv("PATH", saved_path.c_str(), 1);
    ASSERT_TRUE(created);
    ASSERT_TRUE(ok) << notice;
    EXPECT_FALSE(commits.empty());

    // Read the kernel-delivered argv of the `log` invocation.
    std::ifstream log(argv_log);
    std::string line;
    bool found_log = false;
    while (std::getline(log, line)) {
        if (line.rfind("log ", 0) == 0) {
            found_log = true;
            EXPECT_NE(line.find("[*.go]"), std::string::npos)
                << "pattern must be one verbatim pathspec arg: " << line;
            // Bounded argv: subcommand + a handful of flags, never one arg
            // per matched file.
            EXPECT_LT(line.size(), 2000u) << line.substr(0, 200);
        }
    }
    EXPECT_TRUE(found_log);

    fs::remove_all(base);
}

#if !defined(_WIN32)
// Hotspots (and collisions/ownership) were sorted on the score alone from
// flat_hash_map iteration order; with every score tied, the emitted order
// followed the per-process abseil hash salt. Each child below is a SEPARATE
// process (own salt) that rebuilds the same fixture and prints the report;
// the parent byte-compares the five outputs. A loop in one process would
// prove nothing — one process, one salt.
TEST(GitFrequency, HotspotsReportByteIdenticalAcrossProcesses) {
    namespace fs = std::filesystem;
    fs::path base = fs::temp_directory_path() /
                    ("lci_git_det_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
    fs::create_directories(base);

    std::vector<std::string> outputs;
    for (int run = 0; run < 5; ++run) {
        fs::path out_file = base / ("run" + std::to_string(run) + ".txt");
        pid_t pid = fork();
        ASSERT_NE(pid, -1);
        if (pid == 0) {
            // Child: fresh process, fresh hash salt. Build the fixture from
            // scratch so nothing is inherited from the parent's mappings.
            fs::path repo = base / ("repo" + std::to_string(run));
            fs::create_directories(repo);
            bool ok = lci::test::run_git(repo, "init -q");
            // 40 files, one commit, one change each: every volatility
            // score ties, so output order is total-order tiebreak or hash
            // order — there is no third option.
            for (int i = 0; i < 40; ++i) {
                std::ofstream(repo / ("m" + std::to_string(i) + ".go"))
                    << "package main\nfunc F" << i << "() {}\n";
            }
            ok = ok && lci::test::run_git(repo, "add -A");
            ok = ok && lci::test::run_git(
                repo,
                "-c user.email=fixture@lci.test -c user.name=lci-fixture "
                "-c commit.gpgsign=false commit -q -m fixture");
            std::string text;
            Provider p;
            ok = ok && Provider::create(repo.string(), p);
            if (ok) {
                FrequencyAnalyzer analyzer(p);
                ChangeFrequencyParams params;
                params.min_changes = 1;
                params.top_n = 50;
                ChangeFrequencyReport report;
                ok = analyzer.analyze(params, report);
                if (ok) {
                    for (const auto& h : report.hotspots) {
                        text += h.file_path;
                        text += '\n';
                    }
                    text += "--collisions--\n";
                    for (const auto& c : report.collisions) {
                        text += c.path;
                        text += '\n';
                    }
                    text += "--ownership--\n";
                    for (const auto& o : report.ownership) {
                        text += o.module_path;
                        text += '\n';
                        text += o.primary_owner.author_email;
                        text += '\n';
                    }
                    text += "--contributor--\n";
                    text += report.summary.most_active_contributor;
                    text += '\n';
                }
            }
            std::ofstream(out_file, std::ios::binary) << (ok ? text : "FAIL");
            std::_Exit(ok ? 0 : 2);
        }
        int status = 0;
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
            << "child run " << run << " failed";
        std::ifstream ifs(out_file, std::ios::binary);
        outputs.emplace_back(std::istreambuf_iterator<char>(ifs),
                             std::istreambuf_iterator<char>());
        ASSERT_NE(outputs.back(), "FAIL");
        ASSERT_FALSE(outputs.back().empty());
    }

    for (size_t i = 1; i < outputs.size(); ++i) {
        EXPECT_EQ(outputs[i], outputs[0])
            << "run " << i << " differs from run 0 (hash-order leak)";
    }
    fs::remove_all(base);
}
#endif

TEST(GitFrequency, SinceIsInterpretedAsUtcRegardlessOfLocalTz) {
    namespace fs = std::filesystem;
    fs::path repo = fs::temp_directory_path() /
                    ("lci_git_since_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
    fs::create_directories(repo);
    std::ofstream(repo / "a.go") << "package main\nfunc A() {}\n";
    ASSERT_TRUE(lci::test::run_git(repo, "init -q"));
    ASSERT_TRUE(lci::test::run_git(repo, "add -A"));
    // Fixed commit instant: 2026-01-15T12:00:00Z = epoch 1768478400. Both
    // dates pinned via the environment (children of std::system inherit).
    constexpr int64_t kCommitEpoch = 1768478400;
    setenv("GIT_AUTHOR_DATE", "1768478400 +0000", 1);
    setenv("GIT_COMMITTER_DATE", "1768478400 +0000", 1);
    ASSERT_TRUE(lci::test::run_git(
        repo,
        "-c user.email=fixture@lci.test -c user.name=lci-fixture "
        "-c commit.gpgsign=false commit -q -m fixture"));
    unsetenv("GIT_AUTHOR_DATE");
    unsetenv("GIT_COMMITTER_DATE");

    // Worst-case zone: UTC+14 (Kiritimati).
    std::string saved_tz;
    if (const char* tz = std::getenv("TZ")) saved_tz = tz;
    setenv("TZ", "Pacific/Kiritimati", 1);
    tzset();

    Provider p;
    ASSERT_TRUE(Provider::create(repo.string(), p));
    HistoryProvider history(p);

    std::vector<CommitInfo> commits;
    // Window ending just AFTER the commit: must contain it.
    ASSERT_TRUE(history.get_commit_history(kCommitEpoch - 60, commits));
    EXPECT_EQ(commits.size(), 1u)
        << "commit at T must be inside a since=T-60 window under any TZ";
    commits.clear();
    ASSERT_TRUE(history.get_commit_history(kCommitEpoch + 60, commits));
    EXPECT_TRUE(commits.empty())
        << "commit at T must be outside a since=T+60 window under any TZ";

    if (saved_tz.empty()) {
        unsetenv("TZ");
    } else {
        setenv("TZ", saved_tz.c_str(), 1);
    }
    tzset();
    fs::remove_all(repo);
}

// WORKING-ref content reads bypass git and hit the filesystem directly, so
// they must be confined to the repository: a "../" (or absolute) path used to
// read any file the process could. The git-ref paths never had this hole —
// git resolves "<ref>:<path>" inside the repo itself.
TEST(GitProvider, GetFileContentWorkingRefusesPathTraversal) {
    namespace fs = std::filesystem;
    fs::path base = fs::temp_directory_path() /
                    ("lci_git_traversal_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
    fs::path repo = base / "repo";
    fs::create_directories(repo);
    // Binary mode: a text-mode stream writes CRLF on Windows and the exact
    // content assertion below then fails on the line ending, not the guard.
    std::ofstream(repo / "inside.txt", std::ios::binary) << "inside\n";
    // A real, readable file OUTSIDE the repo: the refusal below is about
    // confinement, not absence.
    std::ofstream(base / "secret.txt", std::ios::binary) << "outside\n";
    ASSERT_TRUE(lci::test::run_git(repo, "init -q"));

    Provider p;
    ASSERT_TRUE(Provider::create(repo.string(), p));

    std::string content;
    EXPECT_TRUE(p.get_file_content("WORKING", "inside.txt", content));
    EXPECT_EQ(content, "inside\n");

    content.clear();
    EXPECT_FALSE(p.get_file_content("WORKING", "../secret.txt", content))
        << "dot-dot traversal out of the repo must be refused";
    EXPECT_FALSE(p.get_file_content("WORKING",
                                    (base / "secret.txt").string(), content))
        << "absolute path out of the repo must be refused";

    fs::remove_all(base);
}

// An UNTRACKED subdirectory nested inside some outer repository (the benchmark
// corpora under .work/, any gitignored playground) must not silently analyze
// the enclosing repo: rev-parse walks up, finds the outer toplevel, and every
// scope then reports a clean "no changes" about a repo the project root has no
// files in (2026-08-30 validation sweep, guzzle corpus). tracks_any is the
// gate the handlers use to tell that apart from a legitimate monorepo subdir.
TEST(GitProvider, TracksAnyDistinguishesUntrackedNesting) {
    namespace fs = std::filesystem;
    fs::path outer = fs::temp_directory_path() /
                     ("lci_git_nested_" +
                      std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count()));
    fs::create_directories(outer / "tracked");
    fs::create_directories(outer / "untracked");
    std::ofstream(outer / "tracked" / "a.go") << "package a\n";
    std::ofstream(outer / ".gitignore") << "untracked/\n";
    std::ofstream(outer / "untracked" / "b.go") << "package b\n";
    ASSERT_TRUE(lci::test::run_git(outer, "init -q"));
    ASSERT_TRUE(lci::test::run_git(outer, "add -A"));
    ASSERT_TRUE(lci::test::run_git(
        outer,
        "-c user.email=fixture@lci.test -c user.name=lci-fixture "
        "-c commit.gpgsign=false commit -q -m fixture"));

    Provider p;
    ASSERT_TRUE(Provider::create((outer / "untracked").string(), p));
    // create() resolves to the OUTER toplevel — that is the trap. Compare
    // canonicalized: rev-parse resolves symlinks, and macOS temp dirs live
    // behind one (/var/folders -> /private/var/folders).
    EXPECT_EQ(fs::path(p.repo_root()), fs::weakly_canonical(outer));
    EXPECT_FALSE(p.tracks_any((outer / "untracked").string()))
        << "gitignored corpus dir must not count as covered by the repo";
    EXPECT_TRUE(p.tracks_any((outer / "tracked").string()))
        << "a real monorepo subdir stays analyzable";

    fs::remove_all(outer);
}

// ============================================================================
// name-status (-z) parsing + ref hardening
// ============================================================================

TEST(GitProviderParse, NameStatusZKeepsPathsWithSpaces) {
    // The whitespace split truncated this path at the first space, so the
    // file vanished from the analysis entirely.
    std::string out_z = std::string("M") + '\0' + "src/my file.go" + '\0';
    std::vector<ChangedFile> files;
    ASSERT_TRUE(parse_name_status(out_z, files));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].path, "src/my file.go");
    EXPECT_EQ(files[0].status, FileChangeStatus::Modified);
}

TEST(GitProviderParse, NameStatusZKeepsNonAsciiPathsRaw) {
    // -z emits raw bytes, so core.quotePath escaping never appears and needs
    // no unquoting. A quoted name would have been kept verbatim before.
    std::string out_z = std::string("A") + '\0' + "src/caf\xc3\xa9.go" + '\0';
    std::vector<ChangedFile> files;
    ASSERT_TRUE(parse_name_status(out_z, files));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].path, "src/caf\xc3\xa9.go");
    EXPECT_EQ(files[0].status, FileChangeStatus::Added);
}

TEST(GitProviderParse, NameStatusZRenameCarriesBothPaths) {
    std::string out_z = std::string("R100") + '\0' + "old name.go" + '\0' +
                        "new name.go" + '\0' + "M" + '\0' + "other.go" + '\0';
    std::vector<ChangedFile> files;
    ASSERT_TRUE(parse_name_status(out_z, files));
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].status, FileChangeStatus::Renamed);
    EXPECT_EQ(files[0].old_path, "old name.go");
    EXPECT_EQ(files[0].path, "new name.go");
    EXPECT_EQ(files[1].path, "other.go");
}

TEST(GitProviderParse, NameStatusZEmptyInput) {
    std::vector<ChangedFile> files;
    ASSERT_TRUE(parse_name_status("", files));
    EXPECT_TRUE(files.empty());
}

TEST(GitProviderParse, RefsBeginningWithDashAreRejected) {
    // Refs reach git as positional argv; without this screen a base_ref of
    // "--output=/tmp/x" is honoured as a flag by the subcommand.
    EXPECT_FALSE(is_safe_ref("--output=/tmp/pwned"));
    EXPECT_FALSE(is_safe_ref("-n"));
    EXPECT_FALSE(is_safe_ref(""));
    EXPECT_TRUE(is_safe_ref("HEAD"));
    EXPECT_TRUE(is_safe_ref("v1.2.3"));
    EXPECT_TRUE(is_safe_ref("feature/my-branch"));
}

TEST(GitProvider, GetChangedFilesRejectsOptionLikeRefs) {
    Provider p;
    if (!Provider::create(".", p)) GTEST_SKIP() << "not a git repo";

    AnalysisParams params = AnalysisParams::defaults();
    params.scope = AnalysisScope::Commit;
    params.base_ref = "--output=/tmp/lci-should-not-exist";
    std::vector<ChangedFile> files;
    EXPECT_FALSE(p.get_changed_files(params, files));

    params.scope = AnalysisScope::Range;
    params.base_ref = "--upload-pack=/bin/false";
    params.target_ref = "HEAD";
    files.clear();
    EXPECT_FALSE(p.get_changed_files(params, files));
}

TEST(GitProvider, GetCommitHashRejectsUnknownRef) {
    Provider p;
    if (!Provider::create(".", p)) GTEST_SKIP() << "not a git repo";
    std::string hash;
    EXPECT_FALSE(p.get_commit_hash("no-such-ref-xyzzy-1234", hash));
    EXPECT_TRUE(p.get_commit_hash("HEAD", hash));
    EXPECT_EQ(hash.size(), 40u);
}

TEST(GitProvider, BaseRefOfUnknownCommitIsAnErrorNotTheEmptyTree) {
    // get_parent_commit used to read ANY rev-parse failure as "first commit"
    // and substitute the empty-tree hash, so a bad ref produced a plausible
    // report in which the whole repository looked newly added.
    Provider p;
    if (!Provider::create(".", p)) GTEST_SKIP() << "not a git repo";

    AnalysisParams params = AnalysisParams::defaults();
    params.scope = AnalysisScope::Commit;
    params.base_ref = "no-such-ref-xyzzy-1234";
    std::string base;
    EXPECT_FALSE(p.get_base_ref(params, base));
    EXPECT_NE(base, "4b825dc642cb6eb9a060e54bf8d69288fbee4904");
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

TEST(GitTypes, MetricsIssueTypeToString) {
    EXPECT_EQ(to_string(MetricsIssueType::HighComplexity), "high_complexity");
    EXPECT_EQ(to_string(MetricsIssueType::LongFunction), "long_function");
    EXPECT_EQ(to_string(MetricsIssueType::DeepNesting), "deep_nesting");
    EXPECT_EQ(to_string(MetricsIssueType::ComplexityGrew), "complexity_grew");
    EXPECT_EQ(to_string(MetricsIssueType::ImpureFunction), "impure_function");
}

TEST(GitTypes, NamingIssueTypeToString) {
    EXPECT_EQ(to_string(NamingIssueType::CaseMismatch), "case_mismatch");
    EXPECT_EQ(to_string(NamingIssueType::SynonymSplit), "synonym_split");
    EXPECT_EQ(to_string(NamingIssueType::AmbiguousName), "ambiguous_name");
    EXPECT_EQ(to_string(NamingIssueType::VagueName), "vague_name");
    EXPECT_EQ(to_string(NamingIssueType::VocabularyOutlier),
              "vocabulary_outlier");
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

// The duplicate finder pre-tokenizes each symbol once; the set-based path
// must agree exactly with the string_view convenience API.
TEST(GitAnalyzer, TokenSetSimilarityMatchesStringApi) {
    std::string a = "int foo(int x) { return x + 1; }";
    std::string b = "int bar(int y) { return y + 1; }";
    EXPECT_DOUBLE_EQ(token_set_similarity(code_token_set(a), code_token_set(b)),
                     code_structural_similarity(a, b));
    EXPECT_DOUBLE_EQ(token_set_similarity(code_token_set(a), code_token_set(a)),
                     1.0);
    EXPECT_DOUBLE_EQ(token_set_similarity({}, code_token_set(a)), 0.0);
}

// ============================================================================
// Naming findings from the corpus-wide NamingAnalyzer report
// ============================================================================

namespace {

SymbolInfo make_changed_symbol(std::string name) {
    SymbolInfo s;
    s.name = std::move(name);
    s.type = "function";
    s.file_path = "src/x.go";
    s.line = 10;
    s.end_line = 20;
    return s;
}

}  // namespace

TEST(GitAnalyzer, NamingFindingsFilterReportToChangedSymbols) {
    NamingReport report;

    SynonymSplit split;
    split.canonical = "load_user";
    split.members = {{"fetchUser", "a.go:1", 7}, {"loadUser", "b.go:2", 3}};
    report.synonym_splits.push_back(split);

    report.ambiguous_names.push_back({"loadUser", 6});
    report.information.vague_names.push_back({"process", 1.0, 40.0, 3});

    VocabularyOutlier outlier;
    outlier.name = "frobUser";
    outlier.odd_term = "frob";
    outlier.reason = "unknown-verb";
    outlier.suggested = {"transform"};
    report.outliers.push_back(outlier);

    auto load_user = make_changed_symbol("loadUser");
    auto frob_user = make_changed_symbol("frobUser");
    auto untouched = make_changed_symbol("unrelatedName");
    std::vector<const SymbolInfo*> changed = {&load_user, &frob_user,
                                              &untouched};

    std::vector<NamingFinding> out;
    naming_findings_from_report(report, changed, out);

    ASSERT_EQ(out.size(), 3u);

    EXPECT_EQ(out[0].issue_type, NamingIssueType::SynonymSplit);
    EXPECT_EQ(out[0].new_symbol.name, "loadUser");
    EXPECT_EQ(out[0].severity, FindingSeverity::Warning);
    EXPECT_NE(out[0].issue.find("fetchUser"), std::string::npos);
    EXPECT_NE(out[0].suggestion.find("fetchUser"), std::string::npos);

    EXPECT_EQ(out[1].issue_type, NamingIssueType::AmbiguousName);
    EXPECT_EQ(out[1].new_symbol.name, "loadUser");
    EXPECT_NE(out[1].issue.find("6"), std::string::npos);

    EXPECT_EQ(out[2].issue_type, NamingIssueType::VocabularyOutlier);
    EXPECT_EQ(out[2].new_symbol.name, "frobUser");
    EXPECT_EQ(out[2].severity, FindingSeverity::Info);
    EXPECT_NE(out[2].suggestion.find("transform"), std::string::npos);
}

TEST(GitAnalyzer, NamingFindingsVagueNameMatchesOnlyChanged) {
    NamingReport report;
    report.information.vague_names.push_back({"process", 1.0, 40.0, 3});
    report.information.vague_names.push_back({"handle", 1.0, 55.0, 2});

    auto process = make_changed_symbol("process");
    std::vector<const SymbolInfo*> changed = {&process};

    std::vector<NamingFinding> out;
    naming_findings_from_report(report, changed, out);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].issue_type, NamingIssueType::VagueName);
    EXPECT_EQ(out[0].new_symbol.name, "process");
    EXPECT_EQ(out[0].severity, FindingSeverity::Info);
    EXPECT_NE(out[0].issue.find("40"), std::string::npos);
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

TEST(ChurnFilter, MonorepoPackagesNotExcluded) {
    // "packages/" is the first-party layout of every pnpm/yarn/lerna
    // monorepo. Excluding it by default hid the churn of the code the
    // report exists to rank.
    EXPECT_FALSE(should_exclude_from_churn("packages/core/src/index.ts"));
    EXPECT_FALSE(should_exclude_from_churn("packages/ui/button.tsx"));
    // Third-party trees nested inside it are still excluded.
    EXPECT_TRUE(
        should_exclude_from_churn("packages/core/node_modules/dep/index.js"));
}

TEST(ChurnFilter, DoubleStarPrefixPatterns) {
    // "**/<x>" never matched under the old hand-rolled matcher.
    EXPECT_TRUE(should_exclude_from_churn("a/b/gen.go", {}, {"**/gen.go"},
                                          /*skip_defaults=*/true));
    EXPECT_TRUE(should_exclude_from_churn("gen.go", {}, {"**/gen.go"}, true));
    EXPECT_FALSE(should_exclude_from_churn("a/b/keep.go", {}, {"**/gen.go"},
                                           true));
}

TEST(ChurnFilter, QuestionMarkAndCharClassPatterns) {
    // '?' and '[]' were treated as literals and matched nothing.
    EXPECT_TRUE(should_exclude_from_churn("a.c", {}, {"*.[ch]"}, true));
    EXPECT_TRUE(should_exclude_from_churn("a.h", {}, {"*.[ch]"}, true));
    EXPECT_FALSE(should_exclude_from_churn("a.o", {}, {"*.[ch]"}, true));

    EXPECT_TRUE(should_exclude_from_churn("v1.go", {}, {"v?.go"}, true));
    EXPECT_FALSE(should_exclude_from_churn("v12.go", {}, {"v?.go"}, true));
}

TEST(ChurnFilter, SkipDefaults) {
    // With skip_defaults, normally excluded files are included.
    EXPECT_FALSE(should_exclude_from_churn("CHANGELOG.md", {}, {}, true));
    EXPECT_FALSE(should_exclude_from_churn("docs/readme.md", {}, {}, true));
}

// ============================================================================
// Commit history parsing
// ============================================================================

namespace {

// Two commits, each with one numstat entry, in the exact shape
// `git log --numstat -z --format=%H%x00%an%x00%ae%x00%at%x00%s` emits:
// NUL-terminated header fields, then '\n'-prefixed numstat entries.
constexpr char kTwoCommitLog[] =
    "1111111111111111111111111111111111111111\0Ann\0ann@example.com\0"
    "1700000000\0first\0\n10\t2\tsrc/a.go\0"
    "2222222222222222222222222222222222222222\0Bob\0bob@example.com\0"
    "1700000100\0second\0\n3\t4\tsrc/b.go\0";

}  // namespace

TEST(CommitHistoryParse, HeaderCountEqualsCommitCount) {
    // Regression: the parser re-pushed the previous commit on every new
    // header, so N commits yielded 2N-1 entries. Every churn statistic was
    // inflated and the moved-from husks became phantom empty-name authors.
    std::vector<CommitInfo> commits;
    ASSERT_TRUE(parse_commit_history(
        std::string_view{kTwoCommitLog, sizeof(kTwoCommitLog) - 1}, commits));
    ASSERT_EQ(commits.size(), 2u);

    EXPECT_EQ(commits[0].author_name, "Ann");
    EXPECT_EQ(commits[0].message, "first");
    EXPECT_EQ(commits[0].timestamp_epoch, 1700000000);
    ASSERT_EQ(commits[0].file_changes.size(), 1u);
    EXPECT_EQ(commits[0].file_changes[0].path, "src/a.go");
    EXPECT_EQ(commits[0].file_changes[0].lines_added, 10);

    EXPECT_EQ(commits[1].author_name, "Bob");
    EXPECT_EQ(commits[1].message, "second");
    ASSERT_EQ(commits[1].file_changes.size(), 1u);
    EXPECT_EQ(commits[1].file_changes[0].path, "src/b.go");

    // No phantom contributor with an empty name.
    for (const auto& c : commits) {
        EXPECT_FALSE(c.author_name.empty());
        EXPECT_FALSE(c.hash.empty());
    }
}

TEST(CommitHistoryParse, SingleCommitYieldsOne) {
    constexpr char kLog[] =
        "3333333333333333333333333333333333333333\0Cy\0cy@example.com\0"
        "1700000200\0only\0\n1\t1\tx.go\0";
    std::vector<CommitInfo> commits;
    ASSERT_TRUE(parse_commit_history(
        std::string_view{kLog, sizeof(kLog) - 1}, commits));
    EXPECT_EQ(commits.size(), 1u);
}

TEST(CommitHistoryParse, EmptyOutputYieldsNoCommits) {
    std::vector<CommitInfo> commits;
    ASSERT_TRUE(parse_commit_history("", commits));
    EXPECT_TRUE(commits.empty());
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
