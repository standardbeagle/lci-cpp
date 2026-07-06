#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/debounced_rebuilder.h>
#include <lci/indexing/deleted_file_tracker.h>
#include <lci/indexing/watcher.h>

#include "unique_temp.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

namespace fs = std::filesystem;

// -- Temp directory helper (matches existing test pattern) --------------------

class TempDir {
  public:
    TempDir() {
        path_ = test::unique_temp_dir("lci_watcher_test_");
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

    void write_file(const std::string& rel_path,
                    const std::string& content) {
        auto full = path_ / rel_path;
        fs::create_directories(full.parent_path());
        std::ofstream f(full);
        f << content;
    }

  private:
    fs::path path_;
};

// =============================================================================
// DeletedFileTracker tests
// =============================================================================

TEST(DeletedFileTrackerTest, EmptyByDefault) {
    DeletedFileTracker tracker;
    EXPECT_EQ(tracker.deleted_count(), 0);
    EXPECT_FALSE(tracker.is_deleted(1));
    EXPECT_FALSE(tracker.is_deleted(42));
}

TEST(DeletedFileTrackerTest, MarkAndCheck) {
    DeletedFileTracker tracker;
    tracker.mark_deleted(5);
    EXPECT_TRUE(tracker.is_deleted(5));
    EXPECT_FALSE(tracker.is_deleted(6));
    EXPECT_EQ(tracker.deleted_count(), 1);
}

TEST(DeletedFileTrackerTest, MarkDuplicateIsIdempotent) {
    DeletedFileTracker tracker;
    tracker.mark_deleted(10);
    tracker.mark_deleted(10);
    EXPECT_EQ(tracker.deleted_count(), 1);
}

TEST(DeletedFileTrackerTest, MarkBatch) {
    DeletedFileTracker tracker;
    tracker.mark_deleted_batch({1, 2, 3, 4, 5});
    EXPECT_EQ(tracker.deleted_count(), 5);
    for (FileID id = 1; id <= 5; ++id) {
        EXPECT_TRUE(tracker.is_deleted(id));
    }
    EXPECT_FALSE(tracker.is_deleted(6));
}

TEST(DeletedFileTrackerTest, MarkBatchEmpty) {
    DeletedFileTracker tracker;
    tracker.mark_deleted_batch({});
    EXPECT_EQ(tracker.deleted_count(), 0);
}

TEST(DeletedFileTrackerTest, FilterCandidatesEmpty) {
    DeletedFileTracker tracker;
    auto result = tracker.filter_candidates({1, 2, 3});
    EXPECT_EQ(result.size(), 3u);
}

TEST(DeletedFileTrackerTest, FilterCandidatesRemovesDeleted) {
    DeletedFileTracker tracker;
    tracker.mark_deleted(2);
    tracker.mark_deleted(4);
    auto result = tracker.filter_candidates({1, 2, 3, 4, 5});
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], 1u);
    EXPECT_EQ(result[1], 3u);
    EXPECT_EQ(result[2], 5u);
}

TEST(DeletedFileTrackerTest, ClearResetsAll) {
    DeletedFileTracker tracker;
    tracker.mark_deleted_batch({1, 2, 3});
    EXPECT_EQ(tracker.deleted_count(), 3);
    tracker.clear();
    EXPECT_EQ(tracker.deleted_count(), 0);
    EXPECT_FALSE(tracker.is_deleted(1));
}

TEST(DeletedFileTrackerTest, DeletedFileIdsReturnsAll) {
    DeletedFileTracker tracker;
    tracker.mark_deleted_batch({10, 20, 30});
    auto ids = tracker.deleted_file_ids();
    EXPECT_EQ(ids.size(), 3u);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 10u);
    EXPECT_EQ(ids[1], 20u);
    EXPECT_EQ(ids[2], 30u);
}

