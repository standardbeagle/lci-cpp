#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include <lci/cli/commands.h>
#include <lci/core/portable.h>
#include <lci/core/subprocess.h>
#include <lci/language_map.h>
#include <lci/search/search_options.h>

#include "../src/cli/ast_filters.h"
#include "../src/cli/grep_filters.h"
#include "../src/cli/name_aggregation.h"
#include "../src/cli/query_parser.h"
#include "../src/cli/rank_options.h"
#include "../src/cli/symbol_filters.h"
#include "../src/cli/tree_formatter.h"
#include "unique_temp.h"

namespace lci {
namespace cli {

// Validates the JSON shape run_callers renders in text mode. Returns false
// with a message naming the offending key when a required key is missing or
// mistyped; never throws on absent keys. Defined in src/cli/commands.cpp.
bool callers_report_valid(const nlohmann::json& report, std::string& error);

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
    auto tmp = std::filesystem::temp_directory_path();
    flags.root = tmp.string();
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(cfg.project.root, std::filesystem::absolute(tmp).string());
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

// -- def zero-result diagnosis helpers ----------------------------------------

TEST(CliDefDiagnosisTest, RecognizesPythonFromImport) {
    EXPECT_TRUE(line_imports_symbol("from joblib import effective_n_jobs",
                                    "effective_n_jobs"));
    EXPECT_EQ(import_module_of("from joblib import effective_n_jobs"), "joblib");
}

TEST(CliDefDiagnosisTest, RecognizesPythonFromImportIndented) {
    EXPECT_TRUE(line_imports_symbol("    from joblib import Memory", "Memory"));
    EXPECT_EQ(import_module_of("    from joblib import Memory"), "joblib");
}

TEST(CliDefDiagnosisTest, RecognizesPlainImport) {
    EXPECT_TRUE(line_imports_symbol("import numpy", "numpy"));
    // Plain import has no `from` module; caller prints the line verbatim.
    EXPECT_EQ(import_module_of("import numpy"), "");
}

TEST(CliDefDiagnosisTest, RejectsUsageLine) {
    // A call site mentions the symbol but is not an import — must not be
    // misclassified as an import site.
    EXPECT_FALSE(line_imports_symbol("    n = effective_n_jobs(self.n_jobs)",
                                     "effective_n_jobs"));
}

TEST(CliDefDiagnosisTest, RejectsImportLineWithoutSymbol) {
    // The line is an import, but of a different symbol — not this one's site.
    EXPECT_FALSE(
        line_imports_symbol("from joblib import Parallel", "effective_n_jobs"));
}

TEST(CliDefDiagnosisTest, DoesNotMatchImportInsideIdentifier) {
    // "important" contains "import" as a substring but is not an import stmt.
    EXPECT_FALSE(
        line_imports_symbol("important = compute_important(x)", "important"));
}

// -- refs ranking (code-context vs lexical partition) -------------------------

namespace {

ReferenceLocation make_ref(std::string file, int line, int column,
                           std::string context) {
    ReferenceLocation r;
    r.file_path = std::move(file);
    r.line = line;
    r.column = column;
    r.context = std::move(context);
    return r;
}

}  // namespace

// The core discrimination: a `@deprecated` decorator usage is a code
// reference, while the same word inside a docstring is lexical-only noise.
// Column is a 0-based byte offset (the one column contract,
// include/lci/cli/column.h) and points at the match ("deprecated").
TEST(CliRefsPartitionTest, DecoratorIsCodeDocstringIsLexical) {
    std::vector<ReferenceLocation> refs = {
        // Docstring hit — appears FIRST in the raw text-search results, which
        // is exactly the noise-outranks-code failure we are fixing.
        make_ref("a.py", 3, 7, "    \"\"\"This is deprecated behavior.\"\"\""),
        // Decorator usage — real code reference.
        make_ref("b.py", 10, 1, "@deprecated"),
    };

    PartitionedReferences parts = partition_references(refs);

    ASSERT_EQ(parts.code.size(), 1u);
    ASSERT_EQ(parts.lexical.size(), 1u);
    EXPECT_EQ(parts.code[0].file_path, "b.py");
    EXPECT_EQ(parts.lexical[0].file_path, "a.py");
}

// The partition must produce a top-to-bottom ordering where every code
// reference precedes every lexical-only one, regardless of the input order.
TEST(CliRefsPartitionTest, CodeAlwaysPrecedesLexicalInMergedOrder) {
    std::vector<ReferenceLocation> refs = {
        make_ref("doc.py", 1, 4, "# deprecated: use foo instead"),  // comment
        make_ref("use.py", 2, 0, "deprecated(func)"),               // call
        make_ref("s.py", 3, 11, "msg = \"deprecated api\""),        // string
        make_ref("imp.py", 4, 19, "from x import deprecated"),      // import
    };

    PartitionedReferences parts = partition_references(refs);

    // Merge as run_refs prints: code first, then lexical.
    std::vector<std::string> order;
    for (const auto& r : parts.code) order.push_back(r.file_path);
    for (const auto& r : parts.lexical) order.push_back(r.file_path);

    // Code refs (call + import) precede lexical refs (comment + string), and
    // each group keeps its original relative order.
    EXPECT_EQ(order, (std::vector<std::string>{"use.py", "imp.py", "doc.py",
                                               "s.py"}));
}

// A ref with no line text cannot be classified; keep it as code-context so a
// possibly-real reference is never silently hidden.
TEST(CliRefsPartitionTest, EmptyContextIsKeptAsCode) {
    std::vector<ReferenceLocation> refs = {make_ref("x.py", 1, -1, "")};
    PartitionedReferences parts = partition_references(refs);
    EXPECT_EQ(parts.code.size(), 1u);
    EXPECT_TRUE(parts.lexical.empty());
}

// The load-bearing case for real corpora: sklearn docstrings are multi-line
// triple-quoted strings, so a `deprecated` mention on an INTERIOR line carries
// no quote of its own and escapes the single-line classifiers. The mask must
// flag those interior lines while leaving surrounding code (the decorator,
// the def) unmarked — otherwise docstring prose outranks a `@deprecated` usage.
TEST(CliRefsPartitionTest, MultiLineDocstringInteriorLinesAreMasked) {
    std::vector<std::string> lines = {
        "@deprecated(\"1.9\")",           // 1: decorator — code
        "def f(x):",                       // 2: code
        "    \"\"\"Short summary.",        // 3: opens docstring
        "",                                // 4: interior
        "    .. deprecated:: 1.9",         // 5: interior prose (the noise)
        "       use g instead.",           // 6: interior
        "    \"\"\"",                       // 7: closes docstring
        "    return deprecated_call(x)",   // 8: code
    };

    std::vector<bool> mask = python_docstring_line_mask(lines);
    ASSERT_EQ(mask.size(), lines.size());

    EXPECT_FALSE(mask[0]);  // @deprecated decorator is code, never masked
    EXPECT_FALSE(mask[1]);  // def line
    EXPECT_FALSE(mask[2]);  // opener line handled by single-line classifier
    EXPECT_TRUE(mask[3]);   // interior blank
    EXPECT_TRUE(mask[4]);   // ".. deprecated:: 1.9" — the docstring noise
    EXPECT_TRUE(mask[5]);   // interior prose
    EXPECT_FALSE(mask[7]);  // code after the docstring closes
}

// Single-line triple-quoted strings must not leave the scanner stuck "inside"
// a docstring for the rest of the file.
TEST(CliRefsPartitionTest, SameLineTripleQuoteDoesNotLeak) {
    std::vector<std::string> lines = {
        "x = \"\"\"one line deprecated\"\"\"",  // opens and closes
        "y = deprecated(x)",                     // must be code
    };
    std::vector<bool> mask = python_docstring_line_mask(lines);
    EXPECT_FALSE(mask[0]);
    EXPECT_FALSE(mask[1]);
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
    lci::portable::set_env("LCI_MCP_MODE", "1");
    EXPECT_TRUE(is_mcp_mode());
    if (old) {
        lci::portable::set_env("LCI_MCP_MODE", old);
    } else {
        lci::portable::unset_env("LCI_MCP_MODE");
    }
}

// -- -c/--config threading ----------------------------------------------------

TEST(CliConfigFlagTest, NamedConfigFileIsActuallyLoaded) {
    // The flag was parsed and then discarded: every command read
    // <root>/.lci.kdl no matter what the user named.
    auto dir = lci::test::unique_temp_dir("lci_cli_config_flag_");
    std::filesystem::create_directories(dir);

    {
        std::ofstream f(dir / ".lci.kdl");
        f << "search {\n  max_results 11\n}\n";
    }
    {
        std::ofstream f(dir / "alt.kdl");
        f << "search {\n  max_results 77\n}\n";
    }

    GlobalFlags flags;
    flags.root = dir.string();
    flags.config_path = (dir / "alt.kdl").string();

    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(cfg.search.max_results, 77);

    // Discrimination partner: the default path still reads .lci.kdl.
    GlobalFlags defaults;
    defaults.root = dir.string();
    Config default_cfg;
    err = load_config_with_overrides(defaults, default_cfg);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(default_cfg.search.max_results, 11);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(CliConfigFlagTest, MissingNamedConfigFileFailsFast) {
    auto dir = lci::test::unique_temp_dir("lci_cli_config_missing_");
    std::filesystem::create_directories(dir);

    GlobalFlags flags;
    flags.root = dir.string();
    flags.config_path = (dir / "nope.kdl").string();

    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("nope.kdl"), std::string::npos) << err;

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// -- config init command tests (no server needed) -----------------------------

TEST(CliConfigInitTest, KdlFormatCreatesFile) {
    std::string test_file =
        (std::filesystem::temp_directory_path() / "lci_test_config_init.kdl")
            .string();
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
    std::string test_file =
        (std::filesystem::temp_directory_path() / "lci_test_config_init_min.kdl")
            .string();
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

TEST(CliConfigInitTest, YamlFormatIsRejected) {
    // `-f yaml` used to default its output name to ".lci.kdl" and write YAML
    // into it, so the next `lci` run failed to parse the config it had just
    // generated. The KDL loader is the only loader that exists.
    std::string test_file =
        (std::filesystem::temp_directory_path() / "lci_test_config_init.yaml")
            .string();
    std::remove(test_file.c_str());

    GlobalFlags flags;
    EXPECT_EQ(run_config_init(flags, "yaml", test_file, false, false), 1);
    EXPECT_FALSE(std::filesystem::exists(test_file));

    // Refused before any default filename is chosen, so no ".lci.kdl" is
    // clobbered either.
    EXPECT_EQ(run_config_init(flags, "yaml", "", false, false), 1);
}

TEST(CliConfigInitTest, JsonFormatCreatesValidJson) {
    std::string test_file =
        (std::filesystem::temp_directory_path() / "lci_test_config_init.json")
            .string();
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

TEST(CliConfigInitTest, RefusesOverwriteWithoutForce) {
    std::string test_file =
        (std::filesystem::temp_directory_path() / "lci_test_config_exists.kdl")
            .string();
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
    std::string xml_path =
        (std::filesystem::temp_directory_path() / "lci_test.xml").string();
    int rc = run_config_init(flags, "xml", xml_path, false, false);
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

// -- nested subcommand + global flag fallthrough ------------------------------
// `lci config show -r <dir>` must parse: -r is a global option of the root
// app, accepted after a nested subcommand. main.cpp sets fallthrough() only
// on top-level subcommands, so CLI11 rejects the trailing -r with
// "argument was not expected" when the subcommand is nested (config show).

TEST(CliSubcommandTest, NestedSubcommandAcceptsGlobalRootFlag) {
    // Requires the built `lci` binary next to this test tree
    // (build/<preset>/src/lci); the test binary lives in build/<preset>/tests.
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;

    const auto root = lci::test::unique_temp_dir("lci_cli_subcmd_");
    fs::create_directories(root);
    std::string out;
    EXPECT_TRUE(subprocess::run_capture(
        {lci_bin.string(), "config", "show", "-r", root.string()}, "", out));
    std::error_code ec;
    fs::remove_all(root, ec);
}

// -- arrow patterns through the real binary ----------------------------------
// `lci search '->next'`: CLI11 never assigns a dash-prefixed token to a
// positional, so without the argv escape in main.cpp the parse aborts with
// "pattern is required" BEFORE query_parser.h runs. A parser unit test
// cannot observe that rejection — these drive the built binary.

TEST(CliSubcommandTest, ArrowPatternParsesAsContent) {
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;

    const auto root = lci::test::unique_temp_dir("lci_cli_arrow_");
    fs::create_directories(root);
    {
        std::ofstream f(root / "a.cpp");
        f << "struct N { N* next; };\n"
             "int go(N* n) { return n ->next != nullptr; }\n"
             "int other(N* n) { return n->prev != nullptr; }\n";
    }

    // Bare arrow token: normal search exit + the match, not a parse error.
    std::string out;
    EXPECT_TRUE(subprocess::run_capture(
        {lci_bin.string(), "search", "->next", "-r", root.string()}, "",
        out));
    EXPECT_NE(out.find("a.cpp"), std::string::npos) << out;

    // Arrow token after a term stays content, not an exclusion (guards the
    // query_parser gate from 4a0080a end to end).
    out.clear();
    EXPECT_TRUE(subprocess::run_capture(
        {lci_bin.string(), "search", "n ->next", "-r", root.string()}, "",
        out));
    EXPECT_NE(out.find("a.cpp"), std::string::npos) << out;

    std::string ignored;
    subprocess::run_capture(
        {lci_bin.string(), "shutdown", "-r", root.string()}, "", ignored);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(CliSubcommandTest, QuotedExclusionStillParses) {
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;

    const auto root = lci::test::unique_temp_dir("lci_cli_excl_");
    fs::create_directories(root);
    {
        std::ofstream f(root / "b.txt");
        f << "foo bar\nfoo baz\n";
    }

    // `lci search 'foo -bar'` must run as a normal search — the `-bar`
    // exclusion directive is query_parser's job (PlainExclusionStillWorks),
    // and the argv escape must not disturb it.
    std::string out;
    EXPECT_TRUE(subprocess::run_capture(
        {lci_bin.string(), "search", "foo -bar", "-r", root.string()}, "",
        out));
    EXPECT_NE(out.find("b.txt"), std::string::npos) << out;

    std::string ignored;
    subprocess::run_capture(
        {lci_bin.string(), "shutdown", "-r", root.string()}, "", ignored);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(CliSubcommandTest, UnknownOptionStillErrors) {
    // The arrow fix must not disable option validation: a genuine unknown
    // option is still a parse error, not silently swallowed as content.
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;

    const auto root = lci::test::unique_temp_dir("lci_cli_badopt_");
    fs::create_directories(root);
    std::string out;
    EXPECT_FALSE(subprocess::run_capture(
        {lci_bin.string(), "search", "foo", "--nosuchflag", "-r",
         root.string()},
        "", out));
    std::error_code ec;
    fs::remove_all(root, ec);
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

// -- Path-scope resolution (blocker 2: root-relative normalization) ----------
//
// resolve_scope_paths must convert an absolute or cwd-relative positional into
// the root-relative form the index matches against. Purely lexical, so these
// exercise it with synthetic root/cwd values and no disk access.

TEST(GrepScopePaths, AbsolutePathUnderRootBecomesRootRelative) {
    auto out = gf::resolve_scope_paths({"/home/u/proj/src/a.cpp"},
                                       "/home/u/proj", "/tmp");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out.front(), "src/a.cpp");
}

TEST(GrepScopePaths, CwdRelativeFromSubdirBecomesRootRelative) {
    // Run from a subdirectory whose cwd != indexed root: a bare relative token
    // must resolve against the root, not silently match nothing.
    auto out = gf::resolve_scope_paths({"utils"}, "/home/u/proj",
                                       "/home/u/proj/pkg");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out.front(), "pkg/utils");
}

TEST(GrepScopePaths, RelativeTokenAtRootAndDotSlashNormalize) {
    // cwd == root: relative token unchanged; leading ./ stripped.
    EXPECT_EQ(gf::resolve_scope_paths({"src/a.cpp"}, "/home/u/proj",
                                      "/home/u/proj")
                  .front(),
              "src/a.cpp");
    EXPECT_EQ(gf::resolve_scope_paths({"./src"}, "/home/u/proj", "/home/u/proj")
                  .front(),
              "src");
}

TEST(GrepScopePaths, PathEscapingRootLeftUnchanged) {
    // A path outside the indexed root cannot be expressed root-relative; leave
    // it verbatim so the server-side index-membership check rejects it loudly.
    auto out = gf::resolve_scope_paths({"/etc/passwd"}, "/home/u/proj",
                                       "/home/u/proj");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out.front(), "/etc/passwd");
}

// -- regex_literal_seeds ------------------------------------------------------
//
// The seed set for `grep -E` / `search -E`. The former single longest-run
// seed was alternation-blind: err-lookup's production Rust detector
// `\b(?:panic|unreachable|todo|unimplemented)!\s*\(` seeded only
// "unimplemented" and returned 1 of 1203 real sites.

TEST(RegexLiteralSeeds, AlternationYieldsEveryBranch) {
    auto seeds = grep_filters::regex_literal_seeds(
        R"(\b(?:panic|unreachable|todo|unimplemented)!\s*\()");
    EXPECT_EQ(seeds, (std::vector<std::string>{
                         "panic", "unreachable", "todo", "unimplemented"}));
}

TEST(RegexLiteralSeeds, HttpStatusAlternation) {
    auto seeds = grep_filters::regex_literal_seeds(
        R"(\b(?:status|sendStatus|writeHead)\s*\(\s*[45]\d\d\b)");
    EXPECT_EQ(seeds, (std::vector<std::string>{"status", "sendStatus",
                                               "writeHead"}));
}

TEST(RegexLiteralSeeds, EscapedDotJoinsRuns) {
    auto seeds =
        grep_filters::regex_literal_seeds(R"(\berrors\.New\s*\()");
    EXPECT_EQ(seeds, (std::vector<std::string>{"errors.New"}));
}

TEST(RegexLiteralSeeds, QuantifierDropsPrecedingChar) {
    // `abc*` can match "ab" — the run must not include the quantified char.
    auto star = grep_filters::regex_literal_seeds("abcd*efg");
    EXPECT_EQ(star, (std::vector<std::string>{"abc", "efg"}));
    // `+` requires its char, so the full run stands.
    auto plus = grep_filters::regex_literal_seeds("abcd+");
    EXPECT_EQ(plus, (std::vector<std::string>{"abcd"}));
    auto brace = grep_filters::regex_literal_seeds("abcd{2,3}x");
    EXPECT_EQ(brace, (std::vector<std::string>{"abc"}));
}

TEST(RegexLiteralSeeds, ShortRunsAndPureMetaYieldNothing) {
    EXPECT_TRUE(grep_filters::regex_literal_seeds(R"(\d+)").empty());
    EXPECT_TRUE(grep_filters::regex_literal_seeds("a|b").empty());
}

TEST(RegexLiteralSeeds, DuplicateRunsDedup) {
    auto seeds = grep_filters::regex_literal_seeds("foo.*foo.*bar");
    EXPECT_EQ(seeds, (std::vector<std::string>{"foo", "bar"}));
}

// -- regex_every_match_has_seed ------------------------------------------------
//
// The per-branch decision behind the S12.6 fix: the trigram-seeded fast path
// is sound only when EVERY match of the pattern carries a bankable seed. A
// false here routes `lci search -E` to the full scan (and fails `lci grep -E`
// loudly); a true keeps the indexed path. These pin BOTH directions so the
// fallback stays per-branch and never degrades into a blanket full scan.

TEST(AlternationSeedTest, UnseededBranchForcesFallback) {
    // The root-cause pattern: "ab" has no >=3-char literal.
    EXPECT_FALSE(grep_filters::regex_every_match_has_seed("foobar|ab"));
    // Nested group, same defect shape.
    EXPECT_FALSE(grep_filters::regex_every_match_has_seed("(foobar|ab)"));
    // Optional group: `x(?:abc)?` can match "x", which carries no seed.
    EXPECT_FALSE(grep_filters::regex_every_match_has_seed("x(?:abc)?"));
    // Quantified-away runs: `ab*` can match "a"; `abc{0,2}` only guarantees
    // "ab" (the `{0,2}` makes the 'c' optional) — neither reaches 3 chars.
    EXPECT_FALSE(grep_filters::regex_every_match_has_seed("ab*"));
    EXPECT_FALSE(grep_filters::regex_every_match_has_seed("abc{0,2}"));
    // Pure meta.
    EXPECT_FALSE(grep_filters::regex_every_match_has_seed(R"(\d+)"));
}

TEST(AlternationSeedTest, SeededPatternKeepsTrigramIndex) {
    // Every branch seedable -> indexed fast path stays.
    EXPECT_TRUE(grep_filters::regex_every_match_has_seed("foobar|barbaz"));
    EXPECT_TRUE(grep_filters::regex_every_match_has_seed("foobar"));
    // A required outer literal covers an unseedable inner alternation:
    // every match of `foo(bar|ab)` contains "foo".
    EXPECT_TRUE(grep_filters::regex_every_match_has_seed("foo(bar|ab)"));
    // `+` requires its char; `*` only drops the one quantified char.
    EXPECT_TRUE(grep_filters::regex_every_match_has_seed("abcd+"));
    EXPECT_TRUE(grep_filters::regex_every_match_has_seed("abcde*"));
    // The production detector that motivated the multi-seed extractor.
    EXPECT_TRUE(grep_filters::regex_every_match_has_seed(
        R"(\b(?:panic|unreachable|todo|unimplemented)!\s*\()"));
}

// The one comment-classification predicate, shared by CLI and MCP.
TEST(GrepFiltersComment, LineSlashSlashIsComment) {
    EXPECT_TRUE(lci::line_is_comment_only("// hello", LangId::Cpp));
    EXPECT_TRUE(lci::line_is_comment_only("    // indented", LangId::Cpp));
    EXPECT_TRUE(lci::line_is_comment_only("\t// tabbed", LangId::Cpp));
}

TEST(GrepFiltersComment, LineHashIsLanguageGated) {
    // '#' is a comment only where the language says so: Python/Ruby yes,
    // C/C++ no (2,345 '#' lines under src/+include/ are ALL preprocessor
    // code), PHP yes except the '#[' attribute syntax.
    EXPECT_TRUE(lci::line_is_comment_only("# python", LangId::Python));
    EXPECT_TRUE(lci::line_is_comment_only("# note", LangId::Ruby));
    EXPECT_TRUE(lci::line_is_comment_only("# note", LangId::PHP));
    EXPECT_FALSE(lci::line_is_comment_only("#[Route('/x')]", LangId::PHP));
    EXPECT_FALSE(lci::line_is_comment_only("  #include", LangId::Cpp));
}

TEST(GrepFiltersComment, LineSlashStarIsComment) {
    EXPECT_TRUE(lci::line_is_comment_only("/* opening", LangId::Cpp));
    EXPECT_TRUE(
        lci::line_is_comment_only("   /* indented opener", LangId::Cpp));
}

TEST(GrepFiltersComment, StarSlashAnywhereDoesNotMakeAComment) {
    // The defect this task fixed: a line CONTAINING `*/` is not
    // comment-only unless the marker is the whole line. These are real
    // code (trailing comment, string literal) and must be KEPT.
    EXPECT_FALSE(
        lci::line_is_comment_only("int x = 1; /* note */", LangId::Cpp));
    EXPECT_FALSE(
        lci::line_is_comment_only("std::string s = \"*/\";", LangId::Cpp));
    // Accepted residual (chosen, not overlooked): prose that merely CLOSES
    // a block comment is kept — undecidable from one line, and the safe
    // direction is keep. Exactly `*/` alone is still a comment line.
    EXPECT_FALSE(lci::line_is_comment_only("done */", LangId::Cpp));
    EXPECT_FALSE(lci::line_is_comment_only("payload */ trailing", LangId::Cpp));
    EXPECT_TRUE(lci::line_is_comment_only("*/", LangId::Cpp));
}

TEST(GrepFiltersComment, PlainCodeIsNotComment) {
    EXPECT_FALSE(lci::line_is_comment_only("int x = 42;", LangId::Cpp));
    EXPECT_FALSE(lci::line_is_comment_only("    foo(\"//\", x);", LangId::Cpp));
    EXPECT_FALSE(lci::line_is_comment_only("string s = \"# not a comment\";",
                                           LangId::Cpp));
}

TEST(GrepFiltersComment, EmptyOrWhitespaceIsNotComment) {
    EXPECT_FALSE(lci::line_is_comment_only("", LangId::Cpp));
    EXPECT_FALSE(lci::line_is_comment_only("   ", LangId::Cpp));
    EXPECT_FALSE(lci::line_is_comment_only("\t\t", LangId::Cpp));
}

TEST(GrepFiltersComment, MultilineBlockBodyNotDetected) {
    // Known limitation: a line inside `/* ... */` that doesn't open with a
    // marker is NOT classified as a comment ("* more prose" and a
    // dereference like "*p = x" are the same shape on one line).
    EXPECT_FALSE(
        lci::line_is_comment_only("inside block comment", LangId::Cpp));
    EXPECT_FALSE(lci::line_is_comment_only(" * more prose", LangId::Cpp));
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

TEST(GrepFiltersApplyExcludeComments, KeepsCodeLineContainingStarSlash) {
    // DATA LOSS regression pin: a line is comment-only only when it OPENS
    // with a comment marker. `int x = 1; /* note */` is real code with a
    // trailing comment and `std::string s = "*/";` carries the marker inside
    // a string literal. The pre-fix CLI heuristic classified ANY line
    // containing `*/` as comment-only and silently dropped both rows.
    auto make_row = [](int line, const std::string& text) {
        nlohmann::json row;
        row["path"] = "/no/such/file.cpp";
        row["line"] = line;
        row["context"] = {{"start_line", line}, {"lines", {text}}};
        return row;
    };
    nlohmann::json results = nlohmann::json::array(
        {make_row(1, "int x = 1; /* note */"),
         make_row(2, "std::string s = \"*/\";")});
    auto filtered = gf::apply_exclude_comments(results);
    ASSERT_EQ(filtered.size(), 2u)
        << "comment filter deleted real code lines containing `*/`";
}

TEST(GrepFiltersApplyExcludeComments, DropsLinesOpeningWithCommentMarker) {
    // Retained behavior guard (passes pre-fix): lines that OPEN with a
    // comment marker stay dropped. `#` is language-gated, so the hash row
    // uses a .py path where `#` unambiguously opens a comment.
    auto make_row = [](const std::string& path, int line,
                       const std::string& text) {
        nlohmann::json row;
        row["path"] = path;
        row["line"] = line;
        row["context"] = {{"start_line", line}, {"lines", {text}}};
        return row;
    };
    nlohmann::json results = nlohmann::json::array(
        {make_row("/no/such/a.cpp", 1, "// line comment"),
         make_row("/no/such/b.py", 2, "  # hash comment"),
         make_row("/no/such/c.cpp", 3, "/* block opener"),
         make_row("/no/such/d.cpp", 4, "*/"),
         make_row("/no/such/e.cpp", 5, "int kept = 1;")});
    auto filtered = gf::apply_exclude_comments(results);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0]["line"].get<int>(), 5);
}

TEST(GrepFiltersApplyExcludeComments, BlockCloseProseIsKeptResidual) {
    // ACCEPTED RESIDUAL, asserted so it reads as chosen, not overlooked: a
    // line that merely CONTAINS `*/` (closing a block comment opened on an
    // earlier line) is KEPT. Undecidable from one line without cross-line
    // state, and the trade is deliberately asymmetric — keeping a comment
    // line is noise; deleting a code line is a wrong answer.
    auto make_row = [](int line, const std::string& text) {
        nlohmann::json row;
        row["path"] = "/no/such/file.cpp";
        row["line"] = line;
        row["context"] = {{"start_line", line}, {"lines", {text}}};
        return row;
    };
    nlohmann::json results = nlohmann::json::array(
        {make_row(1, "done */"), make_row(2, " * more prose")});
    auto filtered = gf::apply_exclude_comments(results);
    ASSERT_EQ(filtered.size(), 2u)
        << "accepted residual: block-close/continuation prose is kept";
}

TEST(GrepFiltersApplyExcludeComments, AgreesWithSharedPredicate) {
    // Anti-drift pin: the CLI filter and the MCP-side shared predicate
    // (lci::line_is_comment_only) must agree line-for-line on the same
    // input, so the two paths cannot diverge again.
    const std::vector<std::string> lines = {
        "// line comment",
        "   // indented",
        "# hash comment",
        "#include <vector>",
        "/* block opener",
        "int x = 1; /* note */",
        "std::string s = \"*/\";",
        "done */",
        "*/",
        " * more prose",
        "int plain = 42;",
        "",
    };
    const std::string path = "/no/such/file.py";
    nlohmann::json results = nlohmann::json::array();
    for (size_t i = 0; i < lines.size(); ++i) {
        nlohmann::json row;
        row["path"] = path;
        row["line"] = static_cast<int>(i + 1);
        row["context"] = {{"start_line", static_cast<int>(i + 1)},
                          {"lines", {lines[i]}}};
        results.push_back(std::move(row));
    }
    auto filtered = gf::apply_exclude_comments(results);
    std::set<int> kept_lines;
    for (const auto& row : filtered) {
        kept_lines.insert(row["line"].get<int>());
    }
    const LangId lang = language_info_for_path(path).language;
    for (size_t i = 0; i < lines.size(); ++i) {
        bool expect_kept = !lci::line_is_comment_only(lines[i], lang);
        EXPECT_EQ(kept_lines.contains(static_cast<int>(i + 1)), expect_kept)
            << "CLI filter disagrees with lci::line_is_comment_only on: "
            << lines[i];
    }
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
// name aggregation (inspect Callers/Callees rendering)
// ---------------------------------------------------------------------------

TEST(NameAggregation, DuplicatesCollapseWithCounts) {
    std::vector<std::string> names = {"fit", "predict", "fit", "fit"};
    // Discrimination: raw joining would repeat "fit" three times.
    EXPECT_EQ(format_aggregated_names(names),
              "fit x3, predict  (2 unique / 4 total)");
}

TEST(NameAggregation, TestCallersDemotedBelowProduction) {
    std::vector<std::string> names = {"test_alpha", "test_alpha", "test_alpha",
                                      "fit"};
    // test_* loses to production even at higher frequency.
    EXPECT_EQ(format_aggregated_names(names),
              "fit, test_alpha x3  (2 unique / 4 total)");
}

TEST(NameAggregation, CapAppendsRemainderAndTotals) {
    std::vector<std::string> names;
    for (int i = 0; i < 30; ++i) names.push_back("f" + std::to_string(i));
    auto out = format_aggregated_names(names, 25);
    EXPECT_NE(out.find("+5 more"), std::string::npos);
    EXPECT_NE(out.find("(30 unique / 30 total)"), std::string::npos);
}

TEST(NameAggregation, ShortUniqueListRendersPlain) {
    std::vector<std::string> names = {"alpha", "beta"};
    EXPECT_EQ(format_aggregated_names(names), "alpha, beta");
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
    // Unrecognized values fall through to Unknown.
    EXPECT_EQ(ro::parse_strategy("foo"), ro::RankStrategy::Unknown);
    EXPECT_EQ(ro::parse_strategy("bar-baz"), ro::RankStrategy::Unknown);
}

TEST(RankOptionsParseStrategy, GoParityAliases) {
    // Go-parity aliases: proximity / similarity map to Relevance (closest
    // existing behavior). See src/cli/rank_options.h _rationale comment.
    EXPECT_EQ(ro::parse_strategy("proximity"), ro::RankStrategy::Relevance);
    EXPECT_EQ(ro::parse_strategy("PROXIMITY"), ro::RankStrategy::Relevance);
    EXPECT_EQ(ro::parse_strategy("similarity"), ro::RankStrategy::Relevance);
    EXPECT_EQ(ro::parse_strategy("Similarity"), ro::RankStrategy::Relevance);
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
                  ("lci_rank_test_" + std::to_string(lci::portable::process_id()));
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
                   ("lci_rank_real_" + std::to_string(lci::portable::process_id()) + ".cpp");
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

// -- ast_filters classifier tests --------------------------------------------
//
// Direct unit tests for the AST-aware match-position classifiers backing
// `--comments-only`, `--code-only`, and `--strings-only`. These exercise the
// pure heuristic without going through the server. Columns are 0-based byte
// offsets (the one column contract, include/lci/cli/column.h); -1 means
// "position not recorded".

namespace af = ::lci::cli::ast_filters;

TEST(AstFiltersString, MatchInsideDoubleQuoted) {
    // `int x = "foo bar";` — column 11 ('o' in foo) is inside the literal.
    std::string line = "int x = \"foo bar\";";
    EXPECT_TRUE(af::match_is_in_string_literal(line, 11));
}

TEST(AstFiltersString, MatchOutsideDoubleQuoted) {
    // Column 0 ('i' of int) is in code, not a string.
    std::string line = "int x = \"foo\";";
    EXPECT_FALSE(af::match_is_in_string_literal(line, 0));
}

TEST(AstFiltersString, MatchInsideSingleQuoted) {
    // `char c = 'X';` — column 10 (the 'X') is inside the literal.
    std::string line = "char c = 'X';";
    EXPECT_TRUE(af::match_is_in_string_literal(line, 10));
}

TEST(AstFiltersString, EscapedQuoteDoesNotCloseLiteral) {
    // `"a\"b"` — backslash-escaped quote keeps us in-string for 'b'.
    std::string line = "x = \"a\\\"b\";";
    // 0-based: x=0 space=1 ==2 space=3 "=4 a=5 \=6 "=7 b=8 "=9 ;=10
    // So 'b' is at column 8.
    EXPECT_TRUE(af::match_is_in_string_literal(line, 8));
}

TEST(AstFiltersString, MatchInComment) {
    // `// hello world` — column 4 ('h' of hello) is in a comment, NOT a string.
    std::string line = "// hello world";
    EXPECT_FALSE(af::match_is_in_string_literal(line, 4));
}

TEST(AstFiltersString, QuoteInsideCommentDoesNotOpenString) {
    // `// "hello"` — the quotes are inside a comment, no string is opened.
    // The 'h' is at column 4 (still inside the comment).
    std::string line = "// \"hello\"";
    EXPECT_FALSE(af::match_is_in_string_literal(line, 4));
}

TEST(AstFiltersString, ColumnZeroIsARealPosition) {
    // 0-based contract: column 0 is the first byte of the line, a REAL
    // position — here 'x', which is code, not a string.
    std::string line = "x = \"foo\";";
    EXPECT_FALSE(af::match_is_in_string_literal(line, 0));
}

TEST(AstFiltersString, NegativeColumnReturnsFalse) {
    // kColumnUnknown (-1): position not recorded — report false, caller
    // falls back to line-level heuristics.
    EXPECT_FALSE(af::match_is_in_string_literal("anything", -1));
}

TEST(AstFiltersString, ColumnPastEndReturnsFalse) {
    std::string line = "abc";
    EXPECT_FALSE(af::match_is_in_string_literal(line, 100));
}

TEST(AstFiltersString, TripleDoubleQuoteInside) {
    // Python triple-quoted string: `x = """body"""`. Column 8 ('o' of body)
    // is inside the literal.
    std::string line = "x = \"\"\"body\"\"\"";
    // x(0) space(1) =(2) space(3) "(4) "(5) "(6) b(7) o(8) d(9) y(10) ...
    EXPECT_TRUE(af::match_is_in_string_literal(line, 8));
}

TEST(AstFiltersString, UnclosedLiteralRunsThroughEol) {
    // Unclosed `"...` — match anywhere after the opener should report
    // in-string (we treat the literal as continuing through EOL).
    std::string line = "x = \"unterminated";
    // 'u' of "unterminated" sits right after the opening quote at column 4.
    EXPECT_TRUE(af::match_is_in_string_literal(line, 5));
}

TEST(AstFiltersString, BlockCommentSameLineNotString) {
    // `/* hello */` — `hello` is in a comment, not a string.
    std::string line = "x = /* hello */ 42;";
    // 'h' position: x(0) space(1) =(2) space(3) /(4) *(5) space(6) h(7)
    EXPECT_FALSE(af::match_is_in_string_literal(line, 7));
}

TEST(AstFiltersComment, LineLeadingSlashSlash) {
    // `// comment` — every column is in a comment, including leading ws.
    EXPECT_TRUE(af::match_is_in_comment("// hello", 0));
    EXPECT_TRUE(af::match_is_in_comment("// hello", 4));
    EXPECT_TRUE(af::match_is_in_comment("    // indented", 3));
    EXPECT_TRUE(af::match_is_in_comment("    // indented", 8));
}

TEST(AstFiltersComment, LineLeadingHash) {
    EXPECT_TRUE(af::match_is_in_comment("# python comment", 0));
    EXPECT_TRUE(af::match_is_in_comment("# python comment", 9));
}

TEST(AstFiltersComment, InlineSlashSlashSplitsLine) {
    // `int x = 1; // tail` — column on the code part is NOT a comment, but
    // a column at or past the `//` opener IS.
    std::string line = "int x = 1; // tail";
    // i(0) n(1) t(2) space(3) x(4) space(5) =(6) space(7) 1(8) ;(9)
    // space(10) /(11) /(12) space(13) t(14)
    EXPECT_FALSE(af::match_is_in_comment(line, 4));   // 'x' in code
    EXPECT_TRUE(af::match_is_in_comment(line, 11));   // first '/' of `//`
    EXPECT_TRUE(af::match_is_in_comment(line, 14));   // 't' of tail
}

TEST(AstFiltersComment, SlashSlashInsideStringNotComment) {
    // `s = "https://example.com";` — the `//` is inside a string and must
    // NOT mark the rest of the line as comment. Match on `e` of `example`
    // is in a string, not a comment.
    std::string line = "s = \"https://example.com\";";
    // s(0) space(1) =(2) space(3) "(4) h(5) t(6) t(7) p(8) s(9)
    // :(10) /(11) /(12) e(13)
    EXPECT_FALSE(af::match_is_in_comment(line, 13));
}

TEST(AstFiltersComment, BlockCommentCloserOnLine) {
    // `body */ rest` — columns up to and including the `/` of `*/` are
    // comment, columns after are code.
    std::string line = "body */ rest";
    // b(0) o(1) d(2) y(3) space(4) *(5) /(6) space(7) r(8) e(9) s(10) t(11)
    EXPECT_TRUE(af::match_is_in_comment(line, 0));   // 'b' inside comment tail
    EXPECT_TRUE(af::match_is_in_comment(line, 6));   // closing '/'
    EXPECT_FALSE(af::match_is_in_comment(line, 8));  // 'r' of rest -> code
}

TEST(AstFiltersComment, BlockCommentOpenerWithoutCloser) {
    // `code /* tail` — every column from the `/*` opener is comment.
    std::string line = "x = 1 /* tail";
    // x(0) space(1) =(2) space(3) 1(4) space(5) /(6) *(7) space(8) t(9)
    EXPECT_FALSE(af::match_is_in_comment(line, 0));
    EXPECT_TRUE(af::match_is_in_comment(line, 6));
    EXPECT_TRUE(af::match_is_in_comment(line, 9));
}

TEST(AstFiltersComment, BlockCommentSameLineWithCode) {
    // `int x = /* note */ 42;` — `note` columns are comment, `42` is code.
    std::string line = "int x = /* note */ 42;";
    // ...space(7) /(8) *(9) space(10) n(11) o(12) t(13) e(14) space(15)
    // *(16) /(17) space(18) 4(19)
    EXPECT_TRUE(af::match_is_in_comment(line, 11));   // 'n' of note
    EXPECT_FALSE(af::match_is_in_comment(line, 19));  // '4' of 42
}

TEST(AstFiltersComment, UnknownColumnFallsBackToLineHeuristic) {
    // kColumnUnknown (-1) -> line-level classification: a leading `//` line
    // reports comment, a code line reports not-comment.
    EXPECT_TRUE(af::match_is_in_comment("// only", -1));
    EXPECT_FALSE(af::match_is_in_comment("int x = 1;", -1));
}

TEST(AstFiltersComment, EmptyOrWhitespaceLine) {
    EXPECT_FALSE(af::match_is_in_comment("", 0));
    EXPECT_FALSE(af::match_is_in_comment("   ", 0));
}

// -- ast_filters JSON transform tests ----------------------------------------

TEST(AstFiltersApplyCommentsOnly, KeepsOnlyCommentRows) {
    nlohmann::json comment_row;
    comment_row["path"] = "/no/such/file.cpp";
    comment_row["line"] = 1;
    comment_row["column"] = 0;
    comment_row["context"] = {{"start_line", 1}, {"lines", {"// in a comment"}}};

    nlohmann::json code_row;
    code_row["path"] = "/no/such/file.cpp";
    code_row["line"] = 2;
    code_row["column"] = 0;
    code_row["context"] = {{"start_line", 2}, {"lines", {"int x = 1;"}}};

    nlohmann::json string_row;
    string_row["path"] = "/no/such/file.cpp";
    string_row["line"] = 3;
    string_row["column"] = 10;  // closing quote of the string literal
    string_row["context"] = {{"start_line", 3}, {"lines", {"x = \"hello\";"}}};

    nlohmann::json results =
        nlohmann::json::array({comment_row, code_row, string_row});
    auto kept = af::apply_comments_only(results);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0]["line"].get<int>(), 1);
}

TEST(AstFiltersApplyStringsOnly, KeepsOnlyStringRows) {
    nlohmann::json comment_row;
    comment_row["path"] = "/no/such/file.cpp";
    comment_row["line"] = 1;
    comment_row["column"] = 4;
    comment_row["context"] = {{"start_line", 1}, {"lines", {"// hello"}}};

    nlohmann::json code_row;
    code_row["path"] = "/no/such/file.cpp";
    code_row["line"] = 2;
    code_row["column"] = 0;
    code_row["context"] = {{"start_line", 2}, {"lines", {"int x = 1;"}}};

    // `x = "hello";` — 'h' at column 5.
    nlohmann::json string_row;
    string_row["path"] = "/no/such/file.cpp";
    string_row["line"] = 3;
    string_row["column"] = 5;
    string_row["context"] = {{"start_line", 3}, {"lines", {"x = \"hello\";"}}};

    nlohmann::json results =
        nlohmann::json::array({comment_row, code_row, string_row});
    auto kept = af::apply_strings_only(results);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0]["line"].get<int>(), 3);
}

TEST(AstFiltersApplyCodeOnly, DropsCommentAndStringRows) {
    nlohmann::json comment_row;
    comment_row["path"] = "/no/such/file.cpp";
    comment_row["line"] = 1;
    comment_row["column"] = 4;
    comment_row["context"] = {{"start_line", 1}, {"lines", {"// hello"}}};

    nlohmann::json code_row;
    code_row["path"] = "/no/such/file.cpp";
    code_row["line"] = 2;
    code_row["column"] = 4;  // 'x' in `int x = 1;`
    code_row["context"] = {{"start_line", 2}, {"lines", {"int x = 1;"}}};

    nlohmann::json string_row;
    string_row["path"] = "/no/such/file.cpp";
    string_row["line"] = 3;
    string_row["column"] = 5;  // 'h' inside `x = "hello";`
    string_row["context"] = {{"start_line", 3}, {"lines", {"x = \"hello\";"}}};

    nlohmann::json results =
        nlohmann::json::array({comment_row, code_row, string_row});
    auto kept = af::apply_code_only(results);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0]["line"].get<int>(), 2);
}

TEST(AstFiltersApply, EmptyInputProducesEmpty) {
    EXPECT_EQ(af::apply_comments_only(nlohmann::json::array()).size(), 0u);
    EXPECT_EQ(af::apply_strings_only(nlohmann::json::array()).size(), 0u);
    EXPECT_EQ(af::apply_code_only(nlohmann::json::array()).size(), 0u);
}

TEST(AstFiltersApply, MissingMetadataPassesThrough) {
    // Rows without path or line should be passed through (graceful
    // degradation — never silently drop on malformed input).
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"score", 1.0}});  // no path/line
    auto out = af::apply_comments_only(results);
    EXPECT_EQ(out.size(), 1u);
    out = af::apply_strings_only(results);
    EXPECT_EQ(out.size(), 1u);
    out = af::apply_code_only(results);
    EXPECT_EQ(out.size(), 1u);
}

