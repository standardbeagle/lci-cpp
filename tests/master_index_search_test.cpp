#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_explore.h>

#include <nlohmann/json.hpp>

#include "unique_temp.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

// -- Temp directory helper (matches master_index_test.cpp pattern) ------------

class TempDir {
  public:
    TempDir() {
        path_ = test::unique_temp_dir("lci_search_test_");
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

// Pins the evicted-but-searchable fix: LRU eviction (simulated here by
// invalidating the content-store entry while the file stays in every other
// index) must not turn matches into silent false negatives — the scan
// reloads the bytes from disk into a request-local buffer.
TEST(MasterIndexSearchIntegrationTest, SearchStillFindsEvictedFileContent) {
    TempDir dir;
    dir.write_file("main.go",
                   "package main\n"
                   "func main() { evictable_needle() }\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto before = mi.search("evictable_needle", 0);
    ASSERT_GE(before.size(), 1u);

    // Simulate LRU eviction: content gone, file still searchable.
    mi.file_content_store().invalidate_file(
        (dir.path() / "main.go").string());
    ASSERT_TRUE(mi.file_content_store()
                    .get_content(before[0].file_id)
                    .empty());

    auto after = mi.search("evictable_needle", 0);
    ASSERT_GE(after.size(), 1u)
        << "eviction silently dropped a searchable file's matches";
    EXPECT_EQ(after[0].line, before[0].line);
}

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

TEST(MasterIndexSearchIntegrationTest, PathScopesNarrowToFileAndDir) {
    // Same pattern lives in three files across two directories. Path scoping
    // (the `lci grep pattern <path>...` positional) must narrow index-side.
    TempDir dir;
    dir.write_file("pkg/a.go", "package pkg\nvar needle = 1\n");
    dir.write_file("pkg/sub/b.go", "package sub\nvar needle = 2\n");
    dir.write_file("other/c.go", "package other\nvar needle = 3\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto path_of = [](const std::vector<SearchResult>& rs) {
        std::vector<std::string> ps;
        for (const auto& r : rs) ps.push_back(r.path);
        return ps;
    };
    auto has_suffix = [](const std::string& p, const std::string& suf) {
        return p.size() >= suf.size() &&
               p.compare(p.size() - suf.size(), suf.size(), suf) == 0;
    };

    // Baseline: no scope -> all three files hit.
    auto all_hits = mi.search_with_options("needle", SearchOptions{});
    ASSERT_EQ(all_hits.size(), 3u);

    // Exact file scope -> only that file.
    SearchOptions file_opts;
    file_opts.path_scopes = {"pkg/a.go"};
    auto file_hits = mi.search_with_options("needle", file_opts);
    ASSERT_EQ(file_hits.size(), 1u);
    EXPECT_TRUE(has_suffix(file_hits.front().path, "pkg/a.go"));

    // Directory-prefix scope -> both files under pkg/ (incl. pkg/sub), none
    // under other/.
    SearchOptions dir_opts;
    dir_opts.path_scopes = {"pkg"};
    auto dir_hits = mi.search_with_options("needle", dir_opts);
    ASSERT_EQ(dir_hits.size(), 2u);
    for (const auto& p : path_of(dir_hits)) {
        EXPECT_EQ(p.find("/other/"), std::string::npos) << p;
    }
}

TEST(MasterIndexSearchIntegrationTest, ScopesWithoutIndexedMatchFlagsUnindexedPath) {
    // Fail-fast regression (blocker 1): a path that exists on disk but was
    // never indexed must be detected via INDEX membership, not a bare
    // std::filesystem::exists check. Without the membership guard the
    // path-scope filter would empty the candidate set and exit 0 (silent
    // empty). scopes_without_indexed_match reports the offending token.
    TempDir dir;
    dir.write_file("pkg/a.go", "package pkg\nvar needle = 1\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    // Exists on disk but NOT in the index (created after indexing — stands in
    // for a gitignored / wrong-extension / outside-root file that a plain
    // exists() check would wrongly accept).
    dir.write_file("pkg/ghost.go", "package pkg\nvar needle = 9\n");

    // Indexed file and directory scopes match at least one indexed file.
    EXPECT_TRUE(mi.scopes_without_indexed_match({"pkg/a.go"}).empty());
    EXPECT_TRUE(mi.scopes_without_indexed_match({"pkg"}).empty());

    // The on-disk-but-unindexed file matches no indexed file -> flagged.
    auto unmatched = mi.scopes_without_indexed_match({"pkg/ghost.go"});
    ASSERT_EQ(unmatched.size(), 1u);
    EXPECT_EQ(unmatched.front(), "pkg/ghost.go");

    // A wholly unknown directory is flagged; a mixed set reports only the bad
    // entry so the error can name exactly what did not resolve.
    auto mixed = mi.scopes_without_indexed_match({"pkg/a.go", "nope/dir"});
    ASSERT_EQ(mixed.size(), 1u);
    EXPECT_EQ(mixed.front(), "nope/dir");
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

TEST(MasterIndexSearchIntegrationTest,
     CaseInsensitiveFindsDifferentlyCasedFiles) {
    // Discrimination pair: the trigram index stores original-case content,
    // so a lowered pattern's trigrams only hit a.go. The non-empty trigram
    // candidate set used to suppress the postings union, silently dropping
    // b.go (whose only occurrence is "FooBar").
    TempDir dir;
    dir.write_file("a.go", "package main\nvar x = \"foobar\"\n");
    dir.write_file("b.go", "package main\nvar y = \"FooBar\"\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    // Incremental single-file indexing populates the trigram snapshot with
    // original-case content (the bulk pipeline path leaves it empty), which
    // is exactly the state where the lowered pattern's trigram candidates
    // exclude b.go.
    ASSERT_TRUE(mi.index_file((dir.path() / "a.go").string()));
    ASSERT_TRUE(mi.index_file((dir.path() / "b.go").string()));

    SearchOptions opts;
    opts.case_insensitive = true;
    auto results = mi.search_with_options("foobar", opts);

    std::set<std::string> hit_files;
    for (const auto& r : results) {
        hit_files.insert(std::filesystem::path(r.path).filename().string());
    }
    EXPECT_TRUE(hit_files.contains("a.go"));
    EXPECT_TRUE(hit_files.contains("b.go"));

    // Case-sensitive queries with uppercase letters must still prefilter
    // through the (lowercased) postings tokens and land on the exact match.
    SearchOptions exact;
    exact.case_insensitive = false;
    auto upper = mi.search_with_options("FooBar", exact);
    ASSERT_EQ(upper.size(), 1u);
    EXPECT_EQ(std::filesystem::path(upper[0].path).filename().string(),
              "b.go");
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
                auto snap = mi.load_snapshot();
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
// The def/refs path is now fully lock-free: ReferenceTracker is RCU and
// execute_search pins a snapshot (01KSWHQ742), so the raw EnhancedSymbol* and
// content view stay valid across a concurrent reindex with no lock. This test
// is the gate proving that — genuinely TSan-clean (the IndexLockManager that
// once guarded this path has been retired entirely).
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

// Exercises the MCP-handler read pattern post-RCU: pin the ReferenceTracker
// snapshot, then dereference the raw const EnhancedSymbol* returned by the
// pointer accessors (find_symbol_by_name / get_enhanced_symbol /
// get_file_enhanced_symbols) while a concurrent reindex publishes new
// snapshots. The pin must keep those pointers valid (no use-after-free, no torn
// field reads) — this is the gate for task 01KV1QHTS8 (handlers migrated from
// bare-accessor to pin()->accessor). Without the pin, tsan/ASan would flag a
// dangling read when a publish frees the snapshot the pointer aliases.
TEST(MasterIndexSearchIntegrationTest, ConcurrentHandlerReadsDuringIndexing) {
    TempDir dir;
    dir.write_file("stable.go",
        "package main\n"
        "func handlerStableToken() { return }\n");
    dir.write_file("churn.go",
        "package main\n"
        "func churn() { return }\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    const ReferenceTracker& rt = mi.ref_tracker();
    ASSERT_NE(rt.pin()->find_symbol_by_name("handlerStableToken"), nullptr);

    constexpr int kReaders = 4;
    constexpr int kReadsPerThread = 200;
    std::atomic<bool> stop{false};
    std::atomic<int> missing{0};
    std::atomic<int> torn{0};
    std::vector<std::thread> readers;

    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < kReadsPerThread && !stop.load(); ++j) {
                // Handler pattern: pin once, dereference pointers under the pin.
                auto snap = rt.pin();
                auto s = snap->find_symbol_by_name("handlerStableToken");
                if (s == nullptr) {
                    missing.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                // Deref fields of the pinned pointer (would UAF without the pin).
                if (s->symbol.name != "handlerStableToken") {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                auto by_id = snap->get_enhanced_symbol(s->id);
                if (by_id == nullptr || by_id->symbol.name != s->symbol.name) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                for (const auto& fs :
                     snap->get_file_enhanced_symbols(s->symbol.file_id)) {
                    if (fs->symbol.name.empty()) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    std::thread writer([&] {
        for (int i = 0; i < 20; ++i) {
            std::string content = "package main\nfunc churn" +
                                  std::to_string(i) + "() { return }\n";
            mi.update_file((dir.path() / "churn.go").string(), content);
        }
        stop.store(true, std::memory_order_release);
    });

    writer.join();
    for (auto& t : readers) t.join();

    EXPECT_EQ(missing.load(), 0)
        << "pinned handler read lost the stable symbol during a concurrent "
           "reindex of an unrelated file";
    EXPECT_EQ(torn.load(), 0)
        << "pinned handler read observed a torn/freed EnhancedSymbol";
}

// Pagination under index churn: readers walk random page windows through the
// real MCP handler while a writer keeps invalidating files (update_file adds
// and removes symbols). Each individual response must stay self-consistent —
// showing == |symbols|, showing <= total, has_more <-> offset+showing < total
// against ITS OWN total — even though totals drift between responses. This is
// the recovery contract: a client that pages while the index rebuilds sees
// coherent windows, never torn arithmetic.
TEST(MasterIndexSearchIntegrationTest, PaginationCoherentUnderIndexChurn) {
    TempDir dir;
    for (int f = 0; f < 8; ++f) {
        std::string content = "package main\n";
        for (int s = 0; s < 6; ++s) {
            content += "func base" + std::to_string(f) + "_" +
                       std::to_string(s) + "() { return }\n";
        }
        dir.write_file("f" + std::to_string(f) + ".go", content);
    }

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    constexpr int kReaders = 3;
    constexpr int kReadsPerThread = 150;
    std::atomic<bool> stop{false};
    std::atomic<int> incoherent{0};
    std::atomic<int> reads_done{0};
    std::vector<std::thread> readers;

    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&, r] {
            std::mt19937 rng(1234u + static_cast<unsigned>(r));
            std::uniform_int_distribution<int> off_dist(-5, 80);
            std::uniform_int_distribution<int> max_dist(-1, 12);
            for (int j = 0; j < kReadsPerThread; ++j) {
                nlohmann::json params = {{"max", max_dist(rng)},
                                         {"offset", off_dist(rng)}};
                auto result = mcp::handle_list_symbols(params, mi);
                if (result.is_error) {
                    incoherent.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                auto j2 = nlohmann::json::parse(result.text, nullptr,
                                                /*allow_exceptions=*/false);
                if (j2.is_discarded() || !j2.contains("total") ||
                    !j2.contains("showing") || !j2.contains("has_more") ||
                    !j2["symbols"].is_array()) {
                    incoherent.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const int total = j2["total"].get<int>();
                const int showing = j2["showing"].get<int>();
                const int listed = static_cast<int>(j2["symbols"].size());
                const int offset = std::max(0, params["offset"].get<int>());
                const bool more = j2["has_more"].get<bool>();
                if (showing != listed || showing > total ||
                    more != (total > offset + showing)) {
                    incoherent.fetch_add(1, std::memory_order_relaxed);
                }
                reads_done.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread writer([&] {
        int i = 0;
        while (!stop.load(std::memory_order_acquire)) {
            // Invalidate a file with a different symbol count each pass so
            // total keeps moving while readers page.
            const int f = i % 8;
            std::string content = "package main\n";
            const int symbols = 1 + (i % 9);
            for (int s = 0; s < symbols; ++s) {
                content += "func churn" + std::to_string(f) + "_" +
                           std::to_string(i) + "_" + std::to_string(s) +
                           "() { return }\n";
            }
            mi.update_file((dir.path() / ("f" + std::to_string(f) + ".go"))
                               .string(),
                           content);
            ++i;
        }
    });

    for (auto& t : readers) t.join();
    stop.store(true, std::memory_order_release);
    writer.join();

    EXPECT_EQ(incoherent.load(), 0)
        << "a paged response contradicted its own total/showing/has_more "
           "while the index was being invalidated underneath it";
    EXPECT_EQ(reads_done.load(), kReaders * kReadsPerThread);
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

// -- Bulk-index candidate certification (silent-zero regressions) -------------
//
// After index_directory (the bulk pipeline), the trigram snapshot holds no
// per-file trigram data and PostingsIndex holds whole lowercase tokens only.
// Any literal pattern that is not an exact lowercase postings token — a phrase
// containing a space, a mixed-case identifier searched case-sensitively, a
// substring of a longer identifier — must still be found by the verify scan.
// A residue candidate set (postings-PARTIAL files that self-nominate on every
// lookup) must never suppress scanning the rest of the corpus: a non-empty
// residue set is not a narrowing.

namespace {

/// Writes a .go file whose unique-token count exceeds the code-file postings
/// cap for the given config, so the bulk pipeline records it PARTIAL and it
/// self-nominates in every postings lookup. This is the residue shape that
/// suppressed the scan-all fallback in production (voc.txt et al.).
void write_partial_residue_file(TempDir& dir, const Config& cfg,
                                const std::string& rel) {
    const int code_cap = cfg.index.data_file_token_cap * 4;
    std::string blob = "package residue\n// ";
    for (int i = 0; i < code_cap + 50; ++i) {
        blob += "tokres" + std::to_string(i) + " ";
    }
    blob += "\n";
    dir.write_file(rel, blob);
}

/// True when some result's path ends with `suffix`.
bool any_result_in(const std::vector<SearchResult>& results,
                   const std::string& suffix) {
    return std::any_of(results.begin(), results.end(),
                       [&](const SearchResult& r) {
                           return r.path.size() >= suffix.size() &&
                                  r.path.compare(r.path.size() - suffix.size(),
                                                 suffix.size(), suffix) == 0;
                       });
}

}  // namespace

TEST(MasterIndexSearchIntegrationTest,
     BulkPhraseWithSpaceFoundDespitePartialResidue) {
    TempDir dir;
    dir.write_file("a.go", "package main\n// the Index server exits here\n");

    Config cfg = make_default_config();
    cfg.index.data_file_token_cap = 25;
    write_partial_residue_file(dir, cfg, "residue.go");
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));
    ASSERT_GE(mi.postings_index().partial_file_count(), 1)
        << "residue file must be PARTIAL for this test to reproduce the bug";

    auto results = mi.search_with_options("Index server", SearchOptions{});
    EXPECT_TRUE(any_result_in(results, "a.go"))
        << "phrase with a space must be found by the verify scan";
}

TEST(MasterIndexSearchIntegrationTest,
     BulkCaseSensitiveMixedCaseTokenFound) {
    TempDir dir;
    dir.write_file("a.go", "package main\ntype PageWindow struct{}\n");

    Config cfg = make_default_config();
    cfg.index.data_file_token_cap = 25;
    write_partial_residue_file(dir, cfg, "residue.go");
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));
    ASSERT_GE(mi.postings_index().partial_file_count(), 1);

    // Postings tokens are stored lowercase; a case-sensitive query for the
    // mixed-case identifier must still reach the verify scan.
    auto results = mi.search_with_options("PageWindow", SearchOptions{});
    EXPECT_TRUE(any_result_in(results, "a.go"));
}

TEST(MasterIndexSearchIntegrationTest, BulkSubstringOfTokenFound) {
    TempDir dir;
    dir.write_file("a.go", "package main\n// repagination counter\n");

    Config cfg = make_default_config();
    cfg.index.data_file_token_cap = 25;
    write_partial_residue_file(dir, cfg, "residue.go");
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));
    ASSERT_GE(mi.postings_index().partial_file_count(), 1);

    // "pagination" is a strict substring of the stored token "repagination".
    auto results = mi.search_with_options("pagination", SearchOptions{});
    EXPECT_TRUE(any_result_in(results, "a.go"));
}

// -- Punctuation/operator patterns (silent-zero pins) -------------------------
//
// The tokenizer strips punctuation when building postings, and trigram
// windows skip no-alnum runs — a naive narrowing over either index could
// certify `.dump(` or `catch (...)` absent everywhere and return a silent
// zero. Pin the contract: zero results must mean the bytes are truly absent,
// never that the tokenizer dropped the pattern's punctuation.

TEST(MasterIndexSearchIntegrationTest, BulkPunctuationCallPatternFound) {
    TempDir dir;
    dir.write_file("a.cpp",
                   "int main() {\n  payload.dump(2);\n  return 0;\n}\n");
    Config cfg = make_default_config();
    cfg.index.data_file_token_cap = 25;
    write_partial_residue_file(dir, cfg, "residue.go");
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto results = mi.search_with_options(".dump(", SearchOptions{});
    EXPECT_TRUE(any_result_in(results, "a.cpp"))
        << "punctuation-carrying pattern must reach the verify scan";
}

TEST(MasterIndexSearchIntegrationTest, BulkOperatorOnlyTailPatternFound) {
    TempDir dir;
    dir.write_file("a.cpp",
                   "void f() {\n  try { g(); } catch (...) {}\n}\n");
    Config cfg = make_default_config();
    cfg.index.data_file_token_cap = 25;
    write_partial_residue_file(dir, cfg, "residue.go");
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto results = mi.search_with_options("catch (...)", SearchOptions{});
    EXPECT_TRUE(any_result_in(results, "a.cpp"))
        << "operator-only tail (`(...)`) must not be certified absent";
}

TEST(MasterIndexSearchIntegrationTest, BulkQuoteAndParenPatternFound) {
    TempDir dir;
    dir.write_file("a.cpp",
                   "void h(Json j) {\n  auto p = j.value(\"params\", 0);\n}\n");
    Config cfg = make_default_config();
    cfg.index.data_file_token_cap = 25;
    write_partial_residue_file(dir, cfg, "residue.go");
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto results =
        mi.search_with_options("value(\"params\"", SearchOptions{});
    EXPECT_TRUE(any_result_in(results, "a.cpp"));
}

TEST(MasterIndexSearchIntegrationTest, BulkAbsentPunctuationPatternIsEmpty) {
    // Discrimination pair for the pins above: the same punctuation shape,
    // genuinely absent from the corpus, returns empty.
    TempDir dir;
    dir.write_file("a.cpp",
                   "int main() {\n  payload.dump(2);\n  return 0;\n}\n");
    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto results = mi.search_with_options(".undump(", SearchOptions{});
    EXPECT_TRUE(results.empty());
}

TEST(MasterIndexSearchIntegrationTest,
     BulkBloomCertifiesAbsenceIncludingPartialResidue) {
    // The per-file trigram bloom is built by the bulk pipeline workers and
    // must certify pattern absence even for postings-PARTIAL files — the
    // residue class postings narrowing can never exclude, which made every
    // negative query scan the (large) residue files.
    TempDir dir;
    dir.write_file("a.go", "package main\n// the Index server exits here\n");
    Config cfg = make_default_config();
    cfg.index.data_file_token_cap = 25;
    write_partial_residue_file(dir, cfg, "residue.go");
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));
    ASSERT_GE(mi.postings_index().partial_file_count(), 1);

    auto narrowing =
        mi.trigram_index().narrow("zqx totally_absent vbn", false);
    ASSERT_TRUE(narrowing.informative());
    for (FileID fid : mi.get_all_file_ids()) {
        EXPECT_TRUE(narrowing.certifies_absent(fid))
            << "file " << mi.get_file_path(fid)
            << " should be bloom-certified pattern-free";
    }

    // And presence is never certified away (superset contract).
    auto present = mi.trigram_index().narrow("Index server", false);
    bool a_certified = false;
    for (FileID fid : mi.get_all_file_ids()) {
        if (mi.get_file_path(fid).ends_with("a.go")) {
            a_certified = present.certifies_absent(fid);
        }
    }
    EXPECT_FALSE(a_certified);
}

TEST(MasterIndexSearchIntegrationTest, ManyMatchesInOneFileNotSilentlyCapped) {
    // err-lookup regression shape: a repo file with hundreds of legitimate
    // hits (golang/go-style error tables). The hidden kMaxMatchesPerFile=100
    // cap silently dropped every match past 100 in a file even when the
    // caller's max_results budget had room — `-n 1000000` returned 101 of
    // 1201 real sites. Per-file collection must be bounded by the remaining
    // RESULT budget, not a constant.
    TempDir dir;
    std::string content = "package main\n";
    for (int i = 0; i < 300; ++i) {
        content += "var e" + std::to_string(i) + " = needle(" +
                   std::to_string(i) + ")\n";
    }
    dir.write_file("many.go", content);

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    SearchOptions opts;
    opts.max_results = 1000;
    auto results = mi.search_with_options("needle", opts);
    EXPECT_EQ(results.size(), 300u);
}

TEST(MasterIndexSearchIntegrationTest,
     IncrementalTrigramStateDoesNotHideBulkFiles) {
    TempDir dir;
    dir.write_file("a.go", "package main\n// call handle_gadget now\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    // A later watcher-style single-file index populates the trigram snapshot
    // for THAT file only. Its trigrams overlap the pattern's enough to pass
    // the min_required threshold, so a coverage-blind trigram prefilter
    // would narrow to {c.go} and hide the bulk-indexed a.go.
    dir.write_file("c.go", "package main\n// handle_zebra call\n");
    ASSERT_TRUE(mi.index_file((dir.path() / "c.go").string()));

    auto results = mi.search_with_options("handle_g", SearchOptions{});
    EXPECT_TRUE(any_result_in(results, "a.go"))
        << "trigram data covering only c.go must not certify absence in a.go";
}

}  // namespace
}  // namespace lci
