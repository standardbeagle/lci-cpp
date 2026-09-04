#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>
#include <lci/search/symbol_type_alias.h>
#include <lci/mcp/handlers_core.h>
#include <nlohmann/json.hpp>

#include "unique_temp.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace lci {
namespace {

// -- Temp directory helper (matches existing test patterns) -------------------

class TempDir {
  public:
    TempDir() {
        path_ = test::unique_temp_dir("lci_sengine_test_");
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    void write_file(const std::string& rel_path,
                    const std::string& content) {
        auto full = path_ / rel_path;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream f(full);
        f << content;
    }

  private:
    std::filesystem::path path_;
};

// -- Pure function tests ------------------------------------------------------

TEST(SearchPureFunctions, SearchLineNumber) {
    EXPECT_EQ(1, search_line_number("", 0));
    EXPECT_EQ(1, search_line_number("abc", 0));
    EXPECT_EQ(1, search_line_number("abc\ndef", 2));
    EXPECT_EQ(2, search_line_number("abc\ndef", 4));
    EXPECT_EQ(3, search_line_number("a\nb\nc", 4));
}

TEST(SearchPureFunctions, SearchLineStart) {
    EXPECT_EQ(0, search_line_start("abc\ndef", 0));
    EXPECT_EQ(0, search_line_start("abc\ndef", 2));
    EXPECT_EQ(4, search_line_start("abc\ndef", 5));
}

TEST(SearchPureFunctions, SearchLineEnd) {
    EXPECT_EQ(3, search_line_end("abc\ndef", 0));
    EXPECT_EQ(3, search_line_end("abc\ndef", 2));
    EXPECT_EQ(7, search_line_end("abc\ndef", 5));
}

TEST(SearchPureFunctions, IsWordCharacter) {
    EXPECT_TRUE(is_word_character('a'));
    EXPECT_TRUE(is_word_character('Z'));
    EXPECT_TRUE(is_word_character('5'));
    EXPECT_TRUE(is_word_character('_'));
    EXPECT_FALSE(is_word_character(' '));
    EXPECT_FALSE(is_word_character('.'));
    EXPECT_FALSE(is_word_character('\n'));
}

TEST(SearchPureFunctions, IsWordBoundary) {
    std::string_view content = "hello world";
    EXPECT_TRUE(is_word_boundary(content, 0));
    EXPECT_FALSE(is_word_boundary(content, 1));
    EXPECT_TRUE(is_word_boundary(content, 5));
    EXPECT_TRUE(is_word_boundary(content, 6));
}

TEST(SearchPureFunctions, CalculatePatternComplexity) {
    EXPECT_EQ(0, calculate_pattern_complexity(""));
    EXPECT_GT(calculate_pattern_complexity("myFunction"), 10);
    EXPECT_GT(calculate_pattern_complexity("camelCase"),
              calculate_pattern_complexity("simple"));
}

TEST(SearchPureFunctions, CalculateMatchQuality) {
    std::string_view content = "func doStuff() {\n";
    double q = calculate_match_quality(content, 5, 12, "doStuff");
    EXPECT_GT(q, kBaseMatchScore);
}

TEST(SearchPureFunctions, SearchBinaryLineOffset) {
    std::vector<int> offsets = {0, 4, 8};
    EXPECT_EQ(1, search_binary_line_offset(offsets, 0));
    EXPECT_EQ(1, search_binary_line_offset(offsets, 3));
    EXPECT_EQ(2, search_binary_line_offset(offsets, 5));
    EXPECT_EQ(3, search_binary_line_offset(offsets, 10));
}

// -- File classification tests ------------------------------------------------

TEST(FileClassification, CodeFiles) {
    EXPECT_EQ(FileCategory::Code, classify_file("main.go"));
    EXPECT_EQ(FileCategory::Code, classify_file("lib.rs"));
    EXPECT_EQ(FileCategory::Code, classify_file("app.py"));
    EXPECT_EQ(FileCategory::Code, classify_file("index.tsx"));
}

TEST(FileClassification, DocFiles) {
    EXPECT_EQ(FileCategory::Documentation, classify_file("README.md"));
    EXPECT_EQ(FileCategory::Documentation, classify_file("notes.txt"));
}

TEST(FileClassification, ConfigFiles) {
    EXPECT_EQ(FileCategory::Config, classify_file("config.json"));
    EXPECT_EQ(FileCategory::Config, classify_file("settings.yaml"));
    EXPECT_EQ(FileCategory::Config, classify_file("app.kdl"));
}

TEST(FileClassification, TestFiles) {
    EXPECT_EQ(FileCategory::Test, classify_file("main_test.go"));
    EXPECT_EQ(FileCategory::Test, classify_file("app.test.js"));
    EXPECT_EQ(FileCategory::Test, classify_file("app.spec.ts"));
    EXPECT_EQ(FileCategory::Test, classify_file("test_utils.py"));
}

TEST(FileClassification, UnknownFiles) {
    EXPECT_EQ(FileCategory::Unknown, classify_file("Makefile"));
    EXPECT_EQ(FileCategory::Unknown, classify_file("data.bin"));
}

TEST(FileClassification, ExtensionFromBasenameOnly) {
    // Go filepath.Ext takes the extension from the FINAL path element only; a
    // dot in a parent directory name must not leak into the extension.
    EXPECT_EQ("", file_extension("dir.v1/Makefile"));
    EXPECT_EQ(FileCategory::Unknown, classify_file("dir.v1/Makefile"));
    // Sanity: a genuine basename extension is still returned.
    EXPECT_EQ(".go", file_extension("dir.v1/main.go"));
}

TEST(FileClassification, ScoreFileType) {
    EXPECT_DOUBLE_EQ(kCodeFileBoost, score_file_type("main.go"));
    EXPECT_DOUBLE_EQ(kDocFilePenalty, score_file_type("README.md"));
    EXPECT_DOUBLE_EQ(kConfigFileBoost, score_file_type("config.json"));
    EXPECT_DOUBLE_EQ(kCodeFileBoost * 0.8, score_file_type("main_test.go"));
}

TEST(FileClassification, IsTestFile) {
    EXPECT_TRUE(is_test_file("main_test.go"));
    EXPECT_TRUE(is_test_file("app.spec.ts"));
    EXPECT_FALSE(is_test_file("main.go"));
}

// -- SearchCoordinator tests --------------------------------------------------

TEST(SearchCoordinatorTest, DeduplicateEmpty) {
    auto result = SearchCoordinator::deduplicate({});
    EXPECT_TRUE(result.empty());
}

TEST(SearchCoordinatorTest, DeduplicateSingle) {
    std::vector<SearchResult> input;
    input.push_back(SearchResult{FileID{1}, "a.go", 10, 0, "match", 100.0, {}});
    auto result = SearchCoordinator::deduplicate(std::move(input));
    ASSERT_EQ(1u, result.size());
    EXPECT_EQ(10, result[0].line);
}

TEST(SearchCoordinatorTest, DeduplicateKeepsHigherScore) {
    std::vector<SearchResult> input;
    input.push_back(SearchResult{FileID{1}, "a.go", 10, 0, "m1", 50.0, {}});
    input.push_back(SearchResult{FileID{1}, "a.go", 10, 5, "m2", 80.0, {}});
    auto result = SearchCoordinator::deduplicate(std::move(input));
    ASSERT_EQ(1u, result.size());
    EXPECT_DOUBLE_EQ(80.0, result[0].score);
}

TEST(SearchCoordinatorTest, DeduplicateDifferentLines) {
    std::vector<SearchResult> input;
    input.push_back(SearchResult{FileID{1}, "a.go", 10, 0, "m1", 50.0, {}});
    input.push_back(SearchResult{FileID{1}, "a.go", 20, 0, "m2", 80.0, {}});
    auto result = SearchCoordinator::deduplicate(std::move(input));
    EXPECT_EQ(2u, result.size());
}

TEST(SearchCoordinatorTest, MergeTwoSets) {
    std::vector<SearchResult> a;
    a.push_back(SearchResult{FileID{1}, "a.go", 10, 0, "m", 50.0, {}});

    std::vector<SearchResult> b;
    b.push_back(SearchResult{FileID{1}, "a.go", 10, 0, "m", 80.0, {}});
    b.push_back(SearchResult{FileID{2}, "b.go", 5, 0, "m", 60.0, {}});

    auto result = SearchCoordinator::merge(std::move(a), std::move(b));
    EXPECT_EQ(2u, result.size());
}

TEST(SearchCoordinatorTest, RankByScore) {
    std::vector<SearchResult> results;
    results.push_back(SearchResult{FileID{1}, "a.go", 1, 0, "", 50.0, {}});
    results.push_back(SearchResult{FileID{2}, "b.go", 1, 0, "", 100.0, {}});
    results.push_back(SearchResult{FileID{3}, "c.go", 1, 0, "", 75.0, {}});

    SearchCoordinator::rank(results);

    EXPECT_DOUBLE_EQ(100.0, results[0].score);
    EXPECT_DOUBLE_EQ(75.0, results[1].score);
    EXPECT_DOUBLE_EQ(50.0, results[2].score);
}

TEST(SearchCoordinatorTest, RankBreaksTiesByPath) {
    std::vector<SearchResult> results;
    results.push_back(SearchResult{FileID{1}, "b.go", 1, 0, "", 50.0, {}});
    results.push_back(SearchResult{FileID{2}, "a.go", 1, 0, "", 50.0, {}});

    SearchCoordinator::rank(results);

    EXPECT_EQ("a.go", results[0].path);
    EXPECT_EQ("b.go", results[1].path);
}

// -- Context extractor tests --------------------------------------------------

TEST(ContextExtractorTest, ExtractLineContext) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    TempDir dir;
    dir.write_file("ctx.go",
        "package main\n"
        "\n"
        "func hello() {\n"
        "    fmt.Println(\"hello\")\n"
        "}\n"
        "\n"
        "func world() {\n"
        "    fmt.Println(\"world\")\n"
        "}\n");