TEST(AstFiltersApply, PreservesInputOrdering) {
    // Stable order: rows that survive the filter should appear in the
    // same relative order as the input. Each row's embedded context
    // `start_line` aligns with the row's `line` so `read_match_line`
    // resolves the matched line text from the context block (no disk
    // read).
    auto make_row = [](int line_no) {
        nlohmann::json r;
        r["path"] = "/no/such/file.cpp";
        r["line"] = line_no;
        r["column"] = 0;
        r["context"] = {{"start_line", line_no}, {"lines", {"// kept"}}};
        return r;
    };
    nlohmann::json results =
        nlohmann::json::array({make_row(10), make_row(20), make_row(30)});
    auto kept = af::apply_comments_only(results);
    ASSERT_EQ(kept.size(), 3u);
    EXPECT_EQ(kept[0]["line"].get<int>(), 10);
    EXPECT_EQ(kept[1]["line"].get<int>(), 20);
    EXPECT_EQ(kept[2]["line"].get<int>(), 30);
}

// -- split_literal_alternation (bare `a|b` OR queries) ------------------------
//
// Reviewers typed `lci search "FileWatcher|DebouncedRebuilder"` (no --regex)
// and got a silent zero: the literal engine searched for the pipe byte
// verbatim. A top-level alternation of PURE literals is unambiguous — treat
// it as a multi-pattern OR through the existing literal fast path. Anything
// carrying real regex syntax stays out (that is --regex territory).