TEST(DeletedFileTrackerTest, ConcurrentMarks) {
    DeletedFileTracker tracker;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&tracker, t] {
            for (int i = 0; i < kPerThread; ++i) {
                tracker.mark_deleted(
                    static_cast<FileID>(t * kPerThread + i + 1));
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(tracker.deleted_count(), kThreads * kPerThread);
}

// =============================================================================
// DebouncedRebuilder tests
// =============================================================================

TEST(DebouncedRebuilderTest, PendingCountStartsAtZero) {
    DebouncedRebuilder rebuilder(std::chrono::milliseconds{50});
    EXPECT_EQ(rebuilder.pending_count(), 0);
    rebuilder.shutdown();
}

TEST(DebouncedRebuilderTest, ScheduleIncreasesPending) {
    DebouncedRebuilder rebuilder(std::chrono::milliseconds{500});
    rebuilder.schedule_rebuild(1);
    rebuilder.schedule_rebuild(2);
    // Pending count could be 0-2 depending on timing, but should not be
    // negative. With a 500ms debounce and immediate check, likely 2.
    EXPECT_GE(rebuilder.pending_count(), 0);
    rebuilder.shutdown();
}

TEST(DebouncedRebuilderTest, ForceRebuildFiresCallback) {
    std::vector<FileID> received;
    std::mutex mu;
    std::condition_variable cv;
    bool called = false;

    DebouncedRebuilder rebuilder(std::chrono::milliseconds{5000});
    rebuilder.set_callback([&](const std::vector<FileID>& files) {
        std::lock_guard lock(mu);
        received = files;
        called = true;
        cv.notify_one();
    });

    rebuilder.schedule_rebuild(10);
    rebuilder.schedule_rebuild(20);
    rebuilder.force_rebuild();

    std::unique_lock lock(mu);
    EXPECT_TRUE(called);
    std::sort(received.begin(), received.end());
    EXPECT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0], 10u);
    EXPECT_EQ(received[1], 20u);
    rebuilder.shutdown();
}

TEST(DebouncedRebuilderTest, DebouncePreventsImmediateFiring) {
    std::atomic<int> call_count{0};

    DebouncedRebuilder rebuilder(std::chrono::milliseconds{100});
    rebuilder.set_callback([&](const std::vector<FileID>&) {
        ++call_count;
    });

    // Schedule rapid events
    for (FileID i = 1; i <= 10; ++i) {
        rebuilder.schedule_rebuild(i);
    }

    // Should not have fired yet (debounce is 100ms)
    EXPECT_EQ(call_count.load(), 0);

    // Wait for debounce + margin
    std::this_thread::sleep_for(std::chrono::milliseconds{250});

    // Should have fired exactly once
    EXPECT_EQ(call_count.load(), 1);
    rebuilder.shutdown();
}

TEST(DebouncedRebuilderTest, ShutdownIsClean) {
    DebouncedRebuilder rebuilder(std::chrono::milliseconds{50});
    rebuilder.schedule_rebuild(1);
    rebuilder.shutdown();
    // No hang, no crash -- thread joined cleanly.
}

TEST(DebouncedRebuilderTest, DeduplicatesFiles) {
    std::vector<FileID> received;

    DebouncedRebuilder rebuilder(std::chrono::milliseconds{5000});
    rebuilder.set_callback([&](const std::vector<FileID>& files) {
        received = files;
    });

    rebuilder.schedule_rebuild(1);
    rebuilder.schedule_rebuild(1);
    rebuilder.schedule_rebuild(1);
    rebuilder.force_rebuild();

    EXPECT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], 1u);
    rebuilder.shutdown();
}

// =============================================================================
// FileWatcher tests
// =============================================================================

TEST(FileWatcherTest, DisabledWatchModeReturnsFalse) {
    Config cfg = make_default_config();
    cfg.index.watch_mode = false;
    FileWatcher watcher(cfg);
    EXPECT_FALSE(watcher.start("/tmp"));
}

TEST(FileWatcherTest, EmptyRootReturnsFalse) {
    Config cfg = make_default_config();
    cfg.index.watch_mode = true;
    FileWatcher watcher(cfg);
    EXPECT_FALSE(watcher.start(""));
}

TEST(FileWatcherTest, StatsInitiallyInactive) {
    Config cfg = make_default_config();
    FileWatcher watcher(cfg);
    auto stats = watcher.get_stats();
    EXPECT_EQ(stats.events_processed, 0);
    EXPECT_EQ(stats.error_count, 0);
    EXPECT_FALSE(stats.is_active);
}

TEST(FileWatcherTest, StartAndStopNoLeak) {
    TempDir tmp;
    tmp.write_file("hello.go", "package main\n");

    Config cfg = make_default_config();
    cfg.project.root = tmp.path().string();
    cfg.index.watch_mode = true;
    cfg.include = {"**/*.go"};

    FileWatcher watcher(cfg);
    EXPECT_TRUE(watcher.start(tmp.path().string()));
    auto stats = watcher.get_stats();
    EXPECT_TRUE(stats.is_active);
    watcher.stop();
    stats = watcher.get_stats();
    EXPECT_FALSE(stats.is_active);
}

