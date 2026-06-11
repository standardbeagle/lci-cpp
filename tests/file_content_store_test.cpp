#include <gtest/gtest.h>

#include <lci/core/file_content_store.h>
#include <lci/core/mmap.h>
#include <lci/core/portable.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// Line offset computation
// ---------------------------------------------------------------------------
TEST(ComputeLineOffsetsTest, EmptyContent) {
    auto offsets = compute_line_offsets("");
    EXPECT_TRUE(offsets.empty());
}

TEST(ComputeLineOffsetsTest, SingleLineNoNewline) {
    auto offsets = compute_line_offsets("hello");
    ASSERT_EQ(offsets.size(), 1u);
    EXPECT_EQ(offsets[0], 0u);
}

TEST(ComputeLineOffsetsTest, SingleLineWithNewline) {
    // Trailing newline does NOT produce a new line entry (matches Go behavior).
    auto offsets = compute_line_offsets("hello\n");
    ASSERT_EQ(offsets.size(), 1u);
    EXPECT_EQ(offsets[0], 0u);
}

TEST(ComputeLineOffsetsTest, MultipleLines) {
    auto offsets = compute_line_offsets("line1\nline2\nline3");
    ASSERT_EQ(offsets.size(), 3u);
    EXPECT_EQ(offsets[0], 0u);
    EXPECT_EQ(offsets[1], 6u);
    EXPECT_EQ(offsets[2], 12u);
}

TEST(ComputeLineOffsetsTest, CRLFHandling) {
    // CRLF: the \r is part of line content, \n triggers the offset.
    auto offsets = compute_line_offsets("line1\r\nline2\r\nline3");
    ASSERT_EQ(offsets.size(), 3u);
    EXPECT_EQ(offsets[0], 0u);
    EXPECT_EQ(offsets[1], 7u);
    EXPECT_EQ(offsets[2], 14u);
}

TEST(ComputeLineOffsetsTest, EmptyLinesBetween) {
    auto offsets = compute_line_offsets("a\n\nb");
    ASSERT_EQ(offsets.size(), 3u);
    EXPECT_EQ(offsets[0], 0u);
    EXPECT_EQ(offsets[1], 2u);
    EXPECT_EQ(offsets[2], 3u);
}

// ---------------------------------------------------------------------------
// FileContentStore - basic operations
// ---------------------------------------------------------------------------
TEST(FileContentStoreTest, LoadAndReadFile) {
    FileContentStore store;
    FileID id = store.load_file("test.go", "package main\n\nfunc main() {}\n");

    EXPECT_NE(id, 0u);
    EXPECT_EQ(store.get_file_count(), 1);

    auto content = store.get_content(id);
    EXPECT_EQ(content, "package main\n\nfunc main() {}\n");
}

TEST(FileContentStoreTest, GetLineOffsets) {
    FileContentStore store;
    FileID id = store.load_file("test.go", "line1\nline2\nline3");

    const auto* offsets = store.get_line_offsets(id);
    ASSERT_NE(offsets, nullptr);
    ASSERT_EQ(offsets->size(), 3u);
    EXPECT_EQ((*offsets)[0], 0u);
    EXPECT_EQ((*offsets)[1], 6u);
    EXPECT_EQ((*offsets)[2], 12u);
}

TEST(FileContentStoreTest, GetLine) {
    FileContentStore store;
    FileID id = store.load_file("test.go", "line1\nline2\nline3");

    auto ref = store.get_line(id, 0);
    EXPECT_EQ(ref.offset, 0u);
    EXPECT_EQ(ref.length, 5u);
    EXPECT_EQ(store.get_string(ref), "line1");

    ref = store.get_line(id, 1);
    EXPECT_EQ(store.get_string(ref), "line2");

    ref = store.get_line(id, 2);
    EXPECT_EQ(store.get_string(ref), "line3");
}

TEST(FileContentStoreTest, GetLineCount) {
    FileContentStore store;
    FileID id = store.load_file("test.go", "a\nb\nc");
    EXPECT_EQ(store.get_line_count(id), 3);
}

TEST(FileContentStoreTest, GetLineOutOfBounds) {
    FileContentStore store;
    FileID id = store.load_file("test.go", "line1\nline2");

    auto ref = store.get_line(id, -1);
    EXPECT_TRUE(ref.is_empty());

    ref = store.get_line(id, 100);
    EXPECT_TRUE(ref.is_empty());
}

TEST(FileContentStoreTest, FastHashConsistency) {
    FileContentStore store;
    FileID id1 = store.load_file("a.go", "hello world");
    FileID id2 = store.load_file("b.go", "hello world");

    EXPECT_EQ(store.get_fast_hash(id1), store.get_fast_hash(id2));
    EXPECT_NE(store.get_fast_hash(id1), 0u);
}

TEST(FileContentStoreTest, ContentHashConsistency) {
    FileContentStore store;
    FileID id1 = store.load_file("a.go", "hello world");
    FileID id2 = store.load_file("b.go", "hello world");

    EXPECT_EQ(store.get_content_hash(id1), store.get_content_hash(id2));
    std::array<uint8_t, 32> zero_hash{};
    EXPECT_NE(store.get_content_hash(id1), zero_hash);
}