TEST(SplitLiteralAlternation, TwoNames) {
    auto terms =
        grep_filters::split_literal_alternation("FileWatcher|DebouncedRebuilder");
    ASSERT_EQ(terms.size(), 2u);
    EXPECT_EQ(terms[0], "FileWatcher");
    EXPECT_EQ(terms[1], "DebouncedRebuilder");
}

TEST(SplitLiteralAlternation, ThreeNamesWithSpaces) {
    auto terms = grep_filters::split_literal_alternation("foo bar|baz|qux");
    ASSERT_EQ(terms.size(), 3u);
    EXPECT_EQ(terms[0], "foo bar");
}

TEST(SplitLiteralAlternation, SingleTermIsNotAlternation) {
    EXPECT_TRUE(grep_filters::split_literal_alternation("FileWatcher").empty());
}

TEST(SplitLiteralAlternation, EmptyBranchDisqualifies) {
    EXPECT_TRUE(grep_filters::split_literal_alternation("foo|").empty());
    EXPECT_TRUE(grep_filters::split_literal_alternation("|foo").empty());
    EXPECT_TRUE(grep_filters::split_literal_alternation("a||b").empty());
}

TEST(SplitLiteralAlternation, RegexMetaInBranchDisqualifies) {
    // These need --regex; silently splitting them would change semantics.
    EXPECT_TRUE(grep_filters::split_literal_alternation("\\d+|foo").empty());
    EXPECT_TRUE(grep_filters::split_literal_alternation("foo.*|bar").empty());
    EXPECT_TRUE(grep_filters::split_literal_alternation("(foo)|bar").empty());
    EXPECT_TRUE(grep_filters::split_literal_alternation("foo[ab]|bar").empty());
    EXPECT_TRUE(grep_filters::split_literal_alternation("^foo|bar").empty());
    EXPECT_TRUE(grep_filters::split_literal_alternation("foo|bar$").empty());
}

