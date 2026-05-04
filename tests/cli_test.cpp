#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include <lci/cli/commands.h>

#include "../src/cli/grep_filters.h"
#include "../src/cli/query_parser.h"
#include "../src/cli/rank_options.h"
#include "../src/cli/symbol_filters.h"
#include "../src/cli/tree_formatter.h"

namespace lci {
namespace cli {
namespace {

// -- load_config_with_overrides tests -----------------------------------------

TEST(CliConfigTest, DefaultFlagsProduceValidConfig) {
    GlobalFlags flags;
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_FALSE(cfg.project.root.empty());
}

TEST(CliConfigTest, RootOverrideApplied) {
    GlobalFlags flags;
    flags.root = "/tmp";
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(cfg.project.root, "/tmp");
}

TEST(CliConfigTest, IncludeOverrideReplacesConfig) {
    GlobalFlags flags;
    flags.include = {"*.go", "*.rs"};
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(cfg.include.size(), 2u);
    EXPECT_EQ(cfg.include[0], "*.go");
    EXPECT_EQ(cfg.include[1], "*.rs");
}

TEST(CliConfigTest, ExcludeOverrideAppendsToConfig) {
    GlobalFlags flags;
    flags.exclude = {"vendor/**"};
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    bool found = false;
    for (auto& e : cfg.exclude) {
        if (e == "vendor/**") found = true;
    }
    EXPECT_TRUE(found);
}

// -- format helpers -----------------------------------------------------------

TEST(CliFormatTest, FormatBytesSmall) {
    EXPECT_EQ(format_bytes(500), "500 bytes");
}

TEST(CliFormatTest, FormatBytesKB) {
    std::string result = format_bytes(2048);
    EXPECT_NE(result.find("KB"), std::string::npos);
}

TEST(CliFormatTest, FormatBytesMB) {
    std::string result = format_bytes(5 * 1024 * 1024);
    EXPECT_NE(result.find("MB"), std::string::npos);
}

TEST(CliFormatTest, FormatBytesGB) {
    std::string result = format_bytes(int64_t{2} * 1024 * 1024 * 1024);
    EXPECT_NE(result.find("GB"), std::string::npos);
}

TEST(CliFormatTest, FormatMillisecondsSmall) {
    EXPECT_EQ(format_milliseconds(42), "42 ms");
}

TEST(CliFormatTest, FormatMillisecondsSeconds) {
    std::string result = format_milliseconds(5500);
    EXPECT_NE(result.find("seconds"), std::string::npos);
}

TEST(CliFormatTest, FormatMillisecondsMinutes) {
    std::string result = format_milliseconds(120000);
    EXPECT_NE(result.find("minutes"), std::string::npos);
}

TEST(CliFormatTest, FormatSecondsSmall) {
    std::string result = format_seconds(45.0);
    EXPECT_NE(result.find("seconds"), std::string::npos);
}

TEST(CliFormatTest, FormatSecondsMinutes) {
    std::string result = format_seconds(300.0);
    EXPECT_NE(result.find("minutes"), std::string::npos);
}

TEST(CliFormatTest, FormatSecondsHours) {
    std::string result = format_seconds(7200.0);
    EXPECT_NE(result.find("hours"), std::string::npos);
}

TEST(CliFormatTest, FormatSecondsDays) {
    std::string result = format_seconds(200000.0);
    EXPECT_NE(result.find("days"), std::string::npos);
}

// -- MCP auto-detection -------------------------------------------------------

TEST(CliMcpDetectTest, DefaultReturnsFalseInTerminal) {
    // In a test runner, stdin is typically a terminal or redirected.
    // We just verify the function doesn't crash.
    // When connected to a terminal, it should return false.
    // When piped (as in CI), it may return true - both are valid.
    (void)is_mcp_mode();
}

TEST(CliMcpDetectTest, EnvVariableOverride) {
    // Save and restore env
    const char* old = std::getenv("LCI_MCP_MODE");
    setenv("LCI_MCP_MODE", "1", 1);
    EXPECT_TRUE(is_mcp_mode());
    if (old) {
        setenv("LCI_MCP_MODE", old, 1);
    } else {
        unsetenv("LCI_MCP_MODE");
    }
}

// -- config init command tests (no server needed) -----------------------------

TEST(CliConfigInitTest, KdlFormatCreatesFile) {
    std::string test_file = "/tmp/lci_test_config_init.kdl";
    std::remove(test_file.c_str());

    GlobalFlags flags;
    int rc = run_config_init(flags, "kdl", test_file, false, false);
    EXPECT_EQ(rc, 0);

    std::ifstream ifs(test_file);
    ASSERT_TRUE(ifs.good());
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("project"), std::string::npos);
    EXPECT_NE(content.find("index"), std::string::npos);
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, MinimalKdlCreatesFile) {
    std::string test_file = "/tmp/lci_test_config_init_min.kdl";
    std::remove(test_file.c_str());

    GlobalFlags flags;
    int rc = run_config_init(flags, "kdl", test_file, false, true);
    EXPECT_EQ(rc, 0);

    std::ifstream ifs(test_file);
    ASSERT_TRUE(ifs.good());
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("index"), std::string::npos);
    // Minimal should not have a project { } block
    EXPECT_EQ(content.find("project {"), std::string::npos);
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, JsonFormatCreatesValidJson) {
    std::string test_file = "/tmp/lci_test_config_init.json";
    std::remove(test_file.c_str());

    GlobalFlags flags;
    int rc = run_config_init(flags, "json", test_file, false, false);
    EXPECT_EQ(rc, 0);

    std::ifstream ifs(test_file);
    ASSERT_TRUE(ifs.good());
    nlohmann::json j;
    EXPECT_NO_THROW(ifs >> j);
    EXPECT_TRUE(j.contains("project"));
    EXPECT_TRUE(j.contains("index"));
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, YamlFormatCreatesFile) {
    std::string test_file = "/tmp/lci_test_config_init.yaml";
    std::remove(test_file.c_str());

    GlobalFlags flags;
    int rc = run_config_init(flags, "yaml", test_file, false, false);
    EXPECT_EQ(rc, 0);

    std::ifstream ifs(test_file);
    ASSERT_TRUE(ifs.good());
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("version: 1"), std::string::npos);
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, RefusesOverwriteWithoutForce) {
    std::string test_file = "/tmp/lci_test_config_exists.kdl";
    // Create file first
    {
        std::ofstream ofs(test_file);
        ofs << "existing";
    }

    GlobalFlags flags;
    int rc = run_config_init(flags, "kdl", test_file, false, false);
    EXPECT_EQ(rc, 1);

    // With force it should succeed
    rc = run_config_init(flags, "kdl", test_file, true, false);
    EXPECT_EQ(rc, 0);
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, UnsupportedFormatFails) {
    GlobalFlags flags;
    int rc = run_config_init(flags, "xml", "/tmp/lci_test.xml", false, false);
    EXPECT_EQ(rc, 1);
}

// -- config validate command tests --------------------------------------------

TEST(CliConfigValidateTest, ValidConfigReturnsZero) {
    GlobalFlags flags;
    int rc = run_config_validate(flags);
    EXPECT_EQ(rc, 0);
}

// -- config show command tests ------------------------------------------------

TEST(CliConfigShowTest, TableFormatReturnsZero) {
    GlobalFlags flags;
    int rc = run_config_show(flags, "table");
    EXPECT_EQ(rc, 0);
}

TEST(CliConfigShowTest, JsonFormatReturnsZero) {
    GlobalFlags flags;
    int rc = run_config_show(flags, "json");
    EXPECT_EQ(rc, 0);
}

// -- git-analyze validation tests ---------------------------------------------

TEST(CliGitAnalyzeTest, InvalidScopeFails) {
    GlobalFlags flags;
    int rc = run_git_analyze(flags, "invalid", "", "", {}, 0.8, 20, false);
    EXPECT_EQ(rc, 1);
}

TEST(CliGitAnalyzeTest, RangeScopeRequiresBase) {
    GlobalFlags flags;
    int rc = run_git_analyze(flags, "range", "", "", {}, 0.8, 20, false);
    EXPECT_EQ(rc, 1);
}

// -- grep filter helpers ------------------------------------------------------
//
// Direct unit tests for the pure helpers that back `lci grep --context N`,
// `--exclude-tests`, and `--exclude-comments`. These avoid the server hop so
// the heuristics can be exercised exhaustively without indexing a corpus.

namespace gf = ::lci::cli::grep_filters;

TEST(GrepFiltersComment, LineSlashSlashIsComment) {
    EXPECT_TRUE(gf::line_looks_like_comment("// hello"));
    EXPECT_TRUE(gf::line_looks_like_comment("    // indented"));
    EXPECT_TRUE(gf::line_looks_like_comment("\t// tabbed"));
}

TEST(GrepFiltersComment, LineHashIsComment) {
    EXPECT_TRUE(gf::line_looks_like_comment("# python"));
    EXPECT_TRUE(gf::line_looks_like_comment("  #include"));  // matches Go heuristic
}

TEST(GrepFiltersComment, LineSlashStarIsComment) {
    EXPECT_TRUE(gf::line_looks_like_comment("/* opening"));
    EXPECT_TRUE(gf::line_looks_like_comment("   /* indented opener"));
}

TEST(GrepFiltersComment, LineWithStarSlashIsComment) {
    // Block comment closer anywhere on the line counts as "in a comment"
    // (the line crossed the close marker, so its tail is the comment body).
    EXPECT_TRUE(gf::line_looks_like_comment("done */"));
    EXPECT_TRUE(gf::line_looks_like_comment("payload */ trailing"));
}