    std::string file_path = (dir.path() / "ctx.go").string();
    ASSERT_TRUE(mi.index_file(file_path));

    ContextExtractor extractor(mi.file_content_store(), 50);
    std::vector<BlockBoundary> blocks;

    auto ctx = extractor.extract(FileID{1}, blocks, 4, 2);
    EXPECT_FALSE(ctx.lines.empty());
    EXPECT_GT(ctx.start_line, 0);
    EXPECT_GE(ctx.end_line, ctx.start_line);
}

// The window size is a TOTAL, match line included: an even num_lines used to
// return num_lines + 1 lines (including the default 50 and the 100 that
// extract_block_context falls back to).
TEST(ContextExtractorTest, WindowHoldsExactlyNumLines) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    TempDir dir;
    std::string body;
    for (int i = 1; i <= 40; ++i) {
        body += "line" + std::to_string(i) + "\n";
    }
    dir.write_file("wide.txt", body);
    ASSERT_TRUE(mi.index_file((dir.path() / "wide.txt").string()));

    ContextExtractor extractor(mi.file_content_store(), 50);
    std::vector<BlockBoundary> blocks;

    for (int n = 1; n <= 8; ++n) {
        auto ctx = extractor.extract(FileID{1}, blocks, 20, n);
        EXPECT_EQ(static_cast<size_t>(n), ctx.lines.size())
            << "num_lines=" << n;
        EXPECT_EQ(ctx.end_line - ctx.start_line + 1,
                  static_cast<int>(ctx.lines.size()))
            << "num_lines=" << n;
        // The match line is always inside the window.
        EXPECT_LE(ctx.start_line, 20);
        EXPECT_GE(ctx.end_line, 20);
    }
}

// The 100-line window of the long-function branch is built unclamped
// (start + 100). Pins that it still cannot run past the end of the file for a
// match on a long function's last statement.
TEST(ContextExtractorTest, LongFunctionWindowStaysInsideTheFile) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    TempDir dir;
    std::string body = "func big() {\n";
    for (int i = 1; i <= 150; ++i) {
        body += "    step" + std::to_string(i) + "()\n";
    }
    body += "}\n";
    dir.write_file("big.go", body);
    ASSERT_TRUE(mi.index_file((dir.path() / "big.go").string()));

    // Whole file is one 152-line function (0-based block bounds).
    std::vector<BlockBoundary> blocks;
    BlockBoundary fn;
    fn.type = BlockType::Function;
    fn.name = "big";
    fn.start = 0;
    fn.end = 151;
    blocks.push_back(fn);

    // Match on the last statement: the 100-line window runs off the end.
    ContextExtractor extractor(mi.file_content_store(), 50);
    auto ctx = extractor.extract_function_context(FileID{1}, blocks, 151, 5);
    EXPECT_LE(ctx.end_line, 152);
    EXPECT_EQ(ctx.end_line - ctx.start_line + 1,
              static_cast<int>(ctx.lines.size()));
}

// -- SearchEngine integration tests -------------------------------------------