TEST(SplitLiteralAlternation, PunctuationLiteralsStayLiteral) {
    // '.'/'('/etc. are regex meta — a branch carrying them is ambiguous, so
    // the split declines and the pattern flows through the plain literal path
    // (which already handles punctuation bytes).
    EXPECT_TRUE(grep_filters::split_literal_alternation(".dump(|catch").empty());
}

// -- group_rows_by_file (per-file grouped output, --group) --------------------
//
// Reviewer need: "which files mention any of these N names, grouped per file
// with counts". Groups server result rows by path (first-appearance order),
// with total count, sorted deduped line list, and per-term counts.

namespace {

nlohmann::json make_group_row(const std::string& path, int line,
                              const std::string& match) {
    return nlohmann::json{{"path", path}, {"line", line}, {"match", match}};
}

}  // namespace

TEST(GroupRowsByFile, GroupsAndCountsPerTerm) {
    nlohmann::json rows = nlohmann::json::array();
    rows.push_back(make_group_row("src/a.cpp", 3, "FileWatcher"));
    rows.push_back(make_group_row("src/b.cpp", 1, "DebouncedRebuilder"));
    rows.push_back(make_group_row("src/a.cpp", 9, "FileWatcher"));

    auto groups = grep_filters::group_rows_by_file(
        rows, {"FileWatcher", "DebouncedRebuilder"}, false);
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[0]["path"], "src/a.cpp");
    EXPECT_EQ(groups[0]["count"].get<int>(), 2);
    ASSERT_EQ(groups[0]["lines"].size(), 2u);
    EXPECT_EQ(groups[0]["lines"][0].get<int>(), 3);
    EXPECT_EQ(groups[0]["lines"][1].get<int>(), 9);
    EXPECT_EQ(groups[0]["terms"]["FileWatcher"].get<int>(), 2);
    EXPECT_FALSE(groups[0]["terms"].contains("DebouncedRebuilder"));
    EXPECT_EQ(groups[1]["path"], "src/b.cpp");
    EXPECT_EQ(groups[1]["terms"]["DebouncedRebuilder"].get<int>(), 1);
}