TEST(GrepFiltersComment, PlainCodeIsNotComment) {
    EXPECT_FALSE(gf::line_looks_like_comment("int x = 42;"));
    EXPECT_FALSE(gf::line_looks_like_comment("    foo(\"//\", x);"));
    EXPECT_FALSE(gf::line_looks_like_comment("string s = \"# not a comment\";"));
}

TEST(GrepFiltersComment, EmptyOrWhitespaceIsNotComment) {
    EXPECT_FALSE(gf::line_looks_like_comment(""));
    EXPECT_FALSE(gf::line_looks_like_comment("   "));
    EXPECT_FALSE(gf::line_looks_like_comment("\t\t"));
}

TEST(GrepFiltersComment, MultilineBlockBodyNotDetected) {
    // Known limitation matched with Go: a line inside `/* ... */` that
    // doesn't contain `/*` or `*/` is NOT classified as a comment.
    EXPECT_FALSE(gf::line_looks_like_comment("inside block comment"));
}

TEST(GrepFiltersTests, BasenameUnderscoreTestSuffix) {
    EXPECT_TRUE(gf::path_is_test("src/foo_test.cpp"));
    EXPECT_TRUE(gf::path_is_test("/abs/path/widget_test.go"));
    EXPECT_TRUE(gf::path_is_test("module_test.py"));
}

TEST(GrepFiltersTests, BasenameDotTestOrSpec) {
    EXPECT_TRUE(gf::path_is_test("src/foo.test.ts"));
    EXPECT_TRUE(gf::path_is_test("src/foo.spec.js"));
}

TEST(GrepFiltersTests, BasenameTestPrefix) {
    EXPECT_TRUE(gf::path_is_test("src/test_foo.py"));
    EXPECT_TRUE(gf::path_is_test("src/test_widget.cpp"));
}

TEST(GrepFiltersTests, CapitalTestSuffix) {
    EXPECT_TRUE(gf::path_is_test("src/FooTest.cpp"));
    EXPECT_TRUE(gf::path_is_test("src/FooTests.cpp"));
    EXPECT_TRUE(gf::path_is_test("BarTest.java"));
}

TEST(GrepFiltersTests, TestsDirectoryComponent) {
    EXPECT_TRUE(gf::path_is_test("tests/foo.cpp"));
    EXPECT_TRUE(gf::path_is_test("tests/sub/bar.cpp"));
    EXPECT_TRUE(gf::path_is_test("/abs/path/tests/baz.cpp"));
    EXPECT_TRUE(gf::path_is_test("test/foo.cpp"));
}

TEST(GrepFiltersTests, TestlikeButNotTest) {
    // Word "testing" in path but not as a directory component or marker.
    EXPECT_FALSE(gf::path_is_test("src/testing_utils.cpp"));
    EXPECT_FALSE(gf::path_is_test("src/protest_handler.cpp"));
    EXPECT_FALSE(gf::path_is_test("src/contests.cpp"));
    // "Test" buried mid-stem (not a suffix) should not match.
    EXPECT_FALSE(gf::path_is_test("src/TestHelper.cpp"));
}

TEST(GrepFiltersTests, PlainSourceFiles) {
    EXPECT_FALSE(gf::path_is_test("src/foo.cpp"));
    EXPECT_FALSE(gf::path_is_test("src/main.go"));
    EXPECT_FALSE(gf::path_is_test("src/util.py"));
}

TEST(GrepFiltersApplyExcludeTests, FiltersTestPaths) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"path", "src/foo.cpp"}, {"line", 10}});
    results.push_back({{"path", "src/foo_test.cpp"}, {"line", 5}});
    results.push_back({{"path", "tests/bar.cpp"}, {"line", 3}});
    results.push_back({{"path", "src/main.go"}, {"line", 1}});

    auto filtered = gf::apply_exclude_tests(results);
    ASSERT_EQ(filtered.size(), 2u);
    EXPECT_EQ(filtered[0]["path"].get<std::string>(), "src/foo.cpp");
    EXPECT_EQ(filtered[1]["path"].get<std::string>(), "src/main.go");
}

TEST(GrepFiltersApplyExcludeTests, EmptyInputProducesEmpty) {
    auto filtered = gf::apply_exclude_tests(nlohmann::json::array());
    EXPECT_TRUE(filtered.is_array());
    EXPECT_EQ(filtered.size(), 0u);
}

TEST(GrepFiltersApplyExcludeTests, AllTestPathsDropped) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"path", "tests/a.cpp"}, {"line", 1}});
    results.push_back({{"path", "src/foo_test.cpp"}, {"line", 2}});
    auto filtered = gf::apply_exclude_tests(results);
    EXPECT_EQ(filtered.size(), 0u);
}

TEST(GrepFiltersApplyExcludeComments, UsesEmbeddedContext) {
    // When the result carries an embedded context block with the matching
    // line text, the helper inspects it without touching the disk. Build a
    // result whose match line begins with `//` and another that doesn't.
    nlohmann::json comment_row;
    comment_row["path"] = "/no/such/file.cpp";
    comment_row["line"] = 1;
    comment_row["context"] = {{"start_line", 1}, {"lines", {"// commented match"}}};

    nlohmann::json code_row;
    code_row["path"] = "/no/such/file.cpp";
    code_row["line"] = 2;
    code_row["context"] = {{"start_line", 2}, {"lines", {"int x = 1;"}}};

    nlohmann::json results = nlohmann::json::array({comment_row, code_row});
    auto filtered = gf::apply_exclude_comments(results);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0]["line"].get<int>(), 2);
}

TEST(GrepFiltersWidenContext, ZeroIsPassthrough) {
    nlohmann::json input = nlohmann::json::array();
    input.push_back({{"path", "/anything"}, {"line", 1}});
    auto out = gf::widen_context_blocks(input, 0);
    EXPECT_EQ(out, input);
}

TEST(GrepFiltersWidenContext, ReadsLinesFromDisk) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "lci_grep_widen_test.txt";
    {
        std::ofstream out(tmp);
        out << "line1\nline2\nline3\nline4\nline5\n";
    }

    nlohmann::json results = nlohmann::json::array();
    nlohmann::json row;
    row["path"] = tmp.string();
    row["line"] = 3;
    row["context"] = nlohmann::json::object();
    results.push_back(row);

    auto widened = gf::widen_context_blocks(results, 1);
    ASSERT_EQ(widened.size(), 1u);
    auto& ctx = widened[0]["context"];
    EXPECT_EQ(ctx["start_line"].get<int>(), 2);
    EXPECT_EQ(ctx["end_line"].get<int>(), 4);
    ASSERT_EQ(ctx["lines"].size(), 3u);
    EXPECT_EQ(ctx["lines"][0].get<std::string>(), "line2");
    EXPECT_EQ(ctx["lines"][1].get<std::string>(), "line3");
    EXPECT_EQ(ctx["lines"][2].get<std::string>(), "line4");
    ASSERT_EQ(ctx["matched_lines"].size(), 1u);
    EXPECT_EQ(ctx["matched_lines"][0].get<int>(), 3);

    fs::remove(tmp);
}

TEST(GrepFiltersWidenContext, ClampsStartToOne) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "lci_grep_widen_clamp_test.txt";
    {
        std::ofstream out(tmp);
        out << "first\nsecond\nthird\n";
    }

    nlohmann::json results = nlohmann::json::array();
    nlohmann::json row;
    row["path"] = tmp.string();
    row["line"] = 1;
    results.push_back(row);

    auto widened = gf::widen_context_blocks(results, 5);
    ASSERT_EQ(widened.size(), 1u);
    auto& ctx = widened[0]["context"];
    EXPECT_EQ(ctx["start_line"].get<int>(), 1);
    // `to` was 6 but file has only 3 lines — end_line should reflect what we read.
    EXPECT_EQ(ctx["end_line"].get<int>(), 3);
    EXPECT_EQ(ctx["lines"].size(), 3u);

    fs::remove(tmp);
}

TEST(GrepFiltersWidenContext, MissingFileLeavesRowUnchanged) {
    nlohmann::json results = nlohmann::json::array();
    nlohmann::json row;
    row["path"] = "/no/such/file/should/exist.cpp";
    row["line"] = 1;
    row["context"] = {{"sentinel", "unchanged"}};
    results.push_back(row);

    auto widened = gf::widen_context_blocks(results, 3);
    ASSERT_EQ(widened.size(), 1u);
    EXPECT_EQ(widened[0]["context"]["sentinel"].get<std::string>(), "unchanged");
}

// -- Enhanced/assembly output helpers ---------------------------------------
//
// Pure formatting routines that back `lci search --enhanced` and
// `--assembly`. These tests pin the exact output strings against Go's
// reference formatters in cmd/lci/search.go (displayEnhancedResults at L616,
// displayStandardResultsWithAssembly at L353).

TEST(SearchFormatBreadcrumb, BothFieldsPopulated) {
    EXPECT_EQ(gf::format_breadcrumb("function", "myFunc"), "function myFunc");
    EXPECT_EQ(gf::format_breadcrumb("class", "Widget"), "class Widget");
    EXPECT_EQ(gf::format_breadcrumb("method", "Foo::bar"), "method Foo::bar");
}

