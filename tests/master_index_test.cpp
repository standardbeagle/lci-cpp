#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/index_locks.h>
#include <lci/indexing/master_index.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

// -- Temp directory helper (matches pipeline_test.cpp pattern) ----------------

class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lci_mi_test_" + std::to_string(
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

// -- IndexLockManager tests ---------------------------------------------------

TEST(IndexLockManagerTest, AcquireAndReleaseRead) {
    IndexLockManager mgr;
    EXPECT_TRUE(mgr.acquire_read(IndexType::Trigram));
    mgr.release_read(IndexType::Trigram);
}

TEST(IndexLockManagerTest, AcquireAndReleaseWrite) {
    IndexLockManager mgr;
    EXPECT_TRUE(mgr.acquire_write(IndexType::Symbol));
    mgr.release_write(IndexType::Symbol);
}

TEST(IndexLockManagerTest, ReadGuardRAII) {
    IndexLockManager mgr;
    {
        IndexLockManager::ReadGuard guard(mgr, IndexType::Postings);
        EXPECT_TRUE(guard.locked());
    }
    // Lock should be released; another write should succeed.
    EXPECT_TRUE(mgr.acquire_write(IndexType::Postings));
    mgr.release_write(IndexType::Postings);
}

TEST(IndexLockManagerTest, WriteGuardRAII) {
    IndexLockManager mgr;
    {
        IndexLockManager::WriteGuard guard(mgr, IndexType::Reference);
        EXPECT_TRUE(guard.locked());
    }
    EXPECT_TRUE(mgr.acquire_read(IndexType::Reference));
    mgr.release_read(IndexType::Reference);
}

TEST(IndexLockManagerTest, ConcurrentReads) {
    IndexLockManager mgr;
    EXPECT_TRUE(mgr.acquire_read(IndexType::Content));
    EXPECT_TRUE(mgr.acquire_read(IndexType::Content));
    mgr.release_read(IndexType::Content);
    mgr.release_read(IndexType::Content);
}

TEST(IndexLockManagerTest, IndexTypeName) {
    EXPECT_STREQ("Trigram", index_type_name(IndexType::Trigram));
    EXPECT_STREQ("Symbol", index_type_name(IndexType::Symbol));
    EXPECT_STREQ("Reference", index_type_name(IndexType::Reference));
    EXPECT_STREQ("Postings", index_type_name(IndexType::Postings));
    EXPECT_STREQ("Location", index_type_name(IndexType::Location));
    EXPECT_STREQ("Content", index_type_name(IndexType::Content));
}

// -- FileSnapshot tests -------------------------------------------------------

TEST(FileSnapshotTest, DefaultEmpty) {
    FileSnapshot snap;
    EXPECT_EQ(0, snap.file_count());
}

TEST(FileSnapshotTest, CopyOnWrite) {
    auto snap = std::make_shared<FileSnapshot>();
    snap->file_map["a.go"] = FileID{1};
    snap->reverse_file_map[FileID{1}] = "a.go";

    auto copy = std::make_shared<FileSnapshot>(*snap);
    copy->file_map["b.go"] = FileID{2};
    copy->reverse_file_map[FileID{2}] = "b.go";

    EXPECT_EQ(1, snap->file_count());
    EXPECT_EQ(2, copy->file_count());
}

// -- MasterIndex lifecycle tests ----------------------------------------------

TEST(MasterIndexTest, ConstructionDefaults) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    EXPECT_EQ(0, mi.file_count());
    EXPECT_FALSE(mi.is_indexing());

    auto stats = mi.get_stats();
    EXPECT_EQ(0, stats.total_files);
    EXPECT_FALSE(stats.is_indexing);
}

TEST(MasterIndexTest, ClearOnEmptyIndex) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_TRUE(mi.clear());
    EXPECT_EQ(0, mi.file_count());
}

TEST(MasterIndexTest, PathToIdNotFound) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_EQ(FileID{0}, mi.path_to_id("/nonexistent"));
}

TEST(MasterIndexTest, IdToPathNotFound) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_TRUE(mi.id_to_path(FileID{999}).empty());
}

TEST(MasterIndexTest, ReadSnapshotIsLockFree) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    auto snap1 = mi.read_snapshot();
    auto snap2 = mi.read_snapshot();
    EXPECT_EQ(snap1.get(), snap2.get());
}

// -- MasterIndex directory indexing -------------------------------------------

TEST(MasterIndexTest, IndexDirectoryEmptyRoot) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_FALSE(mi.index_directory(""));
}

TEST(MasterIndexTest, IndexDirectoryNonexistent) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_FALSE(mi.index_directory("/this/path/does/not/exist"));
}