TEST(GroupRowsByFile, CaseInsensitiveTermAttribution) {
    nlohmann::json rows = nlohmann::json::array();
    rows.push_back(make_group_row("a.go", 5, "filewatcher"));
    auto groups =
        grep_filters::group_rows_by_file(rows, {"FileWatcher"}, true);
    ASSERT_EQ(groups.size(), 1u);
    // Attribution folds case; the reported key is the QUERY term so callers
    // can join counts back to what they asked for.
    EXPECT_EQ(groups[0]["terms"]["FileWatcher"].get<int>(), 1);
}

TEST(GroupRowsByFile, DuplicateLinesDedupedInLineList) {
    nlohmann::json rows = nlohmann::json::array();
    rows.push_back(make_group_row("a.go", 7, "x"));
    rows.push_back(make_group_row("a.go", 7, "x"));
    auto groups = grep_filters::group_rows_by_file(rows, {"x"}, false);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0]["count"].get<int>(), 2);
    ASSERT_EQ(groups[0]["lines"].size(), 1u);
}

TEST(GroupRowsByFile, EmptyInput) {
    auto groups = grep_filters::group_rows_by_file(nlohmann::json::array(),
                                                   {"x"}, false);
    EXPECT_TRUE(groups.empty());
}

// -- S12 RED pins --------------------------------------------------------------
//
// Each test below pins a defect called out in task S12 and FAILS on the
// pre-fix tree. They are grouped by the GREEN commit that fixes them.