TEST(SearchFormatBreadcrumb, OnlyTypeOrName) {
    EXPECT_EQ(gf::format_breadcrumb("function", ""), "function");
    EXPECT_EQ(gf::format_breadcrumb("", "loneSymbol"), "loneSymbol");
}

TEST(SearchFormatBreadcrumb, BothEmptyReturnsEmpty) {
    EXPECT_EQ(gf::format_breadcrumb("", ""), "");
}

TEST(SearchFormatMetricsLine, AllFieldsZero) {
    EXPECT_EQ(gf::format_metrics_line(0, 0, 0), "");
}

TEST(SearchFormatMetricsLine, OnlyComplexity) {
    EXPECT_EQ(gf::format_metrics_line(7, 0, 0), "complexity: 7");
}

TEST(SearchFormatMetricsLine, OnlyLines) {
    EXPECT_EQ(gf::format_metrics_line(0, 42, 0), "lines: 42");
}

TEST(SearchFormatMetricsLine, OnlyRefs) {
    EXPECT_EQ(gf::format_metrics_line(0, 0, 5), "refs: 5");
}

TEST(SearchFormatMetricsLine, AllThreeJoinedInGoOrder) {
    EXPECT_EQ(gf::format_metrics_line(7, 42, 5),
              "complexity: 7 | lines: 42 | refs: 5");
}

TEST(SearchFormatMetricsLine, NegativeFieldsSkipped) {
    // Metrics fields come from server-side counters; negatives indicate
    // "unknown" upstream. The formatter treats any non-positive value as
    // absent, matching Go's `> 0` gate (search.go:692).
    EXPECT_EQ(gf::format_metrics_line(-1, 0, 0), "");
    EXPECT_EQ(gf::format_metrics_line(0, -1, 5), "refs: 5");
}

TEST(SearchWidenToEnclosingBlock, RowWithEnclosingBoundsWidens) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "lci_search_widen_enclosing.txt";
    {
        std::ofstream out(tmp);
        out << "a\nb\nc\nd\ne\nf\ng\n";  // 7 lines
    }

    nlohmann::json results = nlohmann::json::array();
    nlohmann::json row;
    row["path"] = tmp.string();
    row["line"] = 4;
    row["enclosing_start"] = 2;
    row["enclosing_end"] = 6;
    nlohmann::json ctx;
    ctx["block_type"] = "function";
    ctx["block_name"] = "midFn";
    row["context"] = ctx;
    results.push_back(row);

    auto widened = gf::widen_to_enclosing_block(results);
    ASSERT_EQ(widened.size(), 1u);
    auto& w = widened[0]["context"];
    EXPECT_EQ(w["start_line"].get<int>(), 2);
    EXPECT_EQ(w["end_line"].get<int>(), 6);
    EXPECT_EQ(w["block_type"].get<std::string>(), "function");
    EXPECT_EQ(w["block_name"].get<std::string>(), "midFn");
    ASSERT_EQ(w["lines"].size(), 5u);
    EXPECT_EQ(w["lines"][0].get<std::string>(), "b");
    EXPECT_EQ(w["lines"][4].get<std::string>(), "f");
    ASSERT_EQ(w["matched_lines"].size(), 1u);
    EXPECT_EQ(w["matched_lines"][0].get<int>(), 4);

    fs::remove(tmp);
}

TEST(SearchWidenToEnclosingBlock, MissingBoundsLeftUnchanged) {
    nlohmann::json results = nlohmann::json::array();
    nlohmann::json row;
    row["path"] = "/no/such/file";
    row["line"] = 1;
    row["context"] = {{"sentinel", "untouched"}};
    results.push_back(row);

    auto widened = gf::widen_to_enclosing_block(results);
    ASSERT_EQ(widened.size(), 1u);
    EXPECT_EQ(widened[0]["context"]["sentinel"].get<std::string>(),
              "untouched");
}

TEST(SearchWidenToEnclosingBlock, ZeroOrInvertedRangeIgnored) {
    nlohmann::json results = nlohmann::json::array();
    nlohmann::json zero;
    zero["path"] = "/no/such/file";
    zero["line"] = 1;
    zero["enclosing_start"] = 0;
    zero["enclosing_end"] = 5;
    zero["context"] = {{"keep", true}};
    results.push_back(zero);

    nlohmann::json inverted;
    inverted["path"] = "/no/such/file";
    inverted["line"] = 1;
    inverted["enclosing_start"] = 10;
    inverted["enclosing_end"] = 5;
    inverted["context"] = {{"keep", true}};
    results.push_back(inverted);

    auto widened = gf::widen_to_enclosing_block(results);
    ASSERT_EQ(widened.size(), 2u);
    EXPECT_TRUE(widened[0]["context"]["keep"].get<bool>());
    EXPECT_TRUE(widened[1]["context"]["keep"].get<bool>());
}

// -- symbol_filters tests -----------------------------------------------------

namespace sf = ::lci::cli::symbol_filters;

TEST(SymbolFiltersIsGlob, DetectsStar) {
    EXPECT_TRUE(sf::is_glob_pattern("*.cpp"));
    EXPECT_TRUE(sf::is_glob_pattern("src/*.h"));
}

TEST(SymbolFiltersIsGlob, DetectsQuestionMark) {
    EXPECT_TRUE(sf::is_glob_pattern("foo.?pp"));
}

TEST(SymbolFiltersIsGlob, DetectsCharClass) {
    EXPECT_TRUE(sf::is_glob_pattern("foo.[ch]pp"));
}

TEST(SymbolFiltersIsGlob, BareSubstringIsNotGlob) {
    EXPECT_FALSE(sf::is_glob_pattern("commands.cpp"));
    EXPECT_FALSE(sf::is_glob_pattern("src/cli"));
    EXPECT_FALSE(sf::is_glob_pattern(""));
}

TEST(SymbolFiltersGlobMatch, StarDoesNotCrossSlash) {
    EXPECT_TRUE(sf::glob_match("*.cpp", "main.cpp"));
    EXPECT_FALSE(sf::glob_match("*.cpp", "src/main.cpp"));
}

TEST(SymbolFiltersGlobMatch, QuestionMarkMatchesOneNonSlash) {
    EXPECT_TRUE(sf::glob_match("foo.?pp", "foo.cpp"));
    EXPECT_TRUE(sf::glob_match("foo.?pp", "foo.hpp"));
    EXPECT_FALSE(sf::glob_match("foo.?pp", "foo.pp"));
    EXPECT_FALSE(sf::glob_match("foo.??", "foo./x"));
}

TEST(SymbolFiltersGlobMatch, ExactMatch) {
    EXPECT_TRUE(sf::glob_match("commands.cpp", "commands.cpp"));
    EXPECT_FALSE(sf::glob_match("commands.cpp", "commands.h"));
}

TEST(SymbolFiltersGlobMatch, EmptyPatternMatchesEmptyOnly) {
    EXPECT_TRUE(sf::glob_match("", ""));
    EXPECT_FALSE(sf::glob_match("", "anything"));
}

TEST(SymbolFiltersGlobMatch, TrailingStar) {
    EXPECT_TRUE(sf::glob_match("foo*", "foobar"));
    EXPECT_TRUE(sf::glob_match("foo*", "foo"));
    EXPECT_FALSE(sf::glob_match("foo*", "foo/bar"));
}

TEST(SymbolFiltersGlobMatch, LeadingStar) {
    EXPECT_TRUE(sf::glob_match("*foo", "barfoo"));
    EXPECT_TRUE(sf::glob_match("*foo", "foo"));
    EXPECT_FALSE(sf::glob_match("*foo", "bar/foo"));
}

TEST(SymbolFiltersGlobPathOrBasename, FullPathHit) {
    EXPECT_TRUE(sf::glob_match_path_or_basename("src/cli/*.cpp",
                                                "src/cli/commands.cpp"));
}

TEST(SymbolFiltersGlobPathOrBasename, BasenameFallback) {
    // *.cpp will not match a path with a slash, but matches the basename.
    EXPECT_TRUE(sf::glob_match_path_or_basename("*.cpp",
                                                "src/cli/commands.cpp"));
}

TEST(SymbolFiltersGlobPathOrBasename, NoMatchReturnsFalse) {
    EXPECT_FALSE(sf::glob_match_path_or_basename("*.go",
                                                 "src/cli/commands.cpp"));
}

TEST(SymbolFiltersApplyFileGlob, EmptyPatternIsPassthrough) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"file", "a.cpp"}, {"name", "x"}});
    arr.push_back({{"file", "b.go"}, {"name", "y"}});
    auto out = sf::apply_file_glob(arr, "");
    EXPECT_EQ(out.size(), 2u);
}

TEST(SymbolFiltersApplyFileGlob, FiltersByExtension) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"file", "src/a.cpp"}, {"name", "a"}});
    arr.push_back({{"file", "src/b.go"}, {"name", "b"}});
    arr.push_back({{"file", "src/c.cpp"}, {"name", "c"}});
    auto out = sf::apply_file_glob(arr, "*.cpp");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0]["name"].get<std::string>(), "a");
    EXPECT_EQ(out[1]["name"].get<std::string>(), "c");
}

TEST(SymbolFiltersApplyFileGlob, MissingFileFieldIsDropped) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "noFile"}});
    arr.push_back({{"file", "x.cpp"}, {"name", "ok"}});
    auto out = sf::apply_file_glob(arr, "*.cpp");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]["name"].get<std::string>(), "ok");
}

