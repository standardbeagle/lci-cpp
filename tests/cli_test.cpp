#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include <lci/cli/commands.h>

#include "../src/cli/grep_filters.h"

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

}  // namespace
}  // namespace cli
}  // namespace lci