// -- no cwd auto-index: running a server-backed command from a directory with
// no .lci.kdl and no git root must fail naming `lci init` instead of spawning
// a server that silently indexes the whole cwd.

TEST(CliConfigGuardTest, CwdWithoutConfigOrGitRootFailsNamingLciInit) {
    namespace fs = std::filesystem;
    auto dir = lci::test::unique_temp_dir("lci_cli_no_config_");
    fs::create_directories(dir);
    // Neither .lci.kdl nor .git here, and the system temp dir is not inside
    // a git checkout.

    const fs::path old_cwd = fs::current_path();
    fs::current_path(dir);
    GlobalFlags flags;  // no --root, no -c: the cwd auto-pick path
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    fs::current_path(old_cwd);

    EXPECT_FALSE(err.empty()) << "cwd without .lci.kdl/git root must not "
                                 "auto-index; got a valid config";
    EXPECT_NE(err.find("lci init"), std::string::npos) << err;

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// -- server spawn argv carries the global flags -------------------------------
//
// The auto-spawned server used to receive only --root: -c/--config,
// --include and --exclude were dropped, so every server-backed command
// silently ran against an index built with DIFFERENT filters than the
// invocation asked for.

namespace {

/// Offset of `flag`'s VALUE in argv, or npos when the flag is absent.
size_t flag_value_pos(const std::vector<std::string>& argv,
                      const std::string& flag) {
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == flag) return i + 1;
    }
    return std::string::npos;
}

}  // namespace

TEST(CliServerSpawnTest, SpawnArgvCarriesConfigIncludeExcludeFlags) {
    GlobalFlags flags;
    flags.config_path = "custom.kdl";
    flags.include = {"src/**"};
    flags.exclude = {"x/**"};
    Config cfg;
    cfg.project.root = "/repo";

    const auto argv = build_server_spawn_argv("/bin/lci", cfg, flags);

    ASSERT_EQ(argv.front(), "/bin/lci");
    EXPECT_EQ(argv.back(), "server");

    const size_t root_pos = flag_value_pos(argv, "--root");
    ASSERT_NE(root_pos, std::string::npos);
    EXPECT_EQ(argv[root_pos], "/repo");

    // -c is made absolute (against the cwd) so the spawned server reads the
    // file the user named regardless of its own working directory.
    const size_t config_pos = flag_value_pos(argv, "-c");
    ASSERT_NE(config_pos, std::string::npos) << "-c was dropped";
    EXPECT_TRUE(std::filesystem::path(argv[config_pos]).is_absolute())
        << argv[config_pos];
    EXPECT_EQ(std::filesystem::path(argv[config_pos]).filename(),
              "custom.kdl");

    const size_t inc_pos = flag_value_pos(argv, "--include");
    ASSERT_NE(inc_pos, std::string::npos) << "--include was dropped";
    EXPECT_EQ(argv[inc_pos], "src/**");

    const size_t exc_pos = flag_value_pos(argv, "--exclude");
    ASSERT_NE(exc_pos, std::string::npos) << "--exclude was dropped";
    EXPECT_EQ(argv[exc_pos], "x/**");
}

TEST(CliServerSpawnTest, SpawnArgvWithoutFlagsCarriesOnlyRoot) {
    GlobalFlags flags;
    Config cfg;
    cfg.project.root = "/repo";
    const auto argv = build_server_spawn_argv("/bin/lci", cfg, flags);
    EXPECT_EQ(flag_value_pos(argv, "-c"), std::string::npos);
    EXPECT_EQ(flag_value_pos(argv, "--include"), std::string::npos);
    EXPECT_EQ(flag_value_pos(argv, "--exclude"), std::string::npos);
}

// -- stale-server socket unlink is inode-guarded ------------------------------
//
// After a stale-build server is told to exit, a successor can bind the same
// socket path inside the 250ms exit-poll window. Unlinking unconditionally
// removed the SUCCESSOR's socket (alive, unreachable, holding the start
// lock). The unlink must fire only when the path's inode still matches the
// one observed before the kill.

#ifndef _WIN32
TEST(CliServerSpawnTest, SocketUnlinkOnlyWhenInodeMatches) {
    namespace fs = std::filesystem;
    auto dir = lci::test::unique_temp_dir("lci_cli_sock_inode_");
    fs::create_directories(dir);
    const fs::path sock = dir / "srv.sock";

    // Same inode observed before the kill -> unlinked.
    {
        std::ofstream f(sock);
        f << "x";
    }
    const auto id = socket_file_identity(sock.string());
    ASSERT_TRUE(id.valid);
    // Prepare the "successor" bind BEFORE unlinking the stale one (a fresh
    // create after unlink can recycle the just-freed inode, which is exactly
    // the aliasing the guard exists to distrust): distinct live files are
    // guaranteed distinct inodes.
    const fs::path successor = dir / "srv.sock.successor";
    {
        std::ofstream f(successor);
        f << "y";
    }
    EXPECT_TRUE(unlink_socket_if_identity(sock.string(), id));
    EXPECT_FALSE(fs::exists(sock));

    // Rebound path (the successor's inode) + pre-kill identity -> kept.
    fs::rename(successor, sock);
    const auto rebound = socket_file_identity(sock.string());
    ASSERT_TRUE(rebound.valid);
    ASSERT_NE(rebound.ino, id.ino);
    EXPECT_FALSE(unlink_socket_if_identity(sock.string(), id));
    EXPECT_TRUE(fs::exists(sock)) << "successor's socket was unlinked";
    // And the matching identity DOES unlink it.
    EXPECT_TRUE(unlink_socket_if_identity(sock.string(), rebound));

    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif  // _WIN32

// -- relative -c resolves against the cwd, not --root -------------------------
//
// `lci -r /repo -c mine.kdl` names ./mine.kdl — the directory the command was
// typed in. Resolving it against --root reads a different file (or, when the
// root happens to hold a same-named file, silently applies the wrong config).

TEST(CliConfigGuardTest, RelativeConfigResolvesAgainstCwdNotRoot) {
    namespace fs = std::filesystem;
    auto cwd_dir = lci::test::unique_temp_dir("lci_cli_relc_cwd_");
    auto root_dir = lci::test::unique_temp_dir("lci_cli_relc_root_");
    fs::create_directories(cwd_dir);
    fs::create_directories(root_dir);

    {
        std::ofstream f(cwd_dir / "custom.kdl");
        f << "search {\n  max_results 66\n}\n";
    }
    {
        // Same-named file under --root with a DIFFERENT value: reading it
        // instead of the cwd file is the silent wrong-config defect.
        std::ofstream f(root_dir / "custom.kdl");
        f << "search {\n  max_results 99\n}\n";
    }

    const fs::path old_cwd = fs::current_path();
    fs::current_path(cwd_dir);
    GlobalFlags flags;
    flags.root = root_dir.string();
    flags.config_path = "custom.kdl";
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    fs::current_path(old_cwd);

    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(cfg.search.max_results, 66)
        << "relative -c must resolve against the cwd, not --root";

    std::error_code ec;
    fs::remove_all(cwd_dir, ec);
    fs::remove_all(root_dir, ec);
}

// -- arrow patterns: `lci search '->next'` — a token starting with `->` is a
// C++ member-access pattern, not a `-` exclusion directive.

TEST(QueryParserArrow, ArrowTokenIsContentNotExclusion) {
    auto q = query_parser::parse("->next");
    EXPECT_TRUE(q.exclusions.empty());
    EXPECT_EQ(q.content_query, "->next");
}

TEST(QueryParserArrow, ArrowAfterTermStaysWhole) {
    // `x ->next`: the `->next` token must not be eaten as an exclusion of
    // `>next` — the pair is one logical query.
    auto q = query_parser::parse("x ->next");
    EXPECT_TRUE(q.exclusions.empty());
    EXPECT_EQ(q.content_query, "x ->next");
}

TEST(QueryParserArrow, PlainExclusionStillWorks) {
    auto q = query_parser::parse("auth -test");
    ASSERT_EQ(q.exclusions.size(), 1u);
    EXPECT_EQ(q.exclusions[0], "test");
    EXPECT_EQ(q.content_query, "auth");
}

// -- one column base: every CLI-facing `column` is a 0-based byte offset into
// the matched line (the base the server/search engine and the pinned goldens
// already use); -1 means "not recorded".

TEST(ColumnContract, RegexFilterResultsEmitsZeroBasedColumn) {
    // Line "ab foo": `foo` starts at byte offset 3. The -E row filter used
    // to emit 4 (1-based) while literal rows from the server carry 3
    // (0-based) — the two modes disagreed in --json output.
    nlohmann::json row;
    row["path"] = "/no/such/file";
    row["line"] = 1;
    row["column"] = 0;
    row["context"] = {{"start_line", 1}, {"lines", {"ab foo"}}};
    nlohmann::json results = nlohmann::json::array({row});

    RE2 re("foo");
    auto out = grep_filters::regex_filter_results(std::move(results), re);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]["column"].get<int>(), 3);
    EXPECT_EQ(out[0]["match"].get<std::string>(), "foo");
}