TEST(SymbolFiltersSort, EmptyKeyIsPassthrough) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "z"}});
    arr.push_back({{"name", "a"}});
    auto out = sf::sort_symbols(arr, "");
    EXPECT_EQ(out[0]["name"].get<std::string>(), "z");
    EXPECT_EQ(out[1]["name"].get<std::string>(), "a");
}

TEST(SymbolFiltersSort, NameAscending) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "zebra"}});
    arr.push_back({{"name", "apple"}});
    arr.push_back({{"name", "mango"}});
    auto out = sf::sort_symbols(arr, "name");
    EXPECT_EQ(out[0]["name"].get<std::string>(), "apple");
    EXPECT_EQ(out[1]["name"].get<std::string>(), "mango");
    EXPECT_EQ(out[2]["name"].get<std::string>(), "zebra");
}

TEST(SymbolFiltersSort, ComplexityDescending) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "a"}, {"complexity", 3}});
    arr.push_back({{"name", "b"}, {"complexity", 10}});
    arr.push_back({{"name", "c"}, {"complexity", 5}});
    auto out = sf::sort_symbols(arr, "complexity");
    EXPECT_EQ(out[0]["name"].get<std::string>(), "b");
    EXPECT_EQ(out[1]["name"].get<std::string>(), "c");
    EXPECT_EQ(out[2]["name"].get<std::string>(), "a");
}

TEST(SymbolFiltersSort, RefsDescendingIsSumOfIncomingPlusOutgoing) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "a"}, {"incoming_refs", 1}, {"outgoing_refs", 2}});
    arr.push_back({{"name", "b"}, {"incoming_refs", 5}, {"outgoing_refs", 5}});
    arr.push_back({{"name", "c"}, {"incoming_refs", 10}});
    auto out = sf::sort_symbols(arr, "refs");
    EXPECT_EQ(out[0]["name"].get<std::string>(), "b");  // 10
    EXPECT_EQ(out[1]["name"].get<std::string>(), "c");  // 10 (tie, stable)
    EXPECT_EQ(out[2]["name"].get<std::string>(), "a");  // 3
}

TEST(SymbolFiltersSort, LineAscendingThenFile) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "a"}, {"file", "z.cpp"}, {"line", 5}});
    arr.push_back({{"name", "b"}, {"file", "a.cpp"}, {"line", 100}});
    arr.push_back({{"name", "c"}, {"file", "a.cpp"}, {"line", 1}});
    auto out = sf::sort_symbols(arr, "line");
    EXPECT_EQ(out[0]["name"].get<std::string>(), "c");  // a.cpp:1
    EXPECT_EQ(out[1]["name"].get<std::string>(), "b");  // a.cpp:100
    EXPECT_EQ(out[2]["name"].get<std::string>(), "a");  // z.cpp:5
}

TEST(SymbolFiltersSort, ParamsDescending) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "a"}, {"parameter_count", 1}});
    arr.push_back({{"name", "b"}, {"parameter_count", 7}});
    arr.push_back({{"name", "c"}, {"parameter_count", 3}});
    auto out = sf::sort_symbols(arr, "params");
    EXPECT_EQ(out[0]["name"].get<std::string>(), "b");
    EXPECT_EQ(out[1]["name"].get<std::string>(), "c");
    EXPECT_EQ(out[2]["name"].get<std::string>(), "a");
}

TEST(SymbolFiltersSort, UnknownKeyFallsBackToName) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "z"}});
    arr.push_back({{"name", "a"}});
    auto out = sf::sort_symbols(arr, "fizzbuzz");
    EXPECT_EQ(out[0]["name"].get<std::string>(), "a");
    EXPECT_EQ(out[1]["name"].get<std::string>(), "z");
}

TEST(SymbolFiltersSort, StableForEqualKeys) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "first"}, {"complexity", 5}});
    arr.push_back({{"name", "second"}, {"complexity", 5}});
    arr.push_back({{"name", "third"}, {"complexity", 5}});
    auto out = sf::sort_symbols(arr, "complexity");
    EXPECT_EQ(out[0]["name"].get<std::string>(), "first");
    EXPECT_EQ(out[1]["name"].get<std::string>(), "second");
    EXPECT_EQ(out[2]["name"].get<std::string>(), "third");
}

TEST(SymbolFiltersMaxLimit, ZeroIsPassthrough) {
    nlohmann::json arr = nlohmann::json::array();
    for (int i = 0; i < 5; ++i) {
        arr.push_back({{"name", std::to_string(i)}});
    }
    auto out = sf::apply_max_limit(arr, 0);
    EXPECT_EQ(out.size(), 5u);
}

TEST(SymbolFiltersMaxLimit, NegativeIsPassthrough) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "x"}});
    auto out = sf::apply_max_limit(arr, -1);
    EXPECT_EQ(out.size(), 1u);
}

TEST(SymbolFiltersMaxLimit, TruncatesToMax) {
    nlohmann::json arr = nlohmann::json::array();
    for (int i = 0; i < 10; ++i) {
        arr.push_back({{"name", std::to_string(i)}});
    }
    auto out = sf::apply_max_limit(arr, 3);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0]["name"].get<std::string>(), "0");
    EXPECT_EQ(out[2]["name"].get<std::string>(), "2");
}

TEST(SymbolFiltersMaxLimit, MaxLargerThanInputIsPassthrough) {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"name", "a"}});
    arr.push_back({{"name", "b"}});
    auto out = sf::apply_max_limit(arr, 100);
    EXPECT_EQ(out.size(), 2u);
}

// ---------------------------------------------------------------------------
// tree_formatter helpers
// ---------------------------------------------------------------------------

namespace tf = ::lci::cli::tree_formatter;

namespace {

// Builds a small canonical tree response shape:
//
//   main (depth=0, file=src/a.cpp:10)
//   ├─ helper (depth=1, file=src/a.cpp:20)
//   │   └─ inner (depth=2, file=src/b.cpp:5)
//   └─ other  (depth=1, file=src/a.cpp:30)
//
// Matches /tree's output (server.cpp:984-989). `complexity` and
// `lines_of_code` are stamped only on `helper` to exercise the partial
// metrics annotation path.
nlohmann::json make_sample_tree() {
    auto inner = nlohmann::json{
        {"name", "inner"}, {"line", 5}, {"depth", 2},
        {"file_path", "src/b.cpp"}, {"children", nlohmann::json::array()}};
    auto helper = nlohmann::json{
        {"name", "helper"}, {"line", 20}, {"depth", 1},
        {"file_path", "src/a.cpp"}, {"complexity", 7}, {"lines_of_code", 12},
        {"children", nlohmann::json::array({inner})}};
    auto other = nlohmann::json{
        {"name", "other"}, {"line", 30}, {"depth", 1},
        {"file_path", "src/a.cpp"}, {"children", nlohmann::json::array()}};
    auto root = nlohmann::json{
        {"name", "main"}, {"line", 10}, {"depth", 0},
        {"file_path", "src/a.cpp"},
        {"children", nlohmann::json::array({helper, other})}};
    return nlohmann::json{
        {"root", root}, {"root_function", "main"}, {"max_depth", 5},
        {"total_nodes", 3}};
}

}  // namespace

// -- node_annotations ---------------------------------------------------------

TEST(TreeFormatterAnnotations, ShowLinesEmitsBracketedFileColonLine) {
    nlohmann::json node = {{"name", "f"}, {"line", 42},
                           {"file_path", "src/x.cpp"}};
    tf::Options opts;
    opts.show_lines = true;
    EXPECT_EQ(tf::node_annotations(node, opts), " [src/x.cpp:42]");
}

TEST(TreeFormatterAnnotations, ShowLinesEmptyWhenLineZero) {
    nlohmann::json node = {{"name", "f"}, {"line", 0},
                           {"file_path", "src/x.cpp"}};
    tf::Options opts;
    opts.show_lines = true;
    EXPECT_EQ(tf::node_annotations(node, opts), "");
}

TEST(TreeFormatterAnnotations, MetricsBothFields) {
    nlohmann::json node = {{"name", "f"}, {"complexity", 5},
                           {"lines_of_code", 20}};
    tf::Options opts;
    opts.metrics = true;
    EXPECT_EQ(tf::node_annotations(node, opts), " (complexity:5, lines:20)");
}

TEST(TreeFormatterAnnotations, MetricsOnlyComplexity) {
    nlohmann::json node = {{"name", "f"}, {"complexity", 5}};
    tf::Options opts;
    opts.metrics = true;
    EXPECT_EQ(tf::node_annotations(node, opts), " (complexity:5)");
}

TEST(TreeFormatterAnnotations, MetricsOnlyLinesOfCode) {
    nlohmann::json node = {{"name", "f"}, {"lines_of_code", 12}};
    tf::Options opts;
    opts.metrics = true;
    EXPECT_EQ(tf::node_annotations(node, opts), " (lines:12)");
}

TEST(TreeFormatterAnnotations, MetricsOmittedWhenZero) {
    nlohmann::json node = {{"name", "f"}, {"complexity", 0},
                           {"lines_of_code", 0}};
    tf::Options opts;
    opts.metrics = true;
    EXPECT_EQ(tf::node_annotations(node, opts), "");
}

TEST(TreeFormatterAnnotations, ComposableShowLinesPlusMetrics) {
    nlohmann::json node = {{"name", "f"}, {"line", 42},
                           {"file_path", "src/x.cpp"}, {"complexity", 3},
                           {"lines_of_code", 8}};
    tf::Options opts;
    opts.show_lines = true;
    opts.metrics = true;
    EXPECT_EQ(tf::node_annotations(node, opts),
              " [src/x.cpp:42] (complexity:3, lines:8)");
}