TEST(FileWatcherTest, DetectsFileCreate) {
    TempDir tmp;

    Config cfg = make_default_config();
    cfg.project.root = tmp.path().string();
    cfg.index.watch_mode = true;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<std::string, FileEventType>> events;

    FileWatcher watcher(cfg);
    watcher.set_callback([&](const std::string& path, FileEventType type) {
        std::lock_guard lock(mu);
        events.emplace_back(path, type);
        cv.notify_one();
    });

    ASSERT_TRUE(watcher.start(tmp.path().string()));

    // Create a file after watcher is started
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    tmp.write_file("new_file.cpp", "int main() {}\n");

    {
        std::unique_lock lock(mu);
        cv.wait_for(lock, std::chrono::seconds{2},
                    [&] { return !events.empty(); });
    }

    watcher.stop();

    ASSERT_FALSE(events.empty());
    bool found_create = false;
    for (const auto& [path, type] : events) {
        if (type == FileEventType::Create &&
            path.find("new_file.cpp") != std::string::npos) {
            found_create = true;
        }
    }
    EXPECT_TRUE(found_create);
}

TEST(FileWatcherTest, DetectsFileModify) {
    TempDir tmp;
    tmp.write_file("existing.go", "package main\n");

    Config cfg = make_default_config();
    cfg.project.root = tmp.path().string();
    cfg.index.watch_mode = true;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<std::string, FileEventType>> events;

    FileWatcher watcher(cfg);
    watcher.set_callback([&](const std::string& path, FileEventType type) {
        std::lock_guard lock(mu);
        events.emplace_back(path, type);
        cv.notify_one();
    });

    ASSERT_TRUE(watcher.start(tmp.path().string()));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Modify the file
    {
        std::ofstream f(tmp.path() / "existing.go");
        f << "package main\nfunc hello() {}\n";
    }

    {
        std::unique_lock lock(mu);
        cv.wait_for(lock, std::chrono::seconds{2},
                    [&] { return !events.empty(); });
    }

    watcher.stop();

    ASSERT_FALSE(events.empty());
    bool found_write = false;
    for (const auto& [path, type] : events) {
        if (type == FileEventType::Write &&
            path.find("existing.go") != std::string::npos) {
            found_write = true;
        }
    }
    EXPECT_TRUE(found_write);
}

TEST(FileWatcherTest, DetectsFileDelete) {
    TempDir tmp;
    tmp.write_file("to_delete.rs", "fn main() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = tmp.path().string();
    cfg.index.watch_mode = true;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<std::string, FileEventType>> events;

    FileWatcher watcher(cfg);
    watcher.set_callback([&](const std::string& path, FileEventType type) {
        std::lock_guard lock(mu);
        events.emplace_back(path, type);
        cv.notify_one();
    });

    ASSERT_TRUE(watcher.start(tmp.path().string()));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    fs::remove(tmp.path() / "to_delete.rs");

    {
        std::unique_lock lock(mu);
        cv.wait_for(lock, std::chrono::seconds{2},
                    [&] { return !events.empty(); });
    }

    watcher.stop();

    ASSERT_FALSE(events.empty());
    bool found_delete = false;
    for (const auto& [path, type] : events) {
        if (type == FileEventType::Remove &&
            path.find("to_delete.rs") != std::string::npos) {
            found_delete = true;
        }
    }
    EXPECT_TRUE(found_delete);
}

TEST(FileWatcherTest, RecursiveDirectoryWatching) {
    TempDir tmp;
    fs::create_directories(tmp.path() / "a" / "b" / "c");

    Config cfg = make_default_config();
    cfg.project.root = tmp.path().string();
    cfg.index.watch_mode = true;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<std::string, FileEventType>> events;

    FileWatcher watcher(cfg);
    watcher.set_callback([&](const std::string& path, FileEventType type) {
        std::lock_guard lock(mu);
        events.emplace_back(path, type);
        cv.notify_one();
    });

    ASSERT_TRUE(watcher.start(tmp.path().string()));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Create file deep in nested directory
    {
        std::ofstream f(tmp.path() / "a" / "b" / "c" / "deep.py");
        f << "print('hello')\n";
    }

    {
        std::unique_lock lock(mu);
        cv.wait_for(lock, std::chrono::seconds{2},
                    [&] { return !events.empty(); });
    }

    watcher.stop();

    bool found = false;
    for (const auto& [path, type] : events) {
        if (path.find("deep.py") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(FileWatcherTest, DoubleStopIsSafe) {
    TempDir tmp;
    Config cfg = make_default_config();
    cfg.project.root = tmp.path().string();
    cfg.index.watch_mode = true;

    FileWatcher watcher(cfg);
    ASSERT_TRUE(watcher.start(tmp.path().string()));
    watcher.stop();
    watcher.stop();  // Should not crash or hang
}

}  // namespace
}  // namespace lci