TEST(ColumnContract, WordBoundaryAfterRegexRowIsNotOffByOne) {
    // `lci search -E foo -w`: the regex filter rewrote the column to 1-based
    // and apply_word_boundary then checked the byte window one position too
    // far right, so `foo` in "ab foo" (a real word hit) was DROPPED.
    nlohmann::json row;
    row["path"] = "/no/such/file";
    row["line"] = 1;
    row["column"] = 0;
    row["context"] = {{"start_line", 1}, {"lines", {"ab foo"}}};
    nlohmann::json results = nlohmann::json::array({row});

    RE2 re("foo");
    auto filtered = grep_filters::regex_filter_results(std::move(results), re);
    auto kept =
        grep_filters::apply_word_boundary(std::move(filtered), "foo", false);
    EXPECT_EQ(kept.size(), 1u) << "word hit at a real boundary was dropped";

    // Discrimination: "abfood" has `foo` but not at a word boundary.
    nlohmann::json row2;
    row2["path"] = "/no/such/file";
    row2["line"] = 1;
    row2["column"] = 0;
    row2["context"] = {{"start_line", 1}, {"lines", {"abfood"}}};
    RE2 re2("foo");
    auto filtered2 = grep_filters::regex_filter_results(
        nlohmann::json::array({row2}), re2);
    auto kept2 = grep_filters::apply_word_boundary(std::move(filtered2),
                                                   "foo", false);
    EXPECT_TRUE(kept2.empty());
}

TEST(ColumnContract, AstCommentClassifierIsZeroBased) {
    // "int y; // c": the `//` opener is at byte offset 7 (0-based). The
    // classifier used to treat the column as 1-based and inspected offset 6.
    EXPECT_TRUE(af::match_is_in_comment("int y; // c", 7));
    EXPECT_FALSE(af::match_is_in_comment("int y; // c", 4));  // 'y'
}

TEST(ColumnContract, AstStringClassifierIsZeroBased) {
    // `x = "foo";`: byte offset 9 is the `;` — code, not string. A 1-based
    // reading inspects offset 8 (the closing quote) and misclassifies.
    EXPECT_FALSE(af::match_is_in_string_literal("x = \"foo\";", 9));
    EXPECT_TRUE(af::match_is_in_string_literal("x = \"foo\";", 5));  // 'f'
}

TEST(ColumnContract, UnknownColumnSentinelIsMinusOne) {
    // -1 means "position not recorded"; 0 is a real (first-byte) position.
    EXPECT_FALSE(af::match_is_in_string_literal("x = \"foo\";", -1));
    // Column 0 on a comment line: first byte of `// x` IS in a comment.
    EXPECT_TRUE(af::match_is_in_comment("// x", 0));
}

// -- S12.4: meta-regex full-scan filters, flag effects, result paging --------
//
// These tests drive the built `lci` binary against a temp corpus: the
// defects they pin live in run_search's plumbing (the pure-meta regex
// full-scan path, silently-ignored flags, the pre-filter 500-row cap), which
// pure-unit tests over the filter helpers cannot observe.

// Writes `content` to `root/rel`, creating parent directories.
void write_corpus_file(const std::filesystem::path& root,
                       const std::string& rel, const std::string& content) {
    const auto p = root / rel;
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << content;
}

// Runs `lci <args...> -r <root>`, capturing stdout. Returns run_capture's
// spawn/exit-status bool; `out` receives stdout.
bool run_lci_search(const std::filesystem::path& lci_bin,
                    const std::filesystem::path& root,
                    std::vector<std::string> args, std::string& out) {
    args.push_back("-r");
    args.push_back(root.string());
    return subprocess::run_capture(args, "", out);
}

void shutdown_lci_server(const std::filesystem::path& lci_bin,
                         const std::filesystem::path& root) {
    std::string ignored;
    subprocess::run_capture(
        {lci_bin.string(), "shutdown", "-r", root.string()}, "", ignored);
}