TEST(MasterIndexTest, IndexDirectorySimple) {
    TempDir dir;
    dir.write_file("hello.go", "package main\nfunc main() {}\n");
    dir.write_file("util.go", "package main\nfunc helper() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    EXPECT_FALSE(mi.is_indexing());
    EXPECT_GE(mi.file_count(), 1);

    auto stats = mi.get_stats();
    EXPECT_GT(stats.indexing_time_ns, 0);
}

TEST(MasterIndexTest, IndexDirectoryThenClear) {
    TempDir dir;
    dir.write_file("a.py", "def foo():\n    pass\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    EXPECT_GE(mi.file_count(), 1);

    EXPECT_TRUE(mi.clear());
    EXPECT_EQ(0, mi.file_count());
}

TEST(MasterIndexTest, DoubleIndexDirectory) {
    TempDir dir;
    dir.write_file("x.js", "function x() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    int count1 = mi.file_count();

    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    int count2 = mi.file_count();

    EXPECT_EQ(count1, count2);
}

// -- MasterIndex single-file operations ---------------------------------------

TEST(MasterIndexTest, IndexSingleFile) {
    TempDir dir;
    dir.write_file("single.go", "package main\nfunc single() {}\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "single.go").string();
    EXPECT_TRUE(mi.index_file(file_path));
    EXPECT_EQ(1, mi.file_count());
    EXPECT_NE(FileID{0}, mi.path_to_id(file_path));
}

TEST(MasterIndexTest, IndexFileEmptyPath) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_FALSE(mi.index_file(""));
}

TEST(MasterIndexTest, IndexFileNonexistent) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_FALSE(mi.index_file("/no/such/file.go"));
}

TEST(MasterIndexTest, UpdateFile) {
    TempDir dir;
    dir.write_file("updatable.go", "package main\nvar x = 1\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "updatable.go").string();
    EXPECT_TRUE(mi.index_file(file_path));
    FileID first_id = mi.path_to_id(file_path);
    EXPECT_NE(FileID{0}, first_id);

    std::string new_content = "package main\nvar x = 2\nvar y = 3\n";
    EXPECT_TRUE(mi.update_file(file_path, new_content));
    FileID second_id = mi.path_to_id(file_path);
    EXPECT_NE(FileID{0}, second_id);

    EXPECT_EQ(1, mi.file_count());

    // Verify the content was actually updated.
    auto content = mi.file_content_store().get_content(second_id);
    EXPECT_FALSE(content.empty());
    EXPECT_NE(std::string::npos, content.find("var y = 3"));
}

TEST(MasterIndexTest, UpdateFileEmptyContent) {
    TempDir dir;
    dir.write_file("f.go", "package main\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "f.go").string();
    EXPECT_FALSE(mi.update_file(file_path, ""));
}

TEST(MasterIndexTest, RemoveFile) {
    TempDir dir;
    dir.write_file("removable.go", "package main\nfunc rm() {}\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "removable.go").string();
    EXPECT_TRUE(mi.index_file(file_path));
    EXPECT_EQ(1, mi.file_count());

    EXPECT_TRUE(mi.remove_file(file_path));
    EXPECT_EQ(0, mi.file_count());
    EXPECT_EQ(FileID{0}, mi.path_to_id(file_path));
}

TEST(MasterIndexTest, RemoveNonexistentFile) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_TRUE(mi.remove_file("/no/such/file"));
}

// -- Concurrent access tests --------------------------------------------------

TEST(MasterIndexTest, ConcurrentSnapshotReads) {
    TempDir dir;
    dir.write_file("concurrent.go", "package main\nfunc concurrent() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    mi.index_directory(dir.path().string());

    constexpr int kReaderCount = 8;
    constexpr int kReadsPerThread = 1000;
    std::vector<std::thread> readers;

    for (int i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < kReadsPerThread; ++j) {
                auto snap = mi.read_snapshot();
                (void)snap->file_count();
            }
        });
    }

    for (auto& t : readers) t.join();
}

TEST(MasterIndexTest, ConcurrentReadsWhileUpdating) {
    TempDir dir;
    dir.write_file("base.go", "package main\nfunc base() {}\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "base.go").string();
    mi.index_file(file_path);

    constexpr int kReaderCount = 4;
    constexpr int kIterations = 100;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;

    for (int i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                auto snap = mi.read_snapshot();
                (void)snap->file_count();
                (void)mi.path_to_id(file_path);
            }
        });
    }

    // Writer thread.
    std::thread writer([&] {
        for (int i = 0; i < kIterations; ++i) {
            std::string content = "package main\nvar v" +
                                  std::to_string(i) + " = " +
                                  std::to_string(i) + "\n";
            mi.update_file(file_path, content);
        }
        stop.store(true, std::memory_order_release);
    });

    writer.join();
    for (auto& t : readers) t.join();

    EXPECT_EQ(1, mi.file_count());
}

// -- Sub-index access tests ---------------------------------------------------

TEST(MasterIndexTest, SubIndexAccess) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    // Ensure sub-indexes are accessible and valid.
    (void)mi.trigram_index();
    (void)mi.ref_tracker();
    (void)mi.postings_index();
    (void)mi.symbol_location_index();
    (void)mi.file_content_store();
    (void)mi.config();
}

// -- Stats after indexing -----------------------------------------------------

TEST(MasterIndexTest, StatsAfterIndexing) {
    TempDir dir;
    dir.write_file("stats.go", "package main\nfunc stats() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    mi.index_directory(dir.path().string());
    auto stats = mi.get_stats();
    EXPECT_GE(stats.total_files, 1);
    EXPECT_FALSE(stats.is_indexing);
    EXPECT_GT(stats.indexing_time_ns, 0);
}

}  // namespace
}  // namespace lci