TEST(FileContentStoreTest, DuplicateContentNotReloaded) {
    FileContentStore store;
    FileID id1 = store.load_file("test.go", "same content");
    FileID id2 = store.load_file("test.go", "same content");

    // Same file with same content returns same ID.
    EXPECT_EQ(id1, id2);
}

TEST(FileContentStoreTest, UpdatedContentGetsNewHash) {
    FileContentStore store;
    FileID id = store.load_file("test.go", "version1");
    uint64_t hash1 = store.get_fast_hash(id);

    store.load_file("test.go", "version2");
    uint64_t hash2 = store.get_fast_hash(id);

    EXPECT_NE(hash1, hash2);
}

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------
TEST(FileContentStoreTest, InvalidateByPath) {
    FileContentStore store;
    store.load_file("test.go", "content");
    EXPECT_EQ(store.get_file_count(), 1);

    store.invalidate_file("test.go");
    EXPECT_EQ(store.get_file_count(), 0);
}

TEST(FileContentStoreTest, InvalidateByID) {
    FileContentStore store;
    FileID id = store.load_file("test.go", "content");

    store.invalidate_file_by_id(id);
    EXPECT_EQ(store.get_file_count(), 0);
    EXPECT_TRUE(store.get_content(id).empty());
}

TEST(FileContentStoreTest, InvalidateNonExistent) {
    FileContentStore store;
    // Should not crash or error.
    store.invalidate_file("nonexistent.go");
    store.invalidate_file_by_id(999);
}

TEST(FileContentStoreTest, Clear) {
    FileContentStore store;
    store.load_file("a.go", "aaa");
    store.load_file("b.go", "bbb");
    EXPECT_EQ(store.get_file_count(), 2);

    store.clear();
    EXPECT_EQ(store.get_file_count(), 0);
    EXPECT_EQ(store.get_memory_usage(), 0);
}

// ---------------------------------------------------------------------------
// Batch loading
// ---------------------------------------------------------------------------
TEST(FileContentStoreTest, BatchLoad) {
    FileContentStore store;
    std::vector<std::pair<std::string, std::string_view>> files = {
        {"a.go", "aaa"},
        {"b.go", "bbb"},
        {"c.go", "ccc"},
    };

    auto ids = store.batch_load_files(files);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(store.get_file_count(), 3);

    EXPECT_EQ(store.get_content(ids[0]), "aaa");
    EXPECT_EQ(store.get_content(ids[1]), "bbb");
    EXPECT_EQ(store.get_content(ids[2]), "ccc");
}

TEST(FileContentStoreTest, BatchLoadEmpty) {
    FileContentStore store;
    auto ids = store.batch_load_files({});
    EXPECT_TRUE(ids.empty());
}

// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------
TEST(FileContentStoreTest, MemoryUsageTracking) {
    FileContentStore store;
    EXPECT_EQ(store.get_memory_usage(), 0);

    store.load_file("test.go", "some content");
    EXPECT_GT(store.get_memory_usage(), 0);
}

TEST(FileContentStoreTest, MemoryLimitEviction) {
    // 200 bytes limit - should evict older files.
    FileContentStore store(200);

    store.load_file("a.go", std::string(100, 'a'));
    EXPECT_EQ(store.get_file_count(), 1);

    store.load_file("b.go", std::string(100, 'b'));
    // At this point we may be near or over limit due to overhead.

    store.load_file("c.go", std::string(100, 'c'));
    // LRU eviction should have removed oldest files.
    EXPECT_LE(store.get_memory_usage(), 200);
}

TEST(FileContentStoreTest, PathToIdLookup) {
    FileContentStore store;
    FileID id = store.load_file("test.go", "content");
    EXPECT_EQ(store.path_to_id("test.go"), id);
    EXPECT_EQ(store.path_to_id("nonexistent.go"), 0u);
}

