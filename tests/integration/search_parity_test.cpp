#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace lci {
namespace {

// TempDir matches the pattern used by existing passing tests.
class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lci_parity_test_" + std::to_string(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
                    std::hash<int>{}(counter_++)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = default;
    TempDir& operator=(TempDir&&) = default;

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
    static inline int counter_ = 0;
};

// SearchParityTest verifies that the C++ search engine produces results
// consistent with the Go implementation for baseline queries. Each test
// creates a minimal fixture to ensure deterministic indexing results.

// -- Helper: index a single Go file and return a SearchEngine ----------------

struct IndexedProject {
    TempDir dir;
    std::unique_ptr<MasterIndex> mi;
    std::unique_ptr<SearchEngine> engine;
};

IndexedProject make_indexed(const std::string& file,
                            const std::string& content) {
    IndexedProject p;
    p.dir.write_file(file, content);
    Config cfg = make_default_config();
    cfg.project.root = p.dir.path().string();
    p.mi = std::make_unique<MasterIndex>(cfg);
    EXPECT_TRUE(p.mi->index_directory(p.dir.path().string()));
    p.engine = std::make_unique<SearchEngine>(*p.mi);
    return p;
}

IndexedProject make_indexed_two(const std::string& file1,
                                const std::string& content1,
                                const std::string& file2,
                                const std::string& content2) {
    IndexedProject p;
    p.dir.write_file(file1, content1);
    p.dir.write_file(file2, content2);
    Config cfg = make_default_config();
    cfg.project.root = p.dir.path().string();
    p.mi = std::make_unique<MasterIndex>(cfg);
    EXPECT_TRUE(p.mi->index_directory(p.dir.path().string()));
    p.engine = std::make_unique<SearchEngine>(*p.mi);
    return p;
}

// -- Baseline query: exact function name match --------------------------------

