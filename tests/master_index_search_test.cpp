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
    // stable.go is never modified; its unique token must remain findable
    // through every concurrent write — the core RCU invariant: a reader
    // always observes a consistent snapshot, so committed data published
    // before a write can never transiently disappear or tear.
    dir.write_file("stable.go",
        "package main\n"
        "func alwaysHereStableToken() { return }\n");
    dir.write_file("concurrent.go",
        "package main\n"
        "func concurrent() { return }\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));
    // Precondition: the stable token is findable before any concurrency.
    ASSERT_FALSE(mi.search("alwaysHereStableToken", 0).empty());

    constexpr int kSearcherCount = 4;
    constexpr int kSearchesPerThread = 100;
    std::atomic<bool> stop{false};
    // RCU consistency violations: a search for the never-modified token
    // that comes back empty, or any result with a torn/invalid path.
    std::atomic<int> stable_token_missing{0};
    std::atomic<int> malformed_result{0};
    std::vector<std::thread> searchers;

    for (int i = 0; i < kSearcherCount; ++i) {
        searchers.emplace_back([&] {
            for (int j = 0; j < kSearchesPerThread && !stop.load(); ++j) {
                auto results = mi.search("alwaysHereStableToken", 0);
                if (results.empty()) {
                    stable_token_missing.fetch_add(1, std::memory_order_relaxed);
                }
                for (const auto& r : results) {
                    // A torn snapshot would surface an empty/garbage path.
                    if (r.path.empty()) {
                        malformed_result.fetch_add(1,
                                                   std::memory_order_relaxed);
                    }
                }
                auto ids = mi.get_all_file_ids();
                (void)ids;
            }
        });
    }

    // Concurrent writer churns a *different* file while searches happen.
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

    EXPECT_EQ(stable_token_missing.load(), 0)
        << "RCU read observed a snapshot missing committed stable data "
           "during a concurrent write to an unrelated file";
    EXPECT_EQ(malformed_result.load(), 0)
        << "search returned a result with a torn/empty path under "
           "concurrent indexing";
    // The stable token is still findable after all writes settle.
    EXPECT_FALSE(mi.search("alwaysHereStableToken", 0).empty());
}

TEST(MasterIndexSearchIntegrationTest, ConcurrentSearchReads) {
    TempDir dir;
    dir.write_file("shared.go",
        "package main\nfunc sharedReaderToken() { return }\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    // Establish the expected stable result once, single-threaded.
    auto expected = mi.search("sharedReaderToken", 0);
    ASSERT_FALSE(expected.empty());

    constexpr int kReaderCount = 8;
    constexpr int kReadsPerThread = 200;
    // On a static index, every concurrent reader must observe the exact
    // same result — lock-free reads must be correct, not merely crash-free.
    std::atomic<int> empty_reads{0};
    std::atomic<int> wrong_count{0};
    std::atomic<int> wrong_path{0};
    std::vector<std::thread> readers;

    for (int i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < kReadsPerThread; ++j) {
                auto r1 = mi.search("sharedReaderToken", 0);
                if (r1.empty()) {
                    empty_reads.fetch_add(1, std::memory_order_relaxed);
                }
                if (r1.size() != expected.size()) {
                    wrong_count.fetch_add(1, std::memory_order_relaxed);
                }
                for (const auto& r : r1) {
                    if (r.path.find("shared.go") == std::string::npos) {
                        wrong_path.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                auto snap = mi.read_snapshot();
                (void)snap->file_count();
            }
        });
    }

    for (auto& t : readers) t.join();

    EXPECT_EQ(empty_reads.load(), 0)
        << "lock-free read returned empty on a static index";
    EXPECT_EQ(wrong_count.load(), 0)
        << "lock-free read returned a different result count than the "
           "single-threaded baseline";
    EXPECT_EQ(wrong_path.load(), 0)
        << "lock-free read returned a result outside shared.go";
}

// Exercises the def/refs read path (search_definitions / search_references ->
// ReferenceTracker::find_symbols_by_name -> raw EnhancedSymbol* + content view)
// under a concurrent reindex of an UNRELATED file. The existing concurrent
// tests do text search only and never touch ReferenceTracker, so this is the
// missing coverage for task 01KSWHQ742.
//
// Functionally green with the locks present (def always found, no torn paths).
//
// TSan caveat: under -fsanitize=thread this currently reports FALSE-POSITIVE
// races on the SymbolStore maps. IndexLockManager acquires reads via
// std::shared_timed_mutex::try_lock_shared_for, and TSan does not model the
// timed shared-lock acquisition, so the reader's lock is invisible to it.
// Verified: swapping IndexLockManager to untimed lock_shared()/lock() makes
// this test 0-warning clean, confirming the lock really does serialize today.
// The real fix (01KSWHQ742 phases 2-3) makes the def/refs path lock-free (RCU
// snapshot + pin), after which this is the gate that is genuinely TSan-clean
// with NO lock involved — independent of the timed-mutex instrumentation gap.
TEST(MasterIndexSearchIntegrationTest, ConcurrentDefRefsDuringIndexing) {
    TempDir dir;
    // stable.go is never modified; its definition must remain findable on the
    // def/refs path through every concurrent write to the other file.
    dir.write_file("stable.go",
        "package main\n"
        "func alwaysHereDefRefToken() { return }\n"
        "func callsItOnce() { alwaysHereDefRefToken() }\n");
    dir.write_file("concurrent.go",
        "package main\n"
        "func churned() { return }\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));
    // Precondition: the stable symbol is findable on the def path before any
    // concurrency.
    ASSERT_FALSE(mi.search_definitions("alwaysHereDefRefToken").empty());

    constexpr int kSearcherCount = 4;
    constexpr int kSearchesPerThread = 100;
    std::atomic<bool> stop{false};
    std::atomic<int> stable_def_missing{0};
    std::atomic<int> malformed_result{0};
    std::vector<std::thread> searchers;

    for (int i = 0; i < kSearcherCount; ++i) {
        searchers.emplace_back([&] {
            for (int j = 0; j < kSearchesPerThread && !stop.load(); ++j) {
                auto defs = mi.search_definitions("alwaysHereDefRefToken");
                if (defs.empty()) {
                    stable_def_missing.fetch_add(1, std::memory_order_relaxed);
                }
                for (const auto& r : defs) {
                    if (r.path.empty()) {
                        malformed_result.fetch_add(1,
                                                   std::memory_order_relaxed);
                    }
                }
                // usage_only path: dereferences ref pointers + scans content.
                auto refs = mi.search_references("alwaysHereDefRefToken");
                for (const auto& r : refs) {
                    if (r.path.empty()) {
                        malformed_result.fetch_add(1,
                                                   std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    std::thread writer([&] {
        for (int i = 0; i < 20; ++i) {
            std::string content = "package main\nfunc churned" +
                                  std::to_string(i) + "() { return }\n";
            std::string path = (dir.path() / "concurrent.go").string();
            mi.update_file(path, content);
        }
        stop.store(true, std::memory_order_release);
    });

    writer.join();
    for (auto& t : searchers) t.join();

    EXPECT_EQ(stable_def_missing.load(), 0)
        << "def/refs read observed a snapshot missing committed stable symbol "
           "during a concurrent reindex of an unrelated file";
    EXPECT_EQ(malformed_result.load(), 0)
        << "def/refs search returned a result with a torn/empty path under "
           "concurrent indexing";
    EXPECT_FALSE(mi.search_definitions("alwaysHereDefRefToken").empty());
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