TEST(SearchEngineIntegrationTest, BasicSearch) {
    TempDir dir;
    dir.write_file("main.go",
        "package main\n"
        "\n"
        "func main() {\n"
        "    fmt.Println(\"hello world\")\n"
        "}\n");
    dir.write_file("util.go",
        "package main\n"
        "\n"
        "func helper() string {\n"
        "    return \"hello\"\n"
        "}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    auto results = engine.search("hello", opts);
    EXPECT_GE(results.size(), 1u);

    for (const auto& r : results) {
        EXPECT_NE(FileID{0}, r.file_id);
        EXPECT_FALSE(r.path.empty());
        EXPECT_GT(r.line, 0);
    }
}

TEST(SearchEngineIntegrationTest, PunctuationPatternsNotSilentlyDropped) {
    // MCP-path pin (SearchEngine::search -> find_candidate_files): patterns
    // built from punctuation/operator bytes the tokenizer discards must still
    // reach the verify scan. Zero results must mean truly absent (Karpathy
    // rule 6), never "the tokenizer dropped the pattern".
    TempDir dir;
    dir.write_file("dump.cpp",
        "void emit(Json payload) {\n"
        "    payload.dump(2);\n"
        "    try { run(); } catch (...) {}\n"
        "    auto p = payload.value(\"params\", 0);\n"
        "}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    for (const char* pattern :
         {".dump(", "catch (...)", "value(\"params\""}) {
        auto results = engine.search(pattern, opts);
        ASSERT_GE(results.size(), 1u) << "silent zero for: " << pattern;
        EXPECT_TRUE(results.front().path.ends_with("dump.cpp"));
    }

    // Discrimination pair: same shape, genuinely absent.
    EXPECT_TRUE(engine.search(".undump(", opts).empty());
}

TEST(SearchEngineIntegrationTest, CaseInsensitiveSearch) {
    TempDir dir;
    dir.write_file("case.js",
        "function HelloWorld() {\n"
        "    return 'helloworld';\n"
        "}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.case_insensitive = true;
    auto results = engine.search("helloworld", opts);
    EXPECT_GE(results.size(), 1u);
}

TEST(SearchEngineIntegrationTest, MaxResultsLimit) {
    TempDir dir;
    std::string content;
    for (int i = 0; i < 50; ++i) {
        content += "var item" + std::to_string(i) + " = " +
                   std::to_string(i) + "\n";
    }
    dir.write_file("many.go", "package main\n" + content);

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.max_results = 5;
    auto results = engine.search("item", opts);
    EXPECT_LE(static_cast<int>(results.size()), 5);
}

TEST(SearchEngineIntegrationTest, SearchWithContext) {
    TempDir dir;
    dir.write_file("sample.py",
        "def greet(name):\n"
        "    message = f\"Hello {name}\"\n"
        "    print(message)\n"
        "    return message\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.max_context_lines = 2;
    auto results = engine.search("message", opts);
    EXPECT_GE(results.size(), 1u);

    bool found_context = false;
    for (const auto& r : results) {
        if (!r.context.lines.empty()) {
            found_context = true;
            EXPECT_GT(r.context.start_line, 0);
            EXPECT_GE(r.context.end_line, r.context.start_line);
        }
    }
    EXPECT_TRUE(found_context);
}

TEST(SearchEngineIntegrationTest, ResultsAreRanked) {
    TempDir dir;
    dir.write_file("code.go",
        "package main\nfunc doStuff() { return }\n");
    dir.write_file("readme.md",
        "# doStuff\nThis function does stuff.\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    auto results = engine.search("doStuff", opts);
    EXPECT_GE(results.size(), 1u);

    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_GE(results[i - 1].score, results[i].score);
    }

    if (results.size() >= 2) {
        bool code_first = false;
        for (const auto& r : results) {
            if (r.path.ends_with(".go")) {
                code_first = true;
                break;
            }
            if (r.path.ends_with(".md")) break;
        }
        EXPECT_TRUE(code_first);
    }
}

TEST(SearchEngineIntegrationTest, EmptyPatternReturnsNothing) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    SearchEngine engine(mi);
    SearchOptions opts;
    auto results = engine.search("", opts);
    EXPECT_TRUE(results.empty());
}

TEST(SearchEngineIntegrationTest, PatternTooLongReturnsNothing) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    SearchEngine engine(mi);
    std::string long_pattern(1001, 'x');
    SearchOptions opts;
    auto results = engine.search(long_pattern, opts);
    EXPECT_TRUE(results.empty());
}

TEST(SearchEngineIntegrationTest, WordBoundarySearch) {
    TempDir dir;
    dir.write_file("words.go",
        "package main\n"
        "var fooBar = 1\n"
        "var fooBarBaz = 2\n"
        "var foo = 3\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.word_boundary = true;
    auto results = engine.search("foo", opts);

    bool found_exact = false;
    for (const auto& r : results) {
        if (r.match_text == "foo") found_exact = true;
    }
    EXPECT_TRUE(found_exact);
}

// -- Trigram prefilter reality check ------------------------------------------

// Pins the candidate contract on the BULK path. The read-side trigram
// postings are filled only by the incremental TrigramIndex::index_file path;
// bulk indexing (index_directory -> Pipeline) routes trigrams into
// ShardedTrigramStorage, which the search path never reads. Under
// certified-absence narrowing that is not a problem and needs no
// get_all_file_ids fallback: an index with no coverage certifies nothing, so
// the file stays in the candidate set and the verify scan finds the match.
TEST(SearchEngineIntegrationTest, BulkIndexKeepsUncertifiedFilesAsCandidates) {
    TempDir dir;
    dir.write_file("alpha.go",
        "package main\n"
        "func distinctiveNeedle() int { return 7 }\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    // No index covering this corpus can prove the pattern absent, so the
    // containing file must survive candidate selection.
    auto candidates = mi.find_candidate_files("distinctiveNeedle", false);
    FileID id = mi.path_to_id((dir.path() / "alpha.go").string());
    ASSERT_NE(id, FileID{0});
    EXPECT_NE(std::find(candidates.begin(), candidates.end(), id),
              candidates.end())
        << "a file no index certifies pattern-free was dropped from the "
           "candidate set";

    SearchEngine engine(mi);
    SearchOptions opts;
    auto results = engine.search("distinctiveNeedle", opts);
    EXPECT_FALSE(results.empty());
}

// -- Determinism --------------------------------------------------------------

// Candidate FileIDs must be scanned in sorted order: both candidate sources
// are built by walking an absl hash map, whose iteration order is randomized
// per process, and that order picks WHICH matches survive the collection cap.
TEST(SearchEngineIntegrationTest, CappedCollectionTakesLowestFileIds) {
    TempDir dir;
    constexpr int kFiles = 40;
    for (int i = 0; i < kFiles; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "f%02d.go", i);
        dir.write_file(name,
            "package main\n"
            "func f() { needleToken() }\n");
    }

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    // max_results=3 => collection cap 24, so 16 of the 40 files are dropped.
    SearchEngine engine(mi);
    SearchOptions opts;
    opts.max_results = 3;
    auto results = engine.search("needleToken", opts);
    ASSERT_FALSE(results.empty());

    // Collection visits the 24 lowest FileIDs; all 40 files score equally, so
    // rank() breaks the tie on path and the output cap keeps the three
    // lexicographically smallest of those 24. Any other trio means the scan
    // followed hash order.
    auto ids = mi.get_all_file_ids();
    ASSERT_EQ(static_cast<size_t>(kFiles), ids.size());
    std::sort(ids.begin(), ids.end());
    std::vector<std::string> collected;
    for (size_t i = 0; i < 24 && i < ids.size(); ++i) {
        collected.push_back(mi.get_file_path(ids[i]));
    }
    std::sort(collected.begin(), collected.end());

    ASSERT_EQ(3u, results.size());
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(collected[i], results[i].path);
    }
}

// -- Line/column resolution ---------------------------------------------------

// process_file resolves lines with an incremental cursor instead of rescanning
// from byte 0 per match. Pins the exact line/column of every match in a file
// with many hits so the optimisation cannot drift the emitted values.
TEST(SearchEngineIntegrationTest, MultipleMatchesResolveExactLinesAndColumns) {
    TempDir dir;
    dir.write_file("multi.go",
        "package main\n"        // line 1
        "\n"                    // line 2
        "var a = tok\n"         // line 3, col 8
        "var bb = tok\n"        // line 4, col 9
        "\n"                    // line 5
        "  var ccc = tok\n");   // line 6, col 12

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    auto results = engine.search("tok", opts);
    ASSERT_EQ(3u, results.size());

    std::sort(results.begin(), results.end(),
              [](const SearchResult& a, const SearchResult& b) {
                  return a.line < b.line;
              });
    EXPECT_EQ(3, results[0].line);
    EXPECT_EQ(8, results[0].column);
    EXPECT_EQ(4, results[1].line);
    EXPECT_EQ(9, results[1].column);
    EXPECT_EQ(6, results[2].line);
    EXPECT_EQ(12, results[2].column);
}

// -- Pattern validation -------------------------------------------------------

TEST(SearchEngineIntegrationTest, OverlongPatternReportsAnError) {
    TempDir dir;
    dir.write_file("a.go", "package main\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    SearchStats stats;
    std::string pattern(kMaxSearchPatternBytes + 1, 'x');
    auto results = engine.search(pattern, opts, &stats);

    EXPECT_TRUE(results.empty());
    EXPECT_FALSE(stats.error.empty());

    // A valid query that simply has no hits must stay distinguishable.
    SearchStats no_hits;
    auto none = engine.search("absentToken", opts, &no_hits);
    EXPECT_TRUE(none.empty());
    EXPECT_TRUE(no_hits.error.empty());
}

TEST(SearchEngineIntegrationTest, EmptyPatternReportsAnError) {
    TempDir dir;
    dir.write_file("a.go", "package main\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    SearchStats stats;
    auto results = engine.search("", opts, &stats);
    EXPECT_TRUE(results.empty());
    EXPECT_FALSE(stats.error.empty());
}

// A path filter that fails to compile must surface an error, never silently
// degrade into "no filter" (which searches a superset of what was asked).
TEST(SearchEngineIntegrationTest, BrokenIncludePatternReportsAnError) {
    TempDir dir;
    dir.write_file("a.go", "package main // tokHit\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.include_pattern = "([";  // invalid RE2
    SearchStats stats;
    auto results = engine.search("tokHit", opts, &stats);
    EXPECT_TRUE(results.empty());
    ASSERT_FALSE(stats.error.empty());
    EXPECT_NE(stats.error.find("include_pattern"), std::string::npos)
        << stats.error;

    // A valid include filter still works.
    SearchOptions ok_opts;
    ok_opts.include_pattern = "\\.go$";
    SearchStats ok_stats;
    auto ok = engine.search("tokHit", ok_opts, &ok_stats);
    EXPECT_EQ(1u, ok.size());
    EXPECT_TRUE(ok_stats.error.empty());
}

TEST(SearchEngineIntegrationTest, BrokenExcludePatternReportsAnError) {
    TempDir dir;
    dir.write_file("a.go", "package main // tokHit\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.exclude_pattern = "*bad";  // invalid RE2 (leading repetition)
    SearchStats stats;
    auto results = engine.search("tokHit", opts, &stats);
    EXPECT_TRUE(results.empty());
    ASSERT_FALSE(stats.error.empty());
    EXPECT_NE(stats.error.find("exclude_pattern"), std::string::npos)
        << stats.error;
}

// -- S5 regression tests ------------------------------------------------------

// Criterion 3: an uncompilable RE2 pattern must be reported, not silently
// turned into "0 matches". Pre-fix, SearchEngine::search never validated the
// regex: find_content_matches recompiled it per candidate file, every compile
// failed, and the caller got an empty vector with an EMPTY stats.error --
// indistinguishable from a valid query that matched nothing.
TEST(SearchEngineRegexValidation, InvalidRegexReportsErrorInsteadOfZeroMatches) {
    TempDir dir;
    dir.write_file("a.go", "package main\nfunc Alpha() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.use_regex = true;

    SearchStats stats;
    auto results = engine.search("(", opts, &stats);

    EXPECT_TRUE(results.empty());
    EXPECT_NE(stats.error.find("invalid regex"), std::string::npos)
        << "stats.error was: '" << stats.error << "'";
}

// Criterion 3, valid-regex control: validation must not reject good patterns.
TEST(SearchEngineRegexValidation, ValidRegexStillSearches) {
    TempDir dir;
    dir.write_file("a.go", "package main\nfunc Alpha() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.use_regex = true;

    SearchStats stats;
    auto results = engine.search("func +Alpha", opts, &stats);
    EXPECT_TRUE(stats.error.empty()) << stats.error;
    EXPECT_GE(results.size(), 1u);
}

// Criterion 5: a whole-word, exact-case hit must outrank a substring hit of
// the same pattern in the same file. Pre-fix, score_result() derived the score
// from the file class and the pattern text ONLY -- both identical here -- so
// every row tied and rank() fell through to its path/line tiebreak, making the
// order purely positional: the substring hit on the earlier line won.
TEST(SearchEngineRanking, ExactWordMatchOutranksSubstringMatch) {
    TempDir dir;
    dir.write_file("a.go",
        "package main\n"
        "var Configuration = 1\n"   // line 2: substring hit, earlier
        "var other = 2\n"
        "var Config = 3\n");        // line 4: whole-word exact hit, later

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.case_insensitive = false;
    auto results = engine.search("Config", opts);

    ASSERT_GE(results.size(), 2u);
    EXPECT_EQ(4, results[0].line)
        << "expected the whole-word hit (line 4) to rank first, got line "
        << results[0].line;
    EXPECT_GT(results[0].score, results[1].score);
}

// Criterion 7: a file whose bytes were evicted from the content store is still
// a search candidate (process_file reloads it into a request-local buffer), but
// the context extractor re-fetched the content from the store BY ID and got
// nothing -- so the row came back with a match and an EMPTY context block.
TEST(SearchEngineContext, EvictedFileStillGetsLineContext) {
    TempDir dir;
    dir.write_file("a.go",
        "package main\n"
        "// leading\n"
        "func Target() {}\n"
        "// trailing\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.max_context_lines = 5;

    auto before = engine.search("Target", opts);
    ASSERT_GE(before.size(), 1u);
    ASSERT_FALSE(before[0].context.lines.empty());

    // Simulate LRU eviction of the file's bytes while it stays a candidate.
    mi.file_content_store().invalidate_file_by_id(before[0].file_id);

    auto after = engine.search("Target", opts);
    ASSERT_GE(after.size(), 1u) << "evicted file dropped out of results";
    EXPECT_FALSE(after[0].context.lines.empty())
        << "evicted file returned a match with empty context";
}

// Criterion 2: the MCP `search` tool description advertises short symbol-type
// aliases (func, var, cls, ...), but the engine compared the caller's strings
// verbatim against SymbolType names. Every advertised alias matched nothing,
// and the caller got an empty result set plus a hint blaming its pattern.
TEST(SearchSymbolTypeFilter, AliasReturnsSameSetAsCanonicalName) {
    TempDir dir;
    dir.write_file("a.go",
        "package main\n"
        "func Widget() {}\n"
        "var Widgets = 1\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);

    SearchOptions canonical;
    canonical.symbol_types = {"function"};
    auto by_canonical = engine.search("Widget", canonical);

    SearchOptions alias;
    alias.symbol_types = {"func"};
    auto by_alias = engine.search("Widget", alias);

    ASSERT_FALSE(by_canonical.empty())
        << "control: symbol_types=function matched nothing, test is not "
           "measuring the alias";
    ASSERT_EQ(by_canonical.size(), by_alias.size());
    for (size_t i = 0; i < by_canonical.size(); ++i) {
        EXPECT_EQ(by_canonical[i].path, by_alias[i].path);
        EXPECT_EQ(by_canonical[i].line, by_alias[i].line);
    }
}

// Criterion 2, other half: an unknown symbol type is a caller mistake and must
// be reported. Pre-fix it filtered every row away and returned zero matches --
// the same answer as a correct query over an empty corpus.
TEST(SearchSymbolTypeFilter, UnknownTypeIsAnErrorNotZeroResults) {
    TempDir dir;
    dir.write_file("a.go", "package main\nfunc Widget() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.symbol_types = {"funtcion"};  // typo

    SearchStats stats;
    auto results = engine.search("Widget", opts, &stats);

    EXPECT_TRUE(results.empty());
    EXPECT_NE(stats.error.find("funtcion"), std::string::npos)
        << "stats.error was: '" << stats.error << "'";
    EXPECT_NE(stats.error.find("symbol_type"), std::string::npos)
        << "stats.error was: '" << stats.error << "'";
}

// Alias canonicalization is a pure table lookup; pin it directly so a future
// edit to the table cannot quietly drop an advertised alias.
TEST(SymbolTypeAlias, CanonicalizesAdvertisedAliases) {
    EXPECT_EQ("function", canonical_symbol_type("func"));
    EXPECT_EQ("function", canonical_symbol_type("fn"));
    EXPECT_EQ("function", canonical_symbol_type("def"));
    EXPECT_EQ("variable", canonical_symbol_type("var"));
    EXPECT_EQ("constant", canonical_symbol_type("const"));
    EXPECT_EQ("class", canonical_symbol_type("cls"));
    EXPECT_EQ("method", canonical_symbol_type("meth"));
    EXPECT_EQ("interface", canonical_symbol_type("iface"));
    // Canonical names pass through, case-insensitively.
    EXPECT_EQ("function", canonical_symbol_type("Function"));
    EXPECT_EQ("enum_member", canonical_symbol_type("ENUM_MEMBER"));
    // Deliberate divergence from the tool description: `trait` is its own
    // SymbolType, so it must NOT fold into `interface`.
    EXPECT_EQ("trait", canonical_symbol_type("trait"));
    // Unknown spellings are reported, never silently accepted.
    EXPECT_TRUE(canonical_symbol_type("funtcion").empty());
    EXPECT_TRUE(canonical_symbol_type("").empty());
}

// Criterion 4: `output=files` collapsed the ranked rows to paths by comparing
// each against only the PREVIOUS one. Results are ordered by score, so rows
// for one file interleave with rows for others: a file that appears at ranks
// 1 and 3 was emitted twice, and unique_files counted it twice.
TEST(SearchCoordinatorTest, UniquePathsCollapsesNonAdjacentDuplicates) {
    // Ranked order interleaves a.go and b.go, as a score-ordered list does.
    std::vector<std::string> ranked{"a.go", "b.go", "a.go", "b.go"};

    auto paths = SearchCoordinator::unique_paths(ranked);

    ASSERT_EQ(2u, paths.size())
        << "non-adjacent duplicate paths survived";
    EXPECT_EQ("a.go", paths[0]);  // best-ranked appearance wins
    EXPECT_EQ("b.go", paths[1]);
}

TEST(SearchCoordinatorTest, UniquePathsKeepsAdjacentAndEmptyCases) {
    EXPECT_TRUE(SearchCoordinator::unique_paths({}).empty());

    std::vector<std::string> adjacent{"a.go", "a.go", "b.go"};
    auto paths = SearchCoordinator::unique_paths(adjacent);
    ASSERT_EQ(2u, paths.size());
    EXPECT_EQ("a.go", paths[0]);
    EXPECT_EQ("b.go", paths[1]);

    std::vector<std::string> single{"only.go"};
    EXPECT_EQ(1u, SearchCoordinator::unique_paths(single).size());
}

// Criterion 1: the MCP handler sets SearchOptions::exclude_comments and
// ::invert_match from flags nc/iv, and the engine never read either field.
// The only reader was semantic_filter.cpp, which had no production caller and
// has since been deleted as part of the same slice. So
// `flags=iv` returned exactly the lines it was asked to exclude, and
// `flags=nc` returned the comments it was asked to drop.
TEST(SearchFlagNoComments, ExcludesCommentOnlyLines) {
    TempDir dir;
    dir.write_file("a.go",
        "package main\n"          // 1
        "// Config is a comment\n" // 2 comment-only, matches
        "var Config = 1\n"        // 3 code, matches
        "run(Config)  // Config\n");  // 4 code with trailing comment, matches
    // The '#' case belongs in a language where '#' actually opens a comment.
    // Go has none: a '#' line in a .go file is not a comment (and would not
    // compile), so asserting it away here was asserting the defect.
    dir.write_file("b.py",
        "# Config hash\n"         // 1 comment-only, matches
        "Config = 2\n");          // 2 code, matches

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);

    SearchOptions all;
    auto every = engine.search("Config", all);
    ASSERT_EQ(5u, every.size())
        << "control: a.go lines 2,3,4 and b.py lines 1,2";

    SearchOptions no_comments;
    no_comments.exclude_comments = true;
    auto kept = engine.search("Config", no_comments);

    std::vector<std::pair<std::string, int>> got;
    for (const auto& r : kept) {
        got.emplace_back(std::filesystem::path(r.path).filename().string(),
                         r.line);
    }
    std::sort(got.begin(), got.end());
    std::vector<std::pair<std::string, int>> want{
        {"a.go", 3}, {"a.go", 4}, {"b.py", 2}};
    EXPECT_EQ(want, got)
        << "// comment-only and python '#' must be dropped; a trailing comment "
           "on a code line must not drop the line";
}

// `iv` must behave like `rg -v`: every line that does NOT match, across the
// searched corpus -- not merely the non-matching lines of files that happen to
// contain a match.
TEST(SearchFlagInvertMatch, ReturnsOnlyNonMatchingLines) {
    TempDir dir;
    dir.write_file("a.go",
        "package main\n"     // 1 no match
        "var Config = 1\n"   // 2 match
        "var other = 2\n");  // 3 no match

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.invert_match = true;
    auto results = engine.search("Config", opts);

    std::vector<int> lines;
    for (const auto& r : results) {
        EXPECT_EQ(std::string::npos, r.match_text.find("Config"))
            << "invert returned a line containing the pattern";
        lines.push_back(r.line);
    }
    std::sort(lines.begin(), lines.end());
    EXPECT_EQ((std::vector<int>{1, 3}), lines);
}

// rg -v reports non-matching lines from files with NO match at all. Scoping
// invert to the trigram candidate set (files that contain the pattern) would
// silently drop those files entirely.
TEST(SearchFlagInvertMatch, CoversFilesWithNoMatchAtAll) {
    TempDir dir;
    dir.write_file("has.go", "package main\nvar Config = 1\n");
    dir.write_file("none.go", "package main\nvar plain = 2\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.invert_match = true;
    auto results = engine.search("Config", opts);

    bool saw_none_go = false;
    for (const auto& r : results) {
        if (r.path.find("none.go") != std::string::npos) saw_none_go = true;
    }
    EXPECT_TRUE(saw_none_go)
        << "a file containing no match contributes every one of its lines to "
           "rg -v output; scoping invert to the candidate set drops it";
}

// Criterion 4, end-to-end pin of the PRODUCTION path.
//
// SearchCoordinatorTest.UniquePathsCollapsesNonAdjacentDuplicates pins the
// helper, but it would stay green if handle_search were reverted to its old
// adjacent-only loop -- the helper would simply go uncalled. This drives the
// real MCP handler so the wiring itself is pinned.
//
// The corpus is built so the RANKED rows interleave files: a.go carries both
// the best and the worst match, b.go the middle one, so the score order is
// a, b, a. That is precisely the shape an adjacency filter cannot collapse.
TEST(SearchHandlerFilesOutput, NoDuplicatePathsWhenRankedRowsInterleave) {
    TempDir dir;
    dir.write_file("a.go",
        "Widget = 1\n"          // line 1: word boundary + line start + case
        "var q = xWidget2\n");  // line 2: substring, no boundary -> lowest
    dir.write_file("b.go",
        "  var r = Widget\n");  // line 1: word boundary + case, no line start

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));
    SearchEngine engine(mi);

    // Control: the ranked order really does interleave the two files.
    SearchOptions probe;
    probe.case_insensitive = false;
    auto ranked = engine.search("Widget", probe);
    ASSERT_EQ(3u, ranked.size());
    std::vector<std::string> ranked_names;
    for (const auto& r : ranked) {
        ranked_names.push_back(
            std::filesystem::path(r.path).filename().string());
    }
    ASSERT_EQ((std::vector<std::string>{"a.go", "b.go", "a.go"}), ranked_names)
        << "control: this test only measures the defect when the ranked rows "
           "interleave; adjust the corpus if scoring changes";

    nlohmann::json params;
    params["pattern"] = "Widget";
    params["output"] = "files";
    params["flags"] = "cs";
    auto result = mcp::handle_search(params, mi, &engine);
    ASSERT_FALSE(result.is_error) << result.text;

    auto payload = nlohmann::json::parse(result.text);
    ASSERT_TRUE(payload.contains("files")) << result.text;
    auto files = payload["files"].get<std::vector<std::string>>();

    std::vector<std::string> sorted = files;
    std::sort(sorted.begin(), sorted.end());
    ASSERT_TRUE(std::adjacent_find(sorted.begin(), sorted.end()) ==
                sorted.end())
        << "duplicate path in output=files: " << payload["files"].dump();
    EXPECT_EQ(2u, files.size());
    EXPECT_EQ(static_cast<int>(files.size()),
              payload["unique_files"].get<int>())
        << "unique_files disagrees with the emitted list";
}

// Criterion 5, second half: a synonym-expanded hit must rank BELOW an
// original-pattern hit.
//
// Provenance already reaches the engine -- expand_pattern_semantic hands the
// handler a per-pattern synonym flag vector, which the handler forwards to the
// multi-pattern search() overload. Pre-fix that flag was used for ONE thing,
// forcing case-insensitivity on expanded patterns; it never reached scoring.
// So a well-placed synonym hit outscored a poorly-placed hit on the word the
// user actually typed.
TEST(SearchEngineRanking, SynonymHitRanksBelowOriginalPatternHit) {
    TempDir dir;
    // The synonym hit is deliberately the higher-QUALITY match: whole word, at
    // line start, exact case. The original-pattern hit is a mid-token
    // substring. Quality alone would therefore rank the synonym first.
    dir.write_file("a.go",
        "  x = maybe_login_here\n"  // line 1: 'login' substring, low quality
        "signin = 1\n");            // line 2: 'signin' word+start+case, high

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;
    opts.case_insensitive = false;

    // patterns[0] is what the user typed; patterns[1] is synonym-expanded.
    std::vector<std::string> patterns{"login", "signin"};
    std::vector<bool> synonym_flags{false, true};

    auto results = engine.search(patterns, synonym_flags, opts);
    ASSERT_EQ(2u, results.size());

    EXPECT_EQ(1, results[0].line)
        << "the original-pattern hit (line 1) must rank first even though the "
           "synonym hit (line 2) is the better-placed match; got line "
        << results[0].line;
    EXPECT_EQ(2, results[1].line);
}

// Criterion 4 regression: unique_paths compacts in place, and a view of a
// moved-FROM slot cannot be used to recognize a later duplicate.
//
// The inputs below are derived from the failure mode, not from a shape that
// looks reasonable. The bug needs three things in order: a skip (so `keep`
// falls behind `i`), then a NEW path that therefore gets moved, then a
// duplicate of that moved path. Inputs lacking any of those -- including
// {a,b,a,b}, the shape the original tests used -- never move anything before
// a duplicate check and pass no matter which way the function is written.
TEST(SearchCoordinatorTest, UniquePathsSurvivesCompactionBeforeADuplicate) {
    // Skip at index 2 makes keep lag i; c.go is then moved at index 3; the
    // duplicate c.go at index 4 must still be recognized.
    std::vector<std::string> ranked{"a.go", "b.go", "a.go", "c.go", "c.go"};
    auto paths = SearchCoordinator::unique_paths(ranked);

    ASSERT_EQ(3u, paths.size())
        << "duplicate survived a compaction: " << [&] {
               std::string s;
               for (const auto& p : paths) s += p + " ";
               return s;
           }();
    EXPECT_EQ("a.go", paths[0]);
    EXPECT_EQ("b.go", paths[1]);
    EXPECT_EQ("c.go", paths[2]);
}

TEST(SearchCoordinatorTest, UniquePathsSurvivesCompactionAtADifferentOffset) {
    // Same failure mode, compaction starting one slot earlier.
    std::vector<std::string> ranked{"a.go", "a.go", "b.go", "b.go"};
    auto paths = SearchCoordinator::unique_paths(ranked);

    ASSERT_EQ(2u, paths.size())
        << "duplicate survived a compaction: " << [&] {
               std::string s;
               for (const auto& p : paths) s += p + " ";
               return s;
           }();
    EXPECT_EQ("a.go", paths[0]);
    EXPECT_EQ("b.go", paths[1]);
}

// The same failure mode through the real MCP handler. The unit test alone is
// what let the original defect through, so the production path gets its own
// case: the ranked rows must spell a.go, b.go, a.go, c.go, c.go.
TEST(SearchHandlerFilesOutput, NoDuplicatePathsWhenACompactionPrecedesADup) {
    TempDir dir;
    // Scores are engineered so the ranked path sequence is a, b, a, c, c:
    //   a.go:1  word boundary + line start + exact case  (highest)
    //   b.go:1  word boundary + exact case
    //   a.go:2  word boundary only (lowercase, so no exact-case bonus)
    //   c.go:1  substring, exact case only  (lowest, ties with c.go:2)
    //   c.go:2  substring, exact case only
    dir.write_file("a.go",
        "Widget = 1\n"
        "  z = widget\n");
    dir.write_file("b.go",
        "  y = Widget\n");
    dir.write_file("c.go",
        "  q = xWidgety\n"
        "  r = xWidgetz\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));
    SearchEngine engine(mi);

    // Control: without this exact interleaving the test measures nothing.
    SearchOptions probe;
    probe.case_insensitive = true;
    auto ranked = engine.search("Widget", probe);
    std::vector<std::string> ranked_names;
    for (const auto& r : ranked) {
        ranked_names.push_back(
            std::filesystem::path(r.path).filename().string());
    }
    ASSERT_EQ((std::vector<std::string>{"a.go", "b.go", "a.go", "c.go", "c.go"}),
              ranked_names)
        << "control: ranked rows must compact-then-duplicate; adjust the "
           "corpus if scoring changes";

    nlohmann::json params;
    params["pattern"] = "Widget";
    params["output"] = "files";
    auto result = mcp::handle_search(params, mi, &engine);
    ASSERT_FALSE(result.is_error) << result.text;

    auto payload = nlohmann::json::parse(result.text);
    auto files = payload["files"].get<std::vector<std::string>>();

    std::vector<std::string> sorted = files;
    std::sort(sorted.begin(), sorted.end());
    ASSERT_TRUE(std::adjacent_find(sorted.begin(), sorted.end()) ==
                sorted.end())
        << "duplicate path in output=files: " << payload["files"].dump();
    EXPECT_EQ(3u, files.size()) << payload["files"].dump();
    EXPECT_EQ(static_cast<int>(files.size()),
              payload["unique_files"].get<int>());
}

// Property check against a brute-force oracle. Hand-picked shapes are exactly
// what missed the use-after-move above -- both original tests were written to
// a shape that happened to work. This samples the input space instead.
//
// The oracle is an O(n^2) scan that OWNS its strings, so it shares no
// mechanism with the subject's view-based set (bench-harness-oracle-
// independence rule 1). The alphabet mixes short paths, which move via the
// destination's own SSO buffer, with long ones, which move by stealing the
// heap pointer -- the two cases behave differently under a stale view.
//
// Seeded, so a failure is reproducible (karpathy rule 4). Verified
// discriminating: run against the pre-fix implementation this fails on ~14%
// of samples.
TEST(SearchCoordinatorTest, UniquePathsMatchesBruteForceOracleOnRandomInputs) {
    auto oracle = [](const std::vector<std::string>& in) {
        std::vector<std::string> out;
        for (const auto& p : in) {
            bool dup = false;
            for (const auto& q : out) {
                if (q == p) { dup = true; break; }
            }
            if (!dup) out.push_back(p);
        }
        return out;
    };

    const std::vector<std::string> alphabet{
        "a.go", "b.go", "c.go", "d.go",
        std::string(64, 'x') + "_long1.go",
        std::string(64, 'y') + "_long2.go"};

    std::mt19937 rng(20260904);
    std::uniform_int_distribution<size_t> pick_len(0, 10);
    std::uniform_int_distribution<size_t> pick(0, alphabet.size() - 1);

    for (int trial = 0; trial < 5000; ++trial) {
        std::vector<std::string> in;
        size_t n = pick_len(rng);
        in.reserve(n);
        for (size_t i = 0; i < n; ++i) in.push_back(alphabet[pick(rng)]);

        auto want = oracle(in);
        auto got = SearchCoordinator::unique_paths(in);

        ASSERT_EQ(want, got) << "trial " << trial << " input: " << [&] {
            std::string s;
            for (const auto& p : in) s += p.substr(0, 12) + " ";
            return s;
        }();
    }
}

// Criterion 1 follow-up: line_is_comment_only keyed on a line CONTAINING
// "*/", which is wrong in both directions.
//
// The damaging direction is the false positive: `int x = 1; /* note */` and a
// string literal holding "*/" are real code, and flags=nc was deleting them
// from the results. Dropping a line the caller asked for is a silent wrong
// answer -- the same class this slice exists to remove -- and strictly worse
// than keeping a comment line, which is merely noise.
//
// The false negative is a block-comment CONTINUATION line (" * text"), which
// was kept because it starts with neither "//" nor "/*".
//
// A line that is prose inside a block comment and merely ends the block
// ("  trailing prose */") is NOT decidable from the line alone -- it needs
// cross-line state the search path does not carry. That residual false
// negative is accepted deliberately: it keeps a comment line, it does not
// delete code.
TEST(CommentPredicate, KeepsCodeAndCatchesBlockContinuations) {
    // Comment-only: dropped by flags=nc.
    EXPECT_TRUE(line_is_comment_only("// line comment", LangId::Cpp));
    EXPECT_TRUE(line_is_comment_only("   # hash comment", LangId::Python));
    EXPECT_TRUE(line_is_comment_only("# note", LangId::Ruby));

    // '#' is a preprocessor directive in the C family, not a comment. 2,345
    // lines in this repo's own src/ and include/ start with '#' and every one
    // is code, so an ungated rule deleted all of them under flags=nc.
    EXPECT_FALSE(line_is_comment_only("#include <vector>", LangId::Cpp))
        << "#include is a preprocessor directive";
    EXPECT_FALSE(line_is_comment_only("#pragma once", LangId::Cpp))
        << "#pragma is a preprocessor directive";
    EXPECT_FALSE(line_is_comment_only("#endif", LangId::C))
        << "#endif is a preprocessor directive";
    // Unrecognized languages fall through to "not a comment" on purpose:
    // Markdown is not in the language table and '#' opens a heading there,
    // which is content the caller searched for.
    EXPECT_FALSE(line_is_comment_only("# Heading", LangId::Unknown))
        << "an unknown language must not have its '#' lines deleted";

    // PHP 8.0 gave '#[' to ATTRIBUTES, so a '#' line is only a comment when it
    // is not an attribute. This is PHP-specific on purpose: in Python and Ruby
    // '#[' has no meaning, so "#[x]" there really is an ordinary comment and
    // must keep being dropped.
    EXPECT_FALSE(line_is_comment_only("#[Route('/x')]", LangId::PHP))
        << "#[ opens a PHP attribute, which is code";
    EXPECT_FALSE(line_is_comment_only("  #[Attribute]", LangId::PHP))
        << "#[ opens a PHP attribute, which is code";
    EXPECT_TRUE(line_is_comment_only("# a php comment", LangId::PHP))
        << "a plain '#' line is still a comment in PHP";
    EXPECT_TRUE(line_is_comment_only("#[not an attribute]", LangId::Python))
        << "'#[' has no meaning in python; the line is a comment";
    EXPECT_TRUE(line_is_comment_only("#[also a comment]", LangId::Ruby))
        << "'#[' has no meaning in ruby; the line is a comment";
    EXPECT_TRUE(line_is_comment_only("/* block opener */", LangId::Cpp));
    EXPECT_TRUE(line_is_comment_only("  */", LangId::Cpp));

    // A leading '*' is a dereference or a continued expression far more often
    // than it is a comment continuation. Measured over this repo's own
    // sources: of the 25 lines under src/ and include/ whose trimmed form
    // starts with '*', ALL 25 are code -- dereferences (*snapshot_.load(...),
    // *error = "...") and one continued multiplication
    // (* stats.confidence);, trigram_predictor.h:49). Classifying them as
    // comments deletes code the caller asked for.
    EXPECT_FALSE(line_is_comment_only("*ptr = 5;", LangId::Cpp))
        << "a dereference is not a comment";
    EXPECT_FALSE(line_is_comment_only("  *error = \"boom\";", LangId::Cpp))
        << "a dereference is not a comment";
    EXPECT_FALSE(line_is_comment_only("  *snapshot_.load(order);", LangId::Cpp))
        << "a dereference is not a comment";
    EXPECT_FALSE(line_is_comment_only("      * stats.confidence);", LangId::Cpp))
        << "a continued multiplication is not a comment; this exact line is "
           "real code at include/lci/alloc/trigram_predictor.h:49";

    // ACCEPTED FALSE NEGATIVE, asserted so the behaviour is pinned rather than
    // merely believed: a block-comment continuation is KEPT. It is textually
    // identical to the continued expression above, so no per-line classifier
    // can separate them -- deciding it needs the enclosing block state, which
    // the search path does not carry. Keeping a comment line is noise;
    // deleting a code line is a wrong answer, so the tie breaks this way.
    EXPECT_FALSE(line_is_comment_only("  * continuation prose", LangId::Cpp))
        << "accepted residual: indistinguishable from a continued expression";

    // Code: must survive flags=nc.
    EXPECT_FALSE(line_is_comment_only("int x = 1; /* trailing note */", LangId::Cpp))
        << "a code line with a trailing block comment is not comment-only";
    EXPECT_FALSE(line_is_comment_only("std::string s = \"*/\";", LangId::Cpp))
        << "a string literal containing */ is not a comment";
    EXPECT_FALSE(line_is_comment_only("int y = a / *b;", LangId::Cpp));
    EXPECT_FALSE(line_is_comment_only("code(); // trailing line comment", LangId::Cpp));
    EXPECT_FALSE(line_is_comment_only("", LangId::Cpp));
    EXPECT_FALSE(line_is_comment_only("    ", LangId::Cpp));
}

// The same, through the engine: a code line carrying a trailing block comment
// must still be returned under exclude_comments.
TEST(SearchFlagNoComments, KeepsCodeLinesWithTrailingBlockComments) {
    TempDir dir;
    dir.write_file("a.c",
        "/* Widget opener */\n"        // 1 comment-only, dropped
        " * Widget continuation\n"     // 2 comment, KEPT (accepted residual)
        "int Widget = 1; /* keep */\n" // 3 CODE with trailing block comment
        "const char* s = \"Widget */\";\n"  // 4 CODE, */ inside a string
        "  *Widget = ptr;\n");         // 5 CODE, dereference

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions nc;
    nc.case_insensitive = false;
    nc.exclude_comments = true;
    auto results = engine.search("Widget", nc);

    std::vector<int> lines;
    for (const auto& r : results) lines.push_back(r.line);
    std::sort(lines.begin(), lines.end());
    // Line 2 is a block-comment continuation. It is KEPT, and that is the
    // documented residual, not an oversight: it is textually identical to a
    // continued expression ("* stats.confidence);", real code in this repo),
    // so a per-line classifier cannot separate the two. Keeping a comment is
    // noise; the alternative rule deleted code on all 25 leading-'*' lines in
    // this repo. Asserted explicitly so the trade is visible and would have to
    // be changed deliberately.
    EXPECT_EQ((std::vector<int>{2, 3, 4, 5}), lines)
        << "line 1 (/* opener) must go; code lines 3-5 must stay; line 2 is "
           "the accepted false negative";
}

// Criterion 1, fourth defect in this predicate: '#' was treated as a comment
// marker in EVERY language. In C and C++ it opens a preprocessor directive.
// Measured over this repo's own src/ and include/ .cpp/.h files: 2,345 lines
// start with '#', and all 2,345 are code -- 1,996 #include, 121 #pragma, 78
// #endif, plus #if/#ifdef/#define. So `search pattern=include flags=nc` was
// deleting every one of them. Markdown headings fail the same way.
TEST(SearchFlagNoComments, KeepsPreprocessorDirectivesInCFamilyFiles) {
    TempDir dir;
    dir.write_file("a.cpp",
        "#include <Widget.h>\n"     // 1 CODE: preprocessor directive
        "#pragma Widget once\n"     // 2 CODE: preprocessor directive
        "int Widget = 1;\n"         // 3 CODE
        "// Widget comment\n");     // 4 comment
    // '#' really is a line comment here, so the gate must not over-correct.
    dir.write_file("b.py",
        "# Widget note\n"           // 1 comment
        "Widget = 2\n");            // 2 CODE

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchEngine engine(mi);
    SearchOptions nc;
    nc.case_insensitive = false;
    nc.exclude_comments = true;
    auto results = engine.search("Widget", nc);

    std::vector<std::pair<std::string, int>> got;
    for (const auto& r : results) {
        got.emplace_back(std::filesystem::path(r.path).filename().string(),
                         r.line);
    }
    std::sort(got.begin(), got.end());

    std::vector<std::pair<std::string, int>> want{
        {"a.cpp", 1}, {"a.cpp", 2}, {"a.cpp", 3}, {"b.py", 2}};
    EXPECT_EQ(want, got)
        << "C-family '#' lines are preprocessor directives and must survive; "
           "python '#' is a comment and must be dropped";
}

}  // namespace
}  // namespace lci