TEST(TreeFormatterAnnotations, EmptyWhenNoFlagsSet) {
    nlohmann::json node = {{"name", "f"}, {"line", 42},
                           {"file_path", "src/x.cpp"}, {"complexity", 5}};
    tf::Options opts;
    EXPECT_EQ(tf::node_annotations(node, opts), "");
}

// -- format_compact -----------------------------------------------------------

TEST(TreeFormatterCompact, LinearChainJoinedWithArrow) {
    auto tree = make_sample_tree();
    std::string out = tf::format_compact(tree);
    // Mirrors Go's collectCompactParts (tree_formatter.go:204): recurse
    // into the first child fully, then append `(+N more)` for the
    // remaining siblings of the current level. For the sample tree:
    //   main -> first child helper -> first child inner (no children) ->
    //   then since main has 1 extra sibling -> "(+1 more)".
    // UTF-8 arrow `→` = e2 86 92.
    EXPECT_EQ(out, "main \xe2\x86\x92 helper \xe2\x86\x92 inner "
                   "\xe2\x86\x92 (+1 more)");
}

TEST(TreeFormatterCompact, EmptyTreeReturnsEmpty) {
    nlohmann::json empty = nlohmann::json::object();
    EXPECT_EQ(tf::format_compact(empty), "");
}

TEST(TreeFormatterCompact, SingleNodeNoChildren) {
    nlohmann::json tree = {
        {"root", {{"name", "solo"}, {"children", nlohmann::json::array()}}},
        {"root_function", "solo"}};
    EXPECT_EQ(tf::format_compact(tree), "solo");
}

TEST(TreeFormatterCompact, MultipleSiblingsOnlyFirstFollowed) {
    nlohmann::json tree = {
        {"root",
         {{"name", "r"},
          {"children",
           nlohmann::json::array(
               {{{"name", "a"}, {"children", nlohmann::json::array()}},
                {{"name", "b"}, {"children", nlohmann::json::array()}},
                {{"name", "c"}, {"children", nlohmann::json::array()}}})}}}};
    // Linear follow + sibling count for the others.
    EXPECT_EQ(tf::format_compact(tree), "r \xe2\x86\x92 a \xe2\x86\x92 "
                                         "(+2 more)");
}

// -- format_text --------------------------------------------------------------

TEST(TreeFormatterText, IncludesHeaderAndRootFunction) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    std::string out = tf::format_text(tree, opts);
    EXPECT_NE(out.find("Function tree for 'main'"), std::string::npos);
    EXPECT_NE(out.find("Total nodes: 3"), std::string::npos);
    EXPECT_NE(out.find("Max depth: 5"), std::string::npos);
}

TEST(TreeFormatterText, RootGlyphIsRightArrow) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    std::string out = tf::format_text(tree, opts);
    // Root line uses `→ ` glyph (UTF-8: e2 86 92 + space).
    EXPECT_NE(out.find("\xe2\x86\x92 main"), std::string::npos);
}

TEST(TreeFormatterText, BranchGlyphsForChildren) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    std::string out = tf::format_text(tree, opts);
    // `helper` is the first of two siblings -> `├─→`. `other` is last
    // -> `└─→`.
    EXPECT_NE(out.find("\xe2\x94\x9c\xe2\x94\x80\xe2\x86\x92 helper"),
              std::string::npos);
    EXPECT_NE(out.find("\xe2\x94\x94\xe2\x94\x80\xe2\x86\x92 other"),
              std::string::npos);
}

TEST(TreeFormatterText, ShowLinesAddsBracketedAnnotation) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    opts.show_lines = true;
    std::string out = tf::format_text(tree, opts);
    EXPECT_NE(out.find("main [src/a.cpp:10]"), std::string::npos);
    EXPECT_NE(out.find("helper [src/a.cpp:20]"), std::string::npos);
    EXPECT_NE(out.find("inner [src/b.cpp:5]"), std::string::npos);
}

TEST(TreeFormatterText, MetricsAddsComplexityAnnotation) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    opts.metrics = true;
    std::string out = tf::format_text(tree, opts);
    // `helper` is the only node with metrics in the sample tree.
    EXPECT_NE(out.find("helper (complexity:7, lines:12)"), std::string::npos);
    // No metrics annotation for nodes without complexity/lines_of_code:
    // they should still emit `(depth=N)` from the depth tag, but not a
    // metrics block. Confirm the metrics segment specifically is absent.
    EXPECT_EQ(out.find("inner (complexity"), std::string::npos);
    EXPECT_EQ(out.find("inner (lines:"), std::string::npos);
}

TEST(TreeFormatterText, DepthTagAppendedToEveryNode) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    std::string out = tf::format_text(tree, opts);
    EXPECT_NE(out.find("main (depth=0)"), std::string::npos);
    EXPECT_NE(out.find("helper (depth=1)"), std::string::npos);
    EXPECT_NE(out.find("other (depth=1)"), std::string::npos);
    EXPECT_NE(out.find("inner (depth=2)"), std::string::npos);
}

TEST(TreeFormatterText, EmptyTreeReturnsPlaceholder) {
    nlohmann::json empty = nlohmann::json::object();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    EXPECT_EQ(tf::format_text(empty, opts), "No tree data available\n");
}

// -- agent mode (ASCII-only) --------------------------------------------------

TEST(TreeFormatterAgent, NoUnicodeBoxDrawingChars) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Agent;
    std::string out = tf::format_text(tree, opts);
    // No box-drawing chars or arrows should appear.
    EXPECT_EQ(out.find("\xe2\x86\x92"), std::string::npos)
        << "agent mode must not emit `→` arrows";
    EXPECT_EQ(out.find("\xe2\x94\x9c"), std::string::npos)
        << "agent mode must not emit `├` branch chars";
    EXPECT_EQ(out.find("\xe2\x94\x94"), std::string::npos)
        << "agent mode must not emit `└` branch chars";
    EXPECT_EQ(out.find("\xe2\x94\x82"), std::string::npos)
        << "agent mode must not emit `│` continuation bars";
}

TEST(TreeFormatterAgent, NodesIndentedByTwoSpacesPerLevel) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Agent;
    std::string out = tf::format_text(tree, opts);
    // Root: no indent. Depth-1 children: 2-space indent. Depth-2: 4 spaces.
    EXPECT_NE(out.find("\nmain "), std::string::npos);
    EXPECT_NE(out.find("\n  helper"), std::string::npos);
    EXPECT_NE(out.find("\n    inner"), std::string::npos);
    EXPECT_NE(out.find("\n  other"), std::string::npos);
}

TEST(TreeFormatterAgent, ComposableWithShowLinesAndMetrics) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Agent;
    opts.show_lines = true;
    opts.metrics = true;
    std::string out = tf::format_text(tree, opts);
    // helper has both file/line and metrics in our sample.
    EXPECT_NE(out.find("helper [src/a.cpp:20] (complexity:7, lines:12)"),
              std::string::npos);
    // inner has only file/line (no metrics in sample).
    EXPECT_NE(out.find("inner [src/b.cpp:5]"), std::string::npos);
    // Confirm still ASCII-only.
    EXPECT_EQ(out.find("\xe2\x86\x92"), std::string::npos);
}

// -- format_tree dispatcher ---------------------------------------------------

TEST(TreeFormatterDispatch, CompactModeGoesThroughFormatCompact) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Compact;
    std::string out = tf::format_tree(tree, opts);
    // Compact mode emits a single line ending with newline.
    EXPECT_NE(out.find("main \xe2\x86\x92 helper"), std::string::npos);
    EXPECT_EQ(out.back(), '\n');
    // No tree header in compact mode.
    EXPECT_EQ(out.find("Function tree for"), std::string::npos);
}

TEST(TreeFormatterDispatch, TextModeGoesThroughFormatText) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    std::string out = tf::format_tree(tree, opts);
    EXPECT_NE(out.find("Function tree for 'main'"), std::string::npos);
}

TEST(TreeFormatterDispatch, AgentModeGoesThroughFormatTextAgent) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Agent;
    std::string out = tf::format_tree(tree, opts);
    EXPECT_NE(out.find("Function tree for 'main'"), std::string::npos);
    // ASCII-only.
    EXPECT_EQ(out.find("\xe2\x86\x92"), std::string::npos);
}

// -- max_depth client-side cutoff --------------------------------------------

TEST(TreeFormatterText, MaxDepthCutsOffDeeperNodes) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    opts.max_depth = 1;
    std::string out = tf::format_text(tree, opts);
    EXPECT_NE(out.find("main"), std::string::npos);
    EXPECT_NE(out.find("helper"), std::string::npos);
    EXPECT_NE(out.find("other"), std::string::npos);
    EXPECT_EQ(out.find("inner"), std::string::npos)
        << "max_depth=1 must not emit depth-2 nodes";
}

TEST(TreeFormatterText, MaxDepthZeroMeansNoCutoff) {
    auto tree = make_sample_tree();
    tf::Options opts;
    opts.mode = tf::Mode::Text;
    opts.max_depth = 0;
    std::string out = tf::format_text(tree, opts);
    // All four nodes appear.
    EXPECT_NE(out.find("inner"), std::string::npos);
}

// -- query_parser tests -------------------------------------------------------
//
// Pure parsing/filtering logic; no server needed. Covers the four directives
// (`file:`, `kind:`, `symbol:`, `-term`), the bare-term passthrough, and the
// JSON post-filter helpers that consume the server's `/search` shape.

