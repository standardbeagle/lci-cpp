#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

// -- Temp directory helper (matches master_index_test.cpp pattern) ------------

class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lci_search_test_" + std::to_string(
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

// -- Search validation tests --------------------------------------------------

TEST(MasterIndexSearchTest, SearchEmptyPattern) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    auto results = mi.search("", 0);
    EXPECT_TRUE(results.empty());
}

TEST(MasterIndexSearchTest, SearchOnEmptyIndex) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    auto results = mi.search("hello", 0);
    EXPECT_TRUE(results.empty());
}

TEST(MasterIndexSearchTest, SearchWithOptionsEmptyPattern) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    SearchOptions opts;
    auto results = mi.search_with_options("", opts);
    EXPECT_TRUE(results.empty());
}

TEST(MasterIndexSearchTest, SearchWithOptionsPatternTooLong) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    std::string long_pattern(1001, 'x');
    auto results = mi.search("hello", 0);
    (void)long_pattern;
    auto results2 = mi.search_with_options(long_pattern, SearchOptions{});
    EXPECT_TRUE(results2.empty());
}

TEST(MasterIndexSearchTest, SearchWithOptionsNegativeMaxResults) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    SearchOptions opts;
    opts.max_results = -1;
    auto results = mi.search_with_options("hello", opts);
    EXPECT_TRUE(results.empty());
}

// -- Find candidate files tests -----------------------------------------------

TEST(MasterIndexSearchTest, FindCandidateFilesEmptyIndex) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    auto ids = mi.find_candidate_files("func", false);
    EXPECT_TRUE(ids.empty());
}

// -- Search definitions / references on empty index ---------------------------

TEST(MasterIndexSearchTest, SearchDefinitionsEmptyIndex) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    auto results = mi.search_definitions("foo");
    EXPECT_TRUE(results.empty());
}

TEST(MasterIndexSearchTest, SearchReferencesEmptyIndex) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    auto results = mi.search_references("bar");
    EXPECT_TRUE(results.empty());
}

// -- get_file_path / get_all_file_ids -----------------------------------------

TEST(MasterIndexSearchTest, GetFilePathNotFound) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_TRUE(mi.get_file_path(FileID{999}).empty());
}

TEST(MasterIndexSearchTest, GetAllFileIdsEmpty) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    auto ids = mi.get_all_file_ids();
    EXPECT_TRUE(ids.empty());
}

// -- Integration tests: index files and search --------------------------------

TEST(MasterIndexSearchIntegrationTest, IndexAndSearchText) {
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
    EXPECT_GE(mi.file_count(), 2);

    // Search for text that appears in both files.
    auto results = mi.search("hello", 0);
    EXPECT_GE(results.size(), 1u);

    // Verify results have valid paths and line numbers.
    for (const auto& r : results) {
        EXPECT_NE(FileID{0}, r.file_id);
        EXPECT_FALSE(r.path.empty());
        EXPECT_GT(r.line, 0);
    }
}

