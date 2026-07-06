#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>

#include "unique_temp.h"

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

}  // namespace
}  // namespace lci