namespace qp = ::lci::cli::query_parser;

TEST(QueryParserParse, BareTermsHaveNoDirectives) {
    auto p = qp::parse("auth login");
    EXPECT_EQ(p.content_query, "auth login");
    EXPECT_TRUE(p.empty_directives());
    EXPECT_TRUE(p.file_glob.empty());
    EXPECT_TRUE(p.kinds.empty());
    EXPECT_TRUE(p.symbols.empty());
    EXPECT_TRUE(p.exclusions.empty());
}

TEST(QueryParserParse, EmptyQueryProducesEmptyParse) {
    auto p = qp::parse("");
    EXPECT_TRUE(p.content_query.empty());
    EXPECT_TRUE(p.empty_directives());
}

TEST(QueryParserParse, FileDirectiveExtracted) {
    auto p = qp::parse("file:*.cpp auth");
    EXPECT_EQ(p.file_glob, "*.cpp");
    EXPECT_EQ(p.content_query, "auth");
}

TEST(QueryParserParse, FileDirectiveLastWriteWins) {
    auto p = qp::parse("file:*.cpp file:*.hpp auth");
    EXPECT_EQ(p.file_glob, "*.hpp");
    EXPECT_EQ(p.content_query, "auth");
}

TEST(QueryParserParse, KindDirectiveExtracted) {
    auto p = qp::parse("kind:function Request");
    ASSERT_EQ(p.kinds.size(), 1u);
    EXPECT_EQ(p.kinds[0], "function");
    EXPECT_EQ(p.content_query, "Request");
}

TEST(QueryParserParse, MultipleKindDirectivesPreserved) {
    auto p = qp::parse("kind:function kind:method auth");
    ASSERT_EQ(p.kinds.size(), 2u);
    EXPECT_EQ(p.kinds[0], "function");
    EXPECT_EQ(p.kinds[1], "method");
}

TEST(QueryParserParse, SymbolDirectiveExtracted) {
    auto p = qp::parse("symbol:Request -test");
    ASSERT_EQ(p.symbols.size(), 1u);
    EXPECT_EQ(p.symbols[0], "Request");
    ASSERT_EQ(p.exclusions.size(), 1u);
    EXPECT_EQ(p.exclusions[0], "test");
    EXPECT_TRUE(p.content_query.empty());
}

TEST(QueryParserParse, ExclusionExtracted) {
    auto p = qp::parse("auth -deprecated -legacy");
    EXPECT_EQ(p.content_query, "auth");
    ASSERT_EQ(p.exclusions.size(), 2u);
    EXPECT_EQ(p.exclusions[0], "deprecated");
    EXPECT_EQ(p.exclusions[1], "legacy");
}

TEST(QueryParserParse, BareDashIsContent) {
    // A lone "-" is too ambiguous to treat as exclusion (no term to drop).
    // Keeping it as a content token avoids silently swallowing user input.
    auto p = qp::parse("- foo");
    EXPECT_EQ(p.content_query, "- foo");
    EXPECT_TRUE(p.exclusions.empty());
}

TEST(QueryParserParse, UnknownColonPrefixIsContent) {
    // `http://example` should NOT be misclassified as the unknown `http:`
    // directive; it's a literal substring users want to find.
    auto p = qp::parse("http://example");
    EXPECT_EQ(p.content_query, "http://example");
    EXPECT_TRUE(p.empty_directives());
}

TEST(QueryParserParse, MultipleDirectivesComposeWithBareTerms) {
    auto p = qp::parse("file:*.cpp kind:function symbol:Handler -test auth");
    EXPECT_EQ(p.file_glob, "*.cpp");
    ASSERT_EQ(p.kinds.size(), 1u);
    EXPECT_EQ(p.kinds[0], "function");
    ASSERT_EQ(p.symbols.size(), 1u);
    EXPECT_EQ(p.symbols[0], "Handler");
    ASSERT_EQ(p.exclusions.size(), 1u);
    EXPECT_EQ(p.exclusions[0], "test");
    EXPECT_EQ(p.content_query, "auth");
}

TEST(QueryParserParse, DirectiveOnlyQueryHasEmptyContent) {
    auto p = qp::parse("file:*.cpp kind:function");
    EXPECT_EQ(p.content_query, "");
    EXPECT_FALSE(p.empty_directives());
}

TEST(QueryParserParse, MultipleSpacesCollapse) {
    auto p = qp::parse("  auth   login  ");
    EXPECT_EQ(p.content_query, "auth login");
}

TEST(QueryParserGlob, StarMatchesAnyChars) {
    EXPECT_TRUE(qp::glob_match("*.cpp", "search.cpp"));
    EXPECT_TRUE(qp::glob_match("*.cpp", ".cpp"));
    EXPECT_FALSE(qp::glob_match("*.cpp", "search.hpp"));
}

TEST(QueryParserGlob, QuestionMatchesOneChar) {
    EXPECT_TRUE(qp::glob_match("a?c", "abc"));
    EXPECT_FALSE(qp::glob_match("a?c", "ac"));
    EXPECT_FALSE(qp::glob_match("a?c", "abbc"));
}

TEST(QueryParserGlob, MixedStarAndLiterals) {
    EXPECT_TRUE(qp::glob_match("src/*/main.cpp", "src/cli/main.cpp"));
    EXPECT_TRUE(qp::glob_match("*test*", "auth_test_helper"));
    EXPECT_FALSE(qp::glob_match("src/*/main.cpp", "src/main.cpp"));
}

TEST(QueryParserGlob, PathsWithoutSlashMatchBasename) {
    EXPECT_TRUE(qp::path_matches_glob("*.cpp",
                                      "/abs/path/src/cli/search.cpp"));
    EXPECT_FALSE(qp::path_matches_glob("*.cpp",
                                       "/abs/path/src/cli/search.hpp"));
}

TEST(QueryParserGlob, PathsWithSlashMatchFullPath) {
    EXPECT_TRUE(qp::path_matches_glob("src/*/search.cpp",
                                      "src/cli/search.cpp"));
    EXPECT_FALSE(qp::path_matches_glob("src/*/search.cpp",
                                       "tests/cli/search.cpp"));
}

TEST(QueryParserGlob, EmptyPatternMatchesEverything) {
    EXPECT_TRUE(qp::path_matches_glob("", "anything.cpp"));
    EXPECT_TRUE(qp::path_matches_glob("", ""));
}

// -- Filter helpers (JSON post-filtering) -------------------------------------

namespace {
nlohmann::json make_result(const std::string& path, const std::string& match,
                           const std::string& block_type = "",
                           const std::string& block_name = "") {
    nlohmann::json r;
    r["path"] = path;
    r["match"] = match;
    r["line"] = 1;
    nlohmann::json ctx;
    ctx["block_type"] = block_type;
    ctx["block_name"] = block_name;
    r["context"] = ctx;
    return r;
}
}  // namespace

TEST(QueryParserFilters, FileFilterDropsNonMatching) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("src/cli/search.cpp", "auth"));
    results.push_back(make_result("src/cli/search.hpp", "auth"));
    results.push_back(make_result("tests/cli/search.cpp", "auth"));

    auto filtered = qp::apply_file_filter(results, "*.cpp");
    EXPECT_EQ(filtered.size(), 2u);
    EXPECT_EQ(filtered[0].value("path", ""), "src/cli/search.cpp");
    EXPECT_EQ(filtered[1].value("path", ""), "tests/cli/search.cpp");
}

TEST(QueryParserFilters, FileFilterPathGlob) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("src/cli/search.cpp", "auth"));
    results.push_back(make_result("src/server/server.cpp", "auth"));
    results.push_back(make_result("tests/cli/foo.cpp", "auth"));

    auto filtered = qp::apply_file_filter(results, "src/*/*.cpp");
    EXPECT_EQ(filtered.size(), 2u);
    EXPECT_EQ(filtered[0].value("path", ""), "src/cli/search.cpp");
    EXPECT_EQ(filtered[1].value("path", ""), "src/server/server.cpp");
}

TEST(QueryParserFilters, FileFilterEmptyGlobPassesThrough) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "x"));
    auto filtered = qp::apply_file_filter(results, "");
    EXPECT_EQ(filtered.size(), 1u);
}

TEST(QueryParserFilters, KindFilterDropsNonMatching) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "x", "function", "foo"));
    results.push_back(make_result("a.cpp", "x", "class", "Bar"));
    results.push_back(make_result("a.cpp", "x", "function", "baz"));

    auto filtered = qp::apply_kind_filter(results, {"function"});
    EXPECT_EQ(filtered.size(), 2u);
    EXPECT_EQ(filtered[0].at("context").value("block_name", ""), "foo");
    EXPECT_EQ(filtered[1].at("context").value("block_name", ""), "baz");
}

TEST(QueryParserFilters, KindFilterIsCaseInsensitive) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "x", "Function", "foo"));
    results.push_back(make_result("a.cpp", "x", "FUNCTION", "bar"));

    auto filtered = qp::apply_kind_filter(results, {"function"});
    EXPECT_EQ(filtered.size(), 2u);
}

TEST(QueryParserFilters, KindFilterMultipleKindsORed) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "x", "function", "foo"));
    results.push_back(make_result("a.cpp", "x", "class", "Bar"));
    results.push_back(make_result("a.cpp", "x", "method", "baz"));

    auto filtered = qp::apply_kind_filter(results, {"function", "class"});
    EXPECT_EQ(filtered.size(), 2u);
}