TEST(MasterIndexSearchIntegrationTest, IndexAndSearchWithContext) {
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

    auto results = mi.search("message", 1);
    EXPECT_GE(results.size(), 1u);

    // At least one result should have context lines.
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

TEST(MasterIndexSearchIntegrationTest, IndexAndSearchCaseInsensitive) {
    TempDir dir;
    dir.write_file("case.js",
        "function HelloWorld() {\n"
        "    return 'helloworld';\n"
        "}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchOptions opts;
    opts.case_insensitive = true;
    auto results = mi.search_with_options("helloworld", opts);
    EXPECT_GE(results.size(), 1u);
}

TEST(MasterIndexSearchIntegrationTest, FindCandidateFiles) {
    TempDir dir;
    dir.write_file("a.go", "package main\nfunc doStuff() {}\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    // Use single-file indexing which populates trigrams directly.
    std::string file_path = (dir.path() / "a.go").string();
    ASSERT_TRUE(mi.index_file(file_path));

    auto candidates = mi.find_candidate_files("package", false);
    EXPECT_GE(candidates.size(), 1u);
}

TEST(MasterIndexSearchIntegrationTest, SearchMaxResultsLimit) {
    TempDir dir;
    // Create a file with many occurrences of "item".
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

    SearchOptions opts;
    opts.max_results = 5;
    auto results = mi.search_with_options("item", opts);
    EXPECT_LE(static_cast<int>(results.size()), 5);
}

TEST(MasterIndexSearchIntegrationTest, GetAllFileIds) {
    TempDir dir;
    dir.write_file("one.go", "package main\n");
    dir.write_file("two.go", "package main\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto ids = mi.get_all_file_ids();
    EXPECT_GE(static_cast<int>(ids.size()), 2);
}

TEST(MasterIndexSearchIntegrationTest, GetFilePath) {
    TempDir dir;
    dir.write_file("lookup.go", "package main\nfunc lookup() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto ids = mi.get_all_file_ids();
    ASSERT_FALSE(ids.empty());
    for (FileID fid : ids) {
        std::string path = mi.get_file_path(fid);
        EXPECT_FALSE(path.empty());
    }
}

// -- Concurrent search tests --------------------------------------------------

TEST(MasterIndexSearchIntegrationTest, ConcurrentSearchDuringIndexing) {
    TempDir dir;
    dir.write_file("concurrent.go",
        "package main\n"
        "func concurrent() { return }\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    constexpr int kSearcherCount = 4;
    constexpr int kSearchesPerThread = 100;
    std::atomic<bool> stop{false};
    std::vector<std::thread> searchers;

    for (int i = 0; i < kSearcherCount; ++i) {
        searchers.emplace_back([&] {
            for (int j = 0; j < kSearchesPerThread && !stop.load(); ++j) {
                auto results = mi.search("concurrent", 0);
                (void)results;
                auto ids = mi.get_all_file_ids();
                (void)ids;
            }
        });
    }

    // Concurrent writer updates a file while searches happen.
    std::thread writer([&] {
        for (int i = 0; i < 20; ++i) {
            std::string content = "package main\nvar v" +
                                  std::to_string(i) + " = " +
                                  std::to_string(i) + "\n";
            std::string path = (dir.path() / "concurrent.go").string();
            mi.update_file(path, content);
        }
        stop.store(true, std::memory_order_release);
    });

    writer.join();
    for (auto& t : searchers) t.join();
}

TEST(MasterIndexSearchIntegrationTest, ConcurrentSearchReads) {
    TempDir dir;
    dir.write_file("shared.go",
        "package main\nfunc shared() { return }\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    constexpr int kReaderCount = 8;
    constexpr int kReadsPerThread = 200;
    std::vector<std::thread> readers;

    for (int i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < kReadsPerThread; ++j) {
                auto r1 = mi.search("shared", 0);
                (void)r1;
                auto r2 = mi.find_candidate_files("shared", false);
                (void)r2;
                auto snap = mi.read_snapshot();
                (void)snap->file_count();
            }
        });
    }

    for (auto& t : readers) t.join();
}

// -- Search after single-file indexing ----------------------------------------

TEST(MasterIndexSearchIntegrationTest, SearchAfterSingleFileIndex) {
    TempDir dir;
    dir.write_file("single.go",
        "package main\nfunc singleSearch() { return }\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "single.go").string();
    ASSERT_TRUE(mi.index_file(file_path));

    auto results = mi.search("singleSearch", 0);
    EXPECT_GE(results.size(), 1u);
    if (!results.empty()) {
        EXPECT_EQ(file_path, results[0].path);
    }
}

TEST(MasterIndexSearchIntegrationTest, SearchAfterFileUpdate) {
    TempDir dir;
    dir.write_file("updated.go", "package main\nvar original = 1\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "updated.go").string();
    ASSERT_TRUE(mi.index_file(file_path));

    // Should find "original".
    auto r1 = mi.search("original", 0);
    EXPECT_GE(r1.size(), 1u);

    // Update the file with new content.
    std::string new_content = "package main\nvar replacement = 2\n";
    ASSERT_TRUE(mi.update_file(file_path, new_content));

    // Should find "replacement".
    auto r2 = mi.search("replacement", 0);
    EXPECT_GE(r2.size(), 1u);
}

TEST(MasterIndexSearchIntegrationTest, SearchAfterFileRemoval) {
    TempDir dir;
    dir.write_file("removable.go", "package main\nfunc removable() {}\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "removable.go").string();
    ASSERT_TRUE(mi.index_file(file_path));

    auto r1 = mi.search("removable", 0);
    EXPECT_GE(r1.size(), 1u);

    ASSERT_TRUE(mi.remove_file(file_path));

    // After removal, index is empty so search returns nothing.
    auto r2 = mi.search("removable", 0);
    EXPECT_TRUE(r2.empty());
}

}  // namespace
}  // namespace lci