// ---------------------------------------------------------------------------
// Concurrent read during write
// ---------------------------------------------------------------------------
TEST(FileContentStoreTest, ConcurrentReadDuringWrite) {
    FileContentStore store;
    store.load_file("initial.go", "initial content");

    constexpr int kReaders = 4;
    constexpr int kIterations = 1000;
    std::atomic<bool> done{false};
    std::atomic<int> read_count{0};

    // Reader threads continuously read from the store.
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            while (!done.load(std::memory_order_relaxed)) {
                auto content = store.get_content(1);
                // Content should be valid (either initial or updated).
                EXPECT_FALSE(content.empty());
                read_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Writer thread updates the store.
    for (int i = 0; i < kIterations; ++i) {
        std::string content = "updated content iteration " + std::to_string(i);
        store.load_file("initial.go", content);
    }

    done.store(true, std::memory_order_relaxed);
    for (auto& t : readers) {
        t.join();
    }

    EXPECT_GT(read_count.load(), 0);
}

// ---------------------------------------------------------------------------
// MappedFile - basic operations
// ---------------------------------------------------------------------------
TEST(MappedFileTest, OpenNonExistent) {
    MappedFile mf;
    std::string error;
    EXPECT_FALSE(mf.open("/nonexistent/path/to/file", &error));
    EXPECT_FALSE(error.empty());
}

TEST(MappedFileTest, OpenAndReadFile) {
    // Create a temporary file.
    std::string path =
        (std::filesystem::temp_directory_path() /
         ("lci_mmap_test_" + std::to_string(lci::portable::process_id()) +
          ".txt"))
            .string();
    {
        std::ofstream out(path);
        out << "hello mmap world";
    }

    MappedFile mf;
    ASSERT_TRUE(mf.open(path));
    EXPECT_EQ(mf.view(), "hello mmap world");
    EXPECT_EQ(mf.size(), 16u);
    EXPECT_TRUE(mf.is_open());

    mf.close();
    EXPECT_EQ(mf.size(), 0u);

    std::remove(path.c_str());
}

TEST(MappedFileTest, EmptyFile) {
    std::string path =
        (std::filesystem::temp_directory_path() /
         ("lci_mmap_empty_" + std::to_string(lci::portable::process_id()) +
          ".txt"))
            .string();
    { std::ofstream out(path); }

    MappedFile mf;
    ASSERT_TRUE(mf.open(path));
    EXPECT_EQ(mf.size(), 0u);
    EXPECT_TRUE(mf.view().empty());

    mf.close();
    std::remove(path.c_str());
}

TEST(MappedFileTest, MoveSemantics) {
    std::string path =
        (std::filesystem::temp_directory_path() /
         ("lci_mmap_move_" + std::to_string(lci::portable::process_id()) +
          ".txt"))
            .string();
    {
        std::ofstream out(path);
        out << "move test";
    }

    MappedFile mf1;
    ASSERT_TRUE(mf1.open(path));

    MappedFile mf2 = std::move(mf1);
    EXPECT_EQ(mf2.view(), "move test");
    EXPECT_EQ(mf1.size(), 0u);  // NOLINT(bugprone-use-after-move)

    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// SHA-256 correctness
// ---------------------------------------------------------------------------
TEST(FileContentStoreTest, Sha256KnownValue) {
    // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    FileContentStore store;
    FileID id = store.load_file("empty.go", "");
    auto hash = store.get_content_hash(id);

    // Empty content has known SHA-256.
    EXPECT_EQ(hash[0], 0xe3);
    EXPECT_EQ(hash[1], 0xb0);
    EXPECT_EQ(hash[2], 0xc4);
    EXPECT_EQ(hash[3], 0x42);
}

TEST(FileContentStoreTest, Sha256HelloWorld) {
    // SHA-256("hello world") = b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
    FileContentStore store;
    FileID id = store.load_file("hw.go", "hello world");
    auto hash = store.get_content_hash(id);

    EXPECT_EQ(hash[0], 0xb9);
    EXPECT_EQ(hash[1], 0x4d);
    EXPECT_EQ(hash[2], 0x27);
    EXPECT_EQ(hash[3], 0xb9);
}

// Regression: enforce_memory_limit must not corrupt the store when more than
// one entry is evicted in a single call. The earlier implementation looked up
// each evict_id's position in id_index after the prior erase had already
// shifted positions, removing the wrong entry from the vector.
TEST(FileContentStoreTest, MultiEvictionPreservesRemainingEntries) {
    // 1 KB cap forces eviction once a few ~600-byte files coexist.
    const int64_t kLimit = 1024;
    FileContentStore store(kLimit);

    const std::string payload(600, 'x');
    FileID id_a = store.load_file("a.go", payload);
    FileID id_b = store.load_file("b.go", payload);
    FileID id_c = store.load_file("c.go", payload);
    FileID id_d = store.load_file("d.go", payload);
    FileID id_e = store.load_file("e.go", payload);

    // Most recent loads must remain reachable through both lookup paths.
    EXPECT_NE(store.get_file(id_e), nullptr);
    EXPECT_EQ(store.path_to_id("e.go"), id_e);

    // Memory accounting agrees with surviving content (single-counting).
    EXPECT_LE(store.get_memory_usage(), kLimit + 700);

    // Any lookup that succeeds for an evicted id must return the same id —
    // the previous bug erased a different row, leaving stale id_index
    // pointers that mapped one id to another id's content.
    auto check_consistency = [&](FileID id, const std::string& path) {
        auto fp = store.get_file(id);
        if (fp != nullptr) {
            EXPECT_EQ(fp->file_id, id);
        }
        FileID via_path = store.path_to_id(path);
        if (via_path != 0) {
            EXPECT_EQ(via_path, id);
        }
    };
    check_consistency(id_a, "a.go");
    check_consistency(id_b, "b.go");
    check_consistency(id_c, "c.go");
    check_consistency(id_d, "d.go");
}

}  // namespace
}  // namespace lci