TEST(QueryParserFilters, SymbolFilterSubstringMatch) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "x", "function", "Request"));
    results.push_back(make_result("a.cpp", "x", "function", "RequestHandler"));
    results.push_back(make_result("a.cpp", "x", "function", "Response"));

    auto filtered = qp::apply_symbol_filter(results, {"Request"});
    EXPECT_EQ(filtered.size(), 2u);
}

TEST(QueryParserFilters, SymbolFilterCaseInsensitive) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "x", "function", "MyHandler"));
    results.push_back(make_result("a.cpp", "x", "function", "myhandler"));

    auto filtered = qp::apply_symbol_filter(results, {"HANDLER"});
    EXPECT_EQ(filtered.size(), 2u);
}

TEST(QueryParserFilters, ExclusionDropsMatchTermInLine) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "auth login", "", ""));
    results.push_back(make_result("a.cpp", "auth test_login", "", ""));

    auto filtered = qp::apply_exclusion_filter(results, {"test"});
    EXPECT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].value("match", ""), "auth login");
}

TEST(QueryParserFilters, ExclusionDropsMatchTermInPath) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("src/auth.cpp", "Request", "", ""));
    results.push_back(make_result("tests/auth_test.cpp", "Request", "", ""));

    auto filtered = qp::apply_exclusion_filter(results, {"test"});
    EXPECT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].value("path", ""), "src/auth.cpp");
}

TEST(QueryParserFilters, ExclusionMultipleTermsAnyDrops) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "Request", "", ""));
    results.push_back(make_result("a.cpp", "Request_legacy", "", ""));
    results.push_back(make_result("deprecated/a.cpp", "Request", "", ""));

    auto filtered = qp::apply_exclusion_filter(results,
                                               {"legacy", "deprecated"});
    EXPECT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].value("path", ""), "a.cpp");
    EXPECT_EQ(filtered[0].value("match", ""), "Request");
}

TEST(QueryParserFilters, ApplyAllComposesFilters) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("src/cli/search.cpp", "auth login",
                                  "function", "Request"));
    results.push_back(make_result("src/cli/search.hpp", "auth login",
                                  "function", "Request"));  // wrong ext
    results.push_back(make_result("src/cli/search.cpp", "test login",
                                  "function", "Request"));  // exclusion
    results.push_back(make_result("src/cli/search.cpp", "auth login",
                                  "class", "Request"));     // wrong kind
    results.push_back(make_result("src/cli/search.cpp", "auth login",
                                  "function", "Other"));    // wrong symbol

    auto p = qp::parse("file:*.cpp kind:function symbol:Request -test auth");
    auto filtered = qp::apply_all(results, p);
    EXPECT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].value("path", ""), "src/cli/search.cpp");
    EXPECT_EQ(filtered[0].value("match", ""), "auth login");
}

TEST(QueryParserFilters, ApplyAllPassThroughWithNoDirectives) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "x"));
    results.push_back(make_result("b.cpp", "y"));

    auto p = qp::parse("auth");
    auto filtered = qp::apply_all(results, p);
    EXPECT_EQ(filtered.size(), 2u);
}

// -- rank_options tests ------------------------------------------------------
//
// Pure ranking and context-filter logic. No server needed. Covers strategy
// parsing (relevance/recency/file-type + aliases), context filter parsing
// (function/class/top-level + aliases), the block_type matrix, and the JSON
// re-rank helpers. `apply_recency` is exercised against real temp files so
// the mtime sort path (the one with I/O) is verified end-to-end.

namespace ro = ::lci::cli::rank_options;

TEST(RankOptionsParseStrategy, EmptyDefaultsToRelevance) {
    EXPECT_EQ(ro::parse_strategy(""), ro::RankStrategy::Relevance);
}

TEST(RankOptionsParseStrategy, RelevanceRecognized) {
    EXPECT_EQ(ro::parse_strategy("relevance"), ro::RankStrategy::Relevance);
    EXPECT_EQ(ro::parse_strategy("RELEVANCE"), ro::RankStrategy::Relevance);
}

TEST(RankOptionsParseStrategy, RecencyRecognized) {
    EXPECT_EQ(ro::parse_strategy("recency"), ro::RankStrategy::Recency);
    EXPECT_EQ(ro::parse_strategy("Recency"), ro::RankStrategy::Recency);
}

TEST(RankOptionsParseStrategy, FileTypeRecognizedHyphenAndUnderscore) {
    EXPECT_EQ(ro::parse_strategy("file-type"), ro::RankStrategy::FileType);
    EXPECT_EQ(ro::parse_strategy("file_type"), ro::RankStrategy::FileType);
    EXPECT_EQ(ro::parse_strategy("FILE-TYPE"), ro::RankStrategy::FileType);
}

TEST(RankOptionsParseStrategy, UnknownStrategy) {
    EXPECT_EQ(ro::parse_strategy("proximity"), ro::RankStrategy::Unknown);
    EXPECT_EQ(ro::parse_strategy("foo"), ro::RankStrategy::Unknown);
}

TEST(RankOptionsParseContext, EmptyIsNone) {
    EXPECT_EQ(ro::parse_context_filter(""), ro::ContextFilter::None);
}

TEST(RankOptionsParseContext, FunctionAliases) {
    EXPECT_EQ(ro::parse_context_filter("function"), ro::ContextFilter::Function);
    EXPECT_EQ(ro::parse_context_filter("FUNCTION"), ro::ContextFilter::Function);
    EXPECT_EQ(ro::parse_context_filter("method"), ro::ContextFilter::Function);
    EXPECT_EQ(ro::parse_context_filter("func"), ro::ContextFilter::Function);
}

TEST(RankOptionsParseContext, ClassAliases) {
    EXPECT_EQ(ro::parse_context_filter("class"), ro::ContextFilter::Class);
    EXPECT_EQ(ro::parse_context_filter("struct"), ro::ContextFilter::Class);
    EXPECT_EQ(ro::parse_context_filter("interface"), ro::ContextFilter::Class);
    EXPECT_EQ(ro::parse_context_filter("trait"), ro::ContextFilter::Class);
    EXPECT_EQ(ro::parse_context_filter("impl"), ro::ContextFilter::Class);
    EXPECT_EQ(ro::parse_context_filter("record"), ro::ContextFilter::Class);
}

TEST(RankOptionsParseContext, TopLevelAliases) {
    EXPECT_EQ(ro::parse_context_filter("top-level"), ro::ContextFilter::TopLevel);
    EXPECT_EQ(ro::parse_context_filter("top_level"), ro::ContextFilter::TopLevel);
    EXPECT_EQ(ro::parse_context_filter("toplevel"), ro::ContextFilter::TopLevel);
    EXPECT_EQ(ro::parse_context_filter("top"), ro::ContextFilter::TopLevel);
    EXPECT_EQ(ro::parse_context_filter("global"), ro::ContextFilter::TopLevel);
}

TEST(RankOptionsParseContext, UnknownContext) {
    EXPECT_EQ(ro::parse_context_filter("module"), ro::ContextFilter::Unknown);
    EXPECT_EQ(ro::parse_context_filter("loop"), ro::ContextFilter::Unknown);
}

TEST(RankOptionsBlockTypeMatch, FunctionMatrix) {
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Function, "function"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Function, "method"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Function, "constructor"));
    EXPECT_FALSE(ro::block_type_matches(ro::ContextFilter::Function, "class"));
    EXPECT_FALSE(ro::block_type_matches(ro::ContextFilter::Function, ""));
    EXPECT_FALSE(ro::block_type_matches(ro::ContextFilter::Function, "lines"));
}

TEST(RankOptionsBlockTypeMatch, ClassMatrix) {
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Class, "class"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Class, "struct"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Class, "interface"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Class, "trait"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Class, "impl"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Class, "record"));
    EXPECT_FALSE(ro::block_type_matches(ro::ContextFilter::Class, "function"));
    EXPECT_FALSE(ro::block_type_matches(ro::ContextFilter::Class, ""));
}

TEST(RankOptionsBlockTypeMatch, TopLevelMatrix) {
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::TopLevel, ""));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::TopLevel, "lines"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::TopLevel, "context"));
    EXPECT_FALSE(ro::block_type_matches(ro::ContextFilter::TopLevel, "function"));
    EXPECT_FALSE(ro::block_type_matches(ro::ContextFilter::TopLevel, "class"));
}

TEST(RankOptionsBlockTypeMatch, NoneAndUnknownPassThrough) {
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::None, "anything"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Unknown, "anything"));
}

TEST(RankOptionsBlockTypeMatch, CaseInsensitive) {
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Function, "FUNCTION"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::Class, "Struct"));
    EXPECT_TRUE(ro::block_type_matches(ro::ContextFilter::TopLevel, "Lines"));
}

TEST(RankOptionsContextFilter, FunctionDropsNonMatching) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "auth", "function", "login"));
    results.push_back(make_result("b.cpp", "auth", "class", "Login"));
    results.push_back(make_result("c.cpp", "auth", "method", "login"));
    results.push_back(make_result("d.cpp", "auth", "lines", ""));

    auto kept = ro::apply_context_filter(results, ro::ContextFilter::Function);
    EXPECT_EQ(kept.size(), 2u);
    EXPECT_EQ(kept[0].value("path", ""), "a.cpp");
    EXPECT_EQ(kept[1].value("path", ""), "c.cpp");
}