TEST(SearchMetaRegexTest, TextModePrintsTextNotJson) {
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;
    const auto root = lci::test::unique_temp_dir("lci_meta_text_");
    fs::create_directories(root);
    write_corpus_file(root, "alpha.cpp", "12345\n");

    std::string out;
    ASSERT_TRUE(run_lci_search(
        lci_bin, root,
        {lci_bin.string(), "search", "-E", "^\\d{5}$"}, out));
    // Text mode must render the text listing, not a raw JSON envelope.
    EXPECT_EQ(out.find("\"results\""), std::string::npos) << out;
    EXPECT_NE(out.find("alpha.cpp"), std::string::npos) << out;

    shutdown_lci_server(lci_bin, root);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(SearchMetaRegexTest, HonorsPathScopeAndExclude) {
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;
    const auto root = lci::test::unique_temp_dir("lci_meta_scope_");
    fs::create_directories(root);
    write_corpus_file(root, "keep/alpha.cpp", "12345\n");
    write_corpus_file(root, "drop/beta.cpp", "67890\n");

    // Path scope: only keep/ is searched.
    std::string out;
    ASSERT_TRUE(run_lci_search(
        lci_bin, root,
        {lci_bin.string(), "search", "-E", "^\\d{5}$",
         (root / "keep").string()},
        out));
    EXPECT_NE(out.find("alpha.cpp"), std::string::npos) << out;
    EXPECT_EQ(out.find("beta.cpp"), std::string::npos) << out;

    // --exclude drops the drop/ file.
    out.clear();
    ASSERT_TRUE(run_lci_search(
        lci_bin, root,
        {lci_bin.string(), "search", "-E", "^\\d{5}$", "--exclude", "drop"},
        out));
    EXPECT_NE(out.find("alpha.cpp"), std::string::npos) << out;
    EXPECT_EQ(out.find("beta.cpp"), std::string::npos) << out;

    shutdown_lci_server(lci_bin, root);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(SearchMetaRegexTest, HonorsMaxCount) {
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;
    const auto root = lci::test::unique_temp_dir("lci_meta_maxc_");
    fs::create_directories(root);
    write_corpus_file(root, "gamma.cpp", "11111\n22222\n33333\n");

    std::string out;
    ASSERT_TRUE(run_lci_search(
        lci_bin, root,
        {lci_bin.string(), "search", "-E", "^\\d{5}$", "--max-count", "1"},
        out));
    EXPECT_NE(out.find("Found 1 results"), std::string::npos) << out;

    shutdown_lci_server(lci_bin, root);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// Every flag that parses must change the output. Corpus: `token` appears
// twice in alpha.cpp (plus one plain line) and once in beta.cpp (plus one
// plain line), so each flag has an observable effect.
class SearchFlagEffectTest : public ::testing::Test {
  protected:
    std::filesystem::path lci_bin;
    std::filesystem::path root;

    void SetUp() override {
        lci_bin = portable::executable_path().parent_path().parent_path() /
                  "src" / "lci";
        ASSERT_TRUE(std::filesystem::exists(lci_bin)) << lci_bin;
        root = lci::test::unique_temp_dir("lci_search_flags_");
        std::filesystem::create_directories(root);
        write_corpus_file(root, "alpha.cpp",
                          "int token_alpha_one;\n"
                          "int token_alpha_two;\n"
                          "plain alpha line\n");
        write_corpus_file(root, "beta.cpp",
                          "int token_beta_one;\n"
                          "plain beta line\n");
    }

    void TearDown() override {
        shutdown_lci_server(lci_bin, root);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    nlohmann::json search_json(std::vector<std::string> extra) {
        std::vector<std::string> args = {lci_bin.string(), "search", "token",
                                         "--json"};
        args.insert(args.end(), extra.begin(), extra.end());
        std::string out;
        EXPECT_TRUE(run_lci_search(lci_bin, root, args, out));
        return nlohmann::json::parse(out);
    }
};

TEST_F(SearchFlagEffectTest, IncludeFiltersResults) {
    auto j = search_json({"--include", "alpha"});
    ASSERT_TRUE(j.contains("results"));
    EXPECT_EQ(j.value("count", -1), 2) << j.dump();
    for (const auto& row : j["results"]) {
        const std::string path = row["result"].value("path", "");
        EXPECT_NE(path.find("alpha.cpp"), std::string::npos) << path;
    }
}

TEST_F(SearchFlagEffectTest, InvertMatchChangesOutput) {
    auto j = search_json({"--invert-match"});
    EXPECT_EQ(j.value("mode", ""), "invert-match") << j.dump();
    bool saw_plain = false;
    for (const auto& row : j["results"]) {
        if (row.value("match", "").find("plain") != std::string::npos) {
            saw_plain = true;
        }
    }
    EXPECT_TRUE(saw_plain) << j.dump();
}

TEST_F(SearchFlagEffectTest, CountChangesOutput) {
    auto j = search_json({"--count"});
    EXPECT_EQ(j.value("mode", ""), "count") << j.dump();
    int total = 0;
    for (const auto& row : j["results"]) {
        ASSERT_TRUE(row.contains("count")) << row.dump();
        total += row.value("count", 0);
    }
    EXPECT_EQ(total, 3) << j.dump();
}

TEST_F(SearchFlagEffectTest, FilesWithMatchesChangesOutput) {
    auto j = search_json({"-l"});
    EXPECT_EQ(j.value("mode", ""), "files-with-matches") << j.dump();
    EXPECT_EQ(j["results"].size(), 2u) << j.dump();
    for (const auto& row : j["results"]) {
        EXPECT_TRUE(row.contains("path")) << row.dump();
        EXPECT_FALSE(row.contains("line")) << row.dump();
    }
}

TEST_F(SearchFlagEffectTest, MaxCountCapsPerFile) {
    auto baseline = search_json({});
    EXPECT_EQ(baseline.value("count", -1), 3) << baseline.dump();
    auto j = search_json({"--max-count", "1"});
    EXPECT_EQ(j.value("count", -1), 2) << j.dump();  // one per file
}

TEST(SearchPagingTest, MatchBeyondRow500IsReturned) {
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;
    const auto root = lci::test::unique_temp_dir("lci_search_paging_");
    fs::create_directories(root);

    // 600 filler hits push the unique-tail hit to row 601 — past the old
    // hard 500-row client cap.
    std::string content;
    for (int i = 0; i < 600; ++i) {
        content += "needle filler line " + std::to_string(i) + "\n";
    }
    content += "needle_unique_tail\n";
    write_corpus_file(root, "big.cpp", content);

    std::string out;
    ASSERT_TRUE(run_lci_search(
        lci_bin, root,
        {lci_bin.string(), "search", "needle", "--json"}, out));
    auto j = nlohmann::json::parse(out);
    EXPECT_EQ(j.value("count", -1), 601) << out.substr(0, 400);
    EXPECT_NE(out.find("needle_unique_tail"), std::string::npos);

    shutdown_lci_server(lci_bin, root);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// -- S12.6: alternation branch without a literal seed --------------------------
//
// `lci search -E 'foobar|ab'`: the "ab" branch has no >=3-char literal, so
// trigram seed extraction yields {"foobar"} only. Pre-fix, lines matching
// only the "ab" branch were silently absent from -E results with no
// diagnostic (certified-absence class: wrong answer, not slow). The fix
// must either fail loudly or fall back to a full scan for the pattern.

TEST(AlternationSeedTest, UnseededBranchMatchesRgSemantics) {
    // rg is NOT installed on this host, so the expected line set is pinned
    // from rg's documented semantics: `rg -e 'foobar|ab'` reports every
    // line matching either alternative — one row per matching line, so
    // both alpha.cpp:1 (foobar branch) and beta.cpp:1 (ab branch only).
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;
    const auto root = lci::test::unique_temp_dir("lci_alt_seed_");
    fs::create_directories(root);
    write_corpus_file(root, "alpha.cpp", "int foobar_alpha = 1;\n");
    write_corpus_file(root, "beta.cpp",
                      "ab short branch hit\n"
                      "plain line\n");

    std::string out;
    ASSERT_TRUE(run_lci_search(
        lci_bin, root,
        {lci_bin.string(), "search", "-E", "foobar|ab", "--json"}, out));
    auto j = nlohmann::json::parse(out);
    ASSERT_TRUE(j.contains("results")) << out;

    std::set<std::pair<std::string, int>> hits;
    for (const auto& row : j["results"]) {
        const auto& r = row.contains("result") ? row["result"] : row;
        // Rows carry cwd-relative paths; compare on the filename so the
        // pinned set reads as the rg-equivalent (file, line) pairs.
        hits.emplace(
            std::filesystem::path(r.value("path", "")).filename().string(),
            r.value("line", 0));
    }
    const std::set<std::pair<std::string, int>> expected = {
        {"alpha.cpp", 1}, {"beta.cpp", 1}};
    EXPECT_EQ(hits, expected)
        << "the 'ab' branch has no trigram seed; its match must still be "
           "reported (full-scan fallback), not silently dropped\n"
        << out;

    shutdown_lci_server(lci_bin, root);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// -- S12.5: real JSON output, honest --stats, guarded lookups, paged symbols --

TEST(CommandsJsonTest, RefsJsonPrintsValidJsonAndExitsZero) {
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;
    const auto root = lci::test::unique_temp_dir("lci_refs_json_");
    fs::create_directories(root);
    write_corpus_file(root, "helper.cpp", "int helper_fn() { return 7; }\n");
    write_corpus_file(root, "user.cpp",
                      "int use_it() { return helper_fn(); }\n");

    std::string out;
    EXPECT_TRUE(run_lci_search(
        lci_bin, root,
        {lci_bin.string(), "refs", "helper_fn", "--json"}, out))
        << "refs --json must exit 0";
    nlohmann::json j;
    EXPECT_NO_THROW(j = nlohmann::json::parse(out)) << out;
    ASSERT_TRUE(j.is_object()) << out;
    ASSERT_TRUE(j.contains("references") && j["references"].is_array())
        << out;
    EXPECT_FALSE(j["references"].empty()) << out;

    shutdown_lci_server(lci_bin, root);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(CommandsJsonTest, BrowseStatsReportsStats) {
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;
    const auto root = lci::test::unique_temp_dir("lci_browse_stats_");
    fs::create_directories(root);
    write_corpus_file(root, "alpha.cpp",
                      "int alpha_one() { return 1; }\n"
                      "int alpha_two() { return 2; }\n");

    std::string out;
    ASSERT_TRUE(run_lci_search(
        lci_bin, root,
        {lci_bin.string(), "browse", "alpha.cpp", "--stats"}, out));
    EXPECT_NE(out.find("Stats:"), std::string::npos) << out;

    shutdown_lci_server(lci_bin, root);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(CommandsJsonTest, MissingDefinitionsKeyIsClearErrorNotException) {
    // A callers report missing "definitions" must be reported as a clear
    // error, not crash with a nlohmann::json exception.
    const nlohmann::json report = {{"total_callers", 0},
                                   {"total_call_sites", 0},
                                   {"callers", nlohmann::json::array()}};
    std::string error;
    EXPECT_FALSE(callers_report_valid(report, error));
    EXPECT_NE(error.find("definitions"), std::string::npos) << error;

    const nlohmann::json ok = {{"definitions", nlohmann::json::array()},
                               {"total_callers", 0},
                               {"total_call_sites", 0},
                               {"callers", nlohmann::json::array()}};
    error.clear();
    EXPECT_TRUE(callers_report_valid(ok, error)) << error;
}

TEST(SymbolsPagingTest, FileGlobReturnsAllMatchesBeyondFirstServerPage) {
    namespace fs = std::filesystem;
    const auto lci_bin =
        portable::executable_path().parent_path().parent_path() / "src" /
        "lci";
    ASSERT_TRUE(fs::exists(lci_bin)) << lci_bin;
    const auto root = lci::test::unique_temp_dir("lci_symbols_paging_");
    fs::create_directories(root);

    // 2000 symbols in one .ts file: four full 500-row server pages. A
    // client that reads only the first page reports 1500 of them absent.
    std::string content;
    for (int i = 0; i < 2000; ++i) {
        content += "export function sym_page_" + std::to_string(i) +
                   "(): void {}\n";
    }
    write_corpus_file(root, "many.ts", content);

    std::string out;
    ASSERT_TRUE(run_lci_search(
        lci_bin, root,
        {lci_bin.string(), "symbols", "--file", "*.ts", "--json", "-m",
         "5000"},
        out));
    auto j = nlohmann::json::parse(out);
    EXPECT_EQ(j.value("total", -1), 2000) << out.substr(0, 400);
    ASSERT_TRUE(j.contains("symbols") && j["symbols"].is_array()) << out;
    EXPECT_EQ(j["symbols"].size(), 2000u) << out.substr(0, 400);
    bool saw_last = false;
    for (const auto& s : j["symbols"]) {
        if (s.value("name", "") == "sym_page_1999") saw_last = true;
    }
    EXPECT_TRUE(saw_last) << "symbol past the 500-row page must be returned";

    shutdown_lci_server(lci_bin, root);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(MemProfileTest, SizeTCountsFormatWithZu) {
    // Compile-time contract: size_t counts print with %zu, never %d/%ld.
    // -Wformat (in -Wall) rejects a mismatched conversion, so a %d/%ld
    // regression against a size_t argument fails the build, not the test.
    char buf[32];
    const size_t n = 42;
    std::snprintf(buf, sizeof(buf), "%zu", n);
    EXPECT_STREQ(buf, "42");
}

}  // namespace
}  // namespace cli
}  // namespace lci