TEST(SearchParityTest, FindsFunctionByExactName) {
    auto proj = make_indexed("main.go",
        "package main\n"
        "\n"
        "func Add(a, b int) int {\n"
        "    return a + b\n"
        "}\n");

    SearchOptions opts;
    auto results = proj.engine->search("Add", opts);
    ASSERT_GE(results.size(), 1u);

    bool found = false;
    for (const auto& r : results) {
        if (r.path.find("main.go") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected to find Add in main.go";
}

// -- Baseline query: struct name match ----------------------------------------

TEST(SearchParityTest, FindsStructByName) {
    auto proj = make_indexed("main.go",
        "package main\n"
        "\n"
        "type Calculator struct {\n"
        "    Value int\n"
        "}\n"
        "\n"
        "func (c *Calculator) Reset() {\n"
        "    c.Value = 0\n"
        "}\n");

    SearchOptions opts;
    auto results = proj.engine->search("Calculator", opts);
    ASSERT_GE(results.size(), 1u);

    bool found = false;
    for (const auto& r : results) {
        if (r.path.find("main.go") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected to find Calculator in main.go";
}

// -- Baseline query: method name match ----------------------------------------

TEST(SearchParityTest, FindsMethodByName) {
    auto proj = make_indexed("main.go",
        "package main\n"
        "\n"
        "type Calculator struct {\n"
        "    Value int\n"
        "}\n"
        "\n"
        "func (c *Calculator) Reset() {\n"
        "    c.Value = 0\n"
        "}\n");

    SearchOptions opts;
    auto results = proj.engine->search("Reset", opts);
    ASSERT_GE(results.size(), 1u);
}

// -- Baseline query: cross-file search ----------------------------------------

TEST(SearchParityTest, FindsResultsAcrossFiles) {
    auto proj = make_indexed_two(
        "main.go",
        "package main\n\nfunc main() {}\n",
        "handler.go",
        "package main\n"
        "\n"
        "func handleRequest() string {\n"
        "    return \"ok\"\n"
        "}\n");

    SearchOptions opts;
    auto results = proj.engine->search("handleRequest", opts);
    ASSERT_GE(results.size(), 1u);

    bool found = false;
    for (const auto& r : results) {
        if (r.path.find("handler.go") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected results from handler.go";
}

// -- Baseline query: case-insensitive search ----------------------------------

TEST(SearchParityTest, CaseInsensitiveSearchFindsMatches) {
    auto proj = make_indexed("main.go",
        "package main\n"
        "\n"
        "type Calculator struct{}\n");

    SearchOptions opts;
    opts.case_insensitive = true;
    auto results = proj.engine->search("calculator", opts);
    ASSERT_GE(results.size(), 1u);
}

// -- Baseline query: word boundary search -------------------------------------

TEST(SearchParityTest, WordBoundarySearchExcludesPartials) {
    auto proj = make_indexed("main.go",
        "package main\n"
        "\n"
        "func Add(a, b int) int { return a + b }\n"
        "func AddValue(a int) int { return a }\n"
        "var foo = Add(1, 2)\n");

    SearchOptions opts;
    opts.word_boundary = true;
    auto results = proj.engine->search("Add", opts);
    ASSERT_GE(results.size(), 1u);

    for (const auto& r : results) {
        if (r.match_text == "AddValue") {
            FAIL() << "Word boundary search should not match 'AddValue'";
        }
    }
}

// -- Baseline query: max results limit ----------------------------------------

TEST(SearchParityTest, MaxResultsRespected) {
    std::string content = "package main\n";
    for (int i = 0; i < 20; ++i) {
        content += "var item" + std::to_string(i) + " = " +
                   std::to_string(i) + "\n";
    }
    auto proj = make_indexed("main.go", content);

    SearchOptions opts;
    opts.max_results = 3;
    auto results = proj.engine->search("item", opts);
    EXPECT_LE(static_cast<int>(results.size()), 3);
}

// -- Baseline query: empty pattern --------------------------------------------

TEST(SearchParityTest, EmptyPatternReturnsNoResults) {
    auto proj = make_indexed("main.go",
        "package main\nfunc main() {}\n");

    SearchOptions opts;
    auto results = proj.engine->search("", opts);
    EXPECT_TRUE(results.empty());
}

// -- Baseline query: short pattern handles gracefully -------------------------

TEST(SearchParityTest, ShortPatternHandledGracefully) {
    auto proj = make_indexed("main.go",
        "package main\nfunc main() {}\n");

    SearchOptions opts;
    auto r1 = proj.engine->search("a", opts);
    (void)r1;
    auto r2 = proj.engine->search("ab", opts);
    (void)r2;
}

// -- Baseline query: results are ranked (code > docs) -------------------------

TEST(SearchParityTest, CodeFilesRankedAboveDocFiles) {
    auto proj = make_indexed_two(
        "main.go",
        "package main\n\ntype Calculator struct{}\n",
        "readme.md",
        "# Calculator\nThe Calculator struct.\n");

    SearchOptions opts;
    auto results = proj.engine->search("Calculator", opts);
    ASSERT_GE(results.size(), 2u);

    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_GE(results[i - 1].score, results[i].score);
    }

    bool code_first = false;
    for (const auto& r : results) {
        if (r.path.ends_with(".go")) {
            code_first = true;
            break;
        }
        if (r.path.ends_with(".md")) break;
    }
    EXPECT_TRUE(code_first) << "Code files should rank above doc files";
}

// -- Baseline query: context lines --------------------------------------------

TEST(SearchParityTest, SearchWithContextReturnsLines) {
    auto proj = make_indexed("handler.go",
        "package main\n"
        "\n"
        "func handleRequest() {\n"
        "    println(\"handling\")\n"
        "}\n");

    SearchOptions opts;
    opts.max_context_lines = 2;
    auto results = proj.engine->search("handleRequest", opts);
    ASSERT_GE(results.size(), 1u);

    bool has_context = false;
    for (const auto& r : results) {
        if (!r.context.lines.empty()) {
            has_context = true;
            EXPECT_GT(r.context.start_line, 0);
            EXPECT_GE(r.context.end_line, r.context.start_line);
            break;
        }
    }
    EXPECT_TRUE(has_context) << "Search with context should return lines";
}

// -- Baseline query: type in subdirectory -------------------------------------

TEST(SearchParityTest, FindsTypeInSubdirectory) {
    auto proj = make_indexed("models/user.go",
        "package models\n"
        "\n"
        "type User struct {\n"
        "    ID   int\n"
        "    Name string\n"
        "}\n");

    SearchOptions opts;
    auto results = proj.engine->search("User", opts);
    ASSERT_GE(results.size(), 1u);

    bool found = false;
    for (const auto& r : results) {
        if (r.path.find("models") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected to find User in models/user.go";
}

// -- Baseline query: long pattern matches exactly -----------------------------

TEST(SearchParityTest, LongPatternMatchesExactly) {
    auto proj = make_indexed("main.go",
        "package main\n"
        "\n"
        "func handleRequest() string {\n"
        "    return \"ok\"\n"
        "}\n");

    SearchOptions opts;
    auto results = proj.engine->search("handleRequest", opts);
    ASSERT_GE(results.size(), 1u);
    EXPECT_EQ(results[0].path.find("main.go") != std::string::npos, true);
}

}  // namespace
}  // namespace lci