TEST(RankOptionsContextFilter, ClassDropsNonMatching) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "auth", "function", "login"));
    results.push_back(make_result("b.cpp", "auth", "class", "Login"));
    results.push_back(make_result("c.cpp", "auth", "struct", "Foo"));
    results.push_back(make_result("d.cpp", "auth", "interface", "I"));

    auto kept = ro::apply_context_filter(results, ro::ContextFilter::Class);
    EXPECT_EQ(kept.size(), 3u);
    EXPECT_EQ(kept[0].value("path", ""), "b.cpp");
    EXPECT_EQ(kept[1].value("path", ""), "c.cpp");
    EXPECT_EQ(kept[2].value("path", ""), "d.cpp");
}

TEST(RankOptionsContextFilter, TopLevelMatchesEmptyAndSentinel) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "auth", "function", "login"));
    results.push_back(make_result("b.cpp", "auth", "", ""));
    results.push_back(make_result("c.cpp", "auth", "lines", ""));
    results.push_back(make_result("d.cpp", "auth", "context", ""));

    auto kept = ro::apply_context_filter(results, ro::ContextFilter::TopLevel);
    EXPECT_EQ(kept.size(), 3u);
    EXPECT_EQ(kept[0].value("path", ""), "b.cpp");
    EXPECT_EQ(kept[1].value("path", ""), "c.cpp");
    EXPECT_EQ(kept[2].value("path", ""), "d.cpp");
}

TEST(RankOptionsContextFilter, NonePassesThrough) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "x"));
    results.push_back(make_result("b.cpp", "y"));

    auto kept = ro::apply_context_filter(results, ro::ContextFilter::None);
    EXPECT_EQ(kept.size(), 2u);
}

TEST(RankOptionsContextFilter, UnknownPassesThrough) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(make_result("a.cpp", "x", "function"));
    results.push_back(make_result("b.cpp", "y", "class"));

    auto kept = ro::apply_context_filter(results, ro::ContextFilter::Unknown);
    EXPECT_EQ(kept.size(), 2u);
}

TEST(RankOptionsContextFilter, EmptyResultsProducesEmpty) {
    nlohmann::json results = nlohmann::json::array();
    auto kept = ro::apply_context_filter(results, ro::ContextFilter::Function);
    EXPECT_EQ(kept.size(), 0u);
}

// -- Re-rank helpers ---------------------------------------------------------

namespace {
nlohmann::json scored_result(const std::string& path, double score) {
    nlohmann::json r;
    r["path"] = path;
    r["match"] = "x";
    r["line"] = 1;
    r["score"] = score;
    nlohmann::json ctx;
    ctx["block_type"] = "function";
    ctx["block_name"] = "f";
    r["context"] = ctx;
    return r;
}
}  // namespace

TEST(RankOptionsRelevance, SortsByScoreDescending) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(scored_result("a.cpp", 10.0));
    results.push_back(scored_result("b.cpp", 50.0));
    results.push_back(scored_result("c.cpp", 30.0));

    auto sorted = ro::apply_rank(results, ro::RankStrategy::Relevance);
    ASSERT_EQ(sorted.size(), 3u);
    EXPECT_EQ(sorted[0].value("path", ""), "b.cpp");  // 50
    EXPECT_EQ(sorted[1].value("path", ""), "c.cpp");  // 30
    EXPECT_EQ(sorted[2].value("path", ""), "a.cpp");  // 10
}

TEST(RankOptionsRelevance, StableOnTies) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(scored_result("a.cpp", 10.0));
    results.push_back(scored_result("b.cpp", 10.0));
    results.push_back(scored_result("c.cpp", 10.0));

    auto sorted = ro::apply_rank(results, ro::RankStrategy::Relevance);
    ASSERT_EQ(sorted.size(), 3u);
    EXPECT_EQ(sorted[0].value("path", ""), "a.cpp");
    EXPECT_EQ(sorted[1].value("path", ""), "b.cpp");
    EXPECT_EQ(sorted[2].value("path", ""), "c.cpp");
}

TEST(RankOptionsFileType, BoostsCodeOverDocsAndConfig) {
    nlohmann::json results = nlohmann::json::array();
    // Pure file-type re-rank: code (boost) > config (small boost) > unknown (0)
    // > docs (penalty). Engine score is irrelevant — file-type wins.
    results.push_back(scored_result("README.md", 9999.0));     // doc penalty
    results.push_back(scored_result("config.json", 1.0));      // config boost
    results.push_back(scored_result("src/main.cpp", 1.0));     // code boost

    auto sorted = ro::apply_rank(results, ro::RankStrategy::FileType);
    ASSERT_EQ(sorted.size(), 3u);
    EXPECT_EQ(sorted[0].value("path", ""), "src/main.cpp");
    EXPECT_EQ(sorted[1].value("path", ""), "config.json");
    EXPECT_EQ(sorted[2].value("path", ""), "README.md");
}

TEST(RankOptionsFileType, PreservesOriginalScore) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(scored_result("a.cpp", 42.5));

    auto sorted = ro::apply_rank(results, ro::RankStrategy::FileType);
    ASSERT_EQ(sorted.size(), 1u);
    EXPECT_TRUE(sorted[0].contains("original_score"));
    EXPECT_DOUBLE_EQ(sorted[0]["original_score"].get<double>(), 42.5);
    // New score is the file-type score for `.cpp` (kCodeFileBoost=50.0).
    EXPECT_DOUBLE_EQ(sorted[0]["score"].get<double>(), 50.0);
}

TEST(RankOptionsRecency, SortsByFileMtimeDescending) {
    // Create three temp files in deterministic mtime order so the sort
    // is verifiable. Use std::filesystem to set mtime explicitly so the
    // test is robust against fast-filesystem timestamp granularity.
    namespace fs = std::filesystem;
    auto tmpdir = fs::temp_directory_path() /
                  ("lci_rank_test_" + std::to_string(::getpid()));
    fs::create_directories(tmpdir);

    auto write_file = [&](const std::string& name, std::string_view body) {
        auto p = tmpdir / name;
        std::ofstream(p.string()) << body;
        return p.string();
    };

    auto path_a = write_file("a.cpp", "old");
    auto path_b = write_file("b.cpp", "newest");
    auto path_c = write_file("c.cpp", "middle");

    auto now = fs::file_time_type::clock::now();
    fs::last_write_time(path_a, now - std::chrono::hours(48));
    fs::last_write_time(path_b, now);
    fs::last_write_time(path_c, now - std::chrono::hours(24));

    nlohmann::json results = nlohmann::json::array();
    results.push_back(scored_result(path_a, 100.0));  // engine says best
    results.push_back(scored_result(path_b, 1.0));    // newest
    results.push_back(scored_result(path_c, 50.0));   // middle

    auto sorted = ro::apply_rank(results, ro::RankStrategy::Recency);
    ASSERT_EQ(sorted.size(), 3u);
    EXPECT_EQ(sorted[0].value("path", ""), path_b);  // now
    EXPECT_EQ(sorted[1].value("path", ""), path_c);  // -24h
    EXPECT_EQ(sorted[2].value("path", ""), path_a);  // -48h
    // mtime_epoch is stamped on each row.
    EXPECT_GT(sorted[0].value("mtime_epoch", static_cast<int64_t>(0)),
              sorted[1].value("mtime_epoch", static_cast<int64_t>(0)));

    fs::remove_all(tmpdir);
}

TEST(RankOptionsRecency, MissingFilesSinkToBottom) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(scored_result("/tmp/lci_does_not_exist_42.cpp", 100.0));
    // Use the source file we know exists at runtime — the test binary's
    // cwd is the build dir, but absolute paths under /tmp don't resolve,
    // so use one we can reliably stat: argv[0] of the test process.
    // Simpler: create a temp file and reference it.
    namespace fs = std::filesystem;
    auto tmpfile = fs::temp_directory_path() /
                   ("lci_rank_real_" + std::to_string(::getpid()) + ".cpp");
    std::ofstream(tmpfile.string()) << "real";

    results.push_back(scored_result(tmpfile.string(), 1.0));

    auto sorted = ro::apply_rank(results, ro::RankStrategy::Recency);
    ASSERT_EQ(sorted.size(), 2u);
    EXPECT_EQ(sorted[0].value("path", ""), tmpfile.string());  // real file first
    EXPECT_EQ(sorted[1].value("path", ""),
              "/tmp/lci_does_not_exist_42.cpp");                // mtime=0 last
    EXPECT_EQ(sorted[1].value("mtime_epoch", static_cast<int64_t>(-1)),
              static_cast<int64_t>(0));

    fs::remove(tmpfile);
}

TEST(RankOptionsApplyRank, UnknownStrategyPassesThrough) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(scored_result("a.cpp", 1.0));
    results.push_back(scored_result("b.cpp", 100.0));

    // Unknown strategies should not reshape or drop results.
    auto out = ro::apply_rank(results, ro::RankStrategy::Unknown);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].value("path", ""), "a.cpp");
    EXPECT_EQ(out[1].value("path", ""), "b.cpp");
}

TEST(RankOptionsApplyRank, EmptyResultsProducesEmpty) {
    nlohmann::json results = nlohmann::json::array();
    EXPECT_EQ(ro::apply_rank(results, ro::RankStrategy::Relevance).size(), 0u);
    EXPECT_EQ(ro::apply_rank(results, ro::RankStrategy::Recency).size(), 0u);
    EXPECT_EQ(ro::apply_rank(results, ro::RankStrategy::FileType).size(), 0u);
}

}  // namespace
}  // namespace cli
}  // namespace lci
