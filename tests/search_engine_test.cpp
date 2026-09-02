#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>

#include "unique_temp.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

TEST(SearchPureFunctions, FindLiteralOccurrences) {
    auto hits = find_literal_occurrences("abcabc", "abc");
    ASSERT_EQ(2u, hits.size());
    EXPECT_EQ(0, hits[0]);
    EXPECT_EQ(3, hits[1]);
}

TEST(SearchPureFunctions, FindLiteralOccurrencesEmpty) {
    EXPECT_TRUE(find_literal_occurrences("", "abc").empty());
    EXPECT_TRUE(find_literal_occurrences("abc", "").empty());
}

TEST(SearchPureFunctions, FindLiteralOccurrencesCaseInsensitive) {
    auto hits = find_literal_occurrences_ci("AbCaBc", "abc");
    ASSERT_EQ(2u, hits.size());
    EXPECT_EQ(0, hits[0]);
    EXPECT_EQ(3, hits[1]);
}

TEST(SearchPureFunctions, FindWholeWordOccurrences) {
    auto hits = find_whole_word_occurrences("foo bar foo_bar foo", "foo");
    ASSERT_EQ(2u, hits.size());
    EXPECT_EQ(0, hits[0]);
    EXPECT_EQ(16, hits[1]);
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

}  // namespace
}  // namespace lci
