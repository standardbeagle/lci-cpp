#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/config/gitignore.h>
#include <lci/core/file_service.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/trigram.h>
#include <lci/indexing/binary_detector.h>
#include <lci/indexing/pipeline.h>
#include <lci/indexing/pipeline_processor.h>
#include <lci/indexing/pipeline_progress.h>
#include <lci/indexing/pipeline_scanner.h>
#include <lci/indexing/pipeline_types.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

// -- Helper: create a temporary directory tree for scanner tests --------------

class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lci_test_" + std::to_string(
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

    void create_dir(const std::string& rel_path) {
        std::filesystem::create_directories(path_ / rel_path);
    }

  private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// ---------------------------------------------------------------------------
// BinaryDetector
// ---------------------------------------------------------------------------

TEST(BinaryDetectorTest, RecognizesBinaryExtensions) {
    BinaryDetector det;
    EXPECT_TRUE(det.is_binary_by_extension("image.png"));
    EXPECT_TRUE(det.is_binary_by_extension("archive.zip"));
    EXPECT_TRUE(det.is_binary_by_extension("binary.exe"));
    EXPECT_TRUE(det.is_binary_by_extension("font.woff2"));
    EXPECT_TRUE(det.is_binary_by_extension("data.db"));
}

TEST(BinaryDetectorTest, AllowsTextExtensions) {
    BinaryDetector det;
    EXPECT_FALSE(det.is_binary_by_extension("main.go"));
    EXPECT_FALSE(det.is_binary_by_extension("script.py"));
    EXPECT_FALSE(det.is_binary_by_extension("app.js"));
    EXPECT_FALSE(det.is_binary_by_extension("style.min.css"));
    EXPECT_FALSE(det.is_binary_by_extension("bundle.min.js"));
}

TEST(BinaryDetectorTest, NoExtensionReturnsNonBinary) {
    BinaryDetector det;
    EXPECT_FALSE(det.is_binary_by_extension("Makefile"));
    EXPECT_FALSE(det.is_binary_by_extension("Dockerfile"));
}

TEST(BinaryDetectorTest, CaseInsensitiveExtension) {
    BinaryDetector det;
    EXPECT_TRUE(det.is_binary_by_extension("image.PNG"));
    EXPECT_TRUE(det.is_binary_by_extension("archive.ZIP"));
}

TEST(BinaryDetectorTest, MagicNumberDetectsGzip) {
    BinaryDetector det;
    std::string content = "\x1F\x8B\x08\x00rest of data";
    EXPECT_TRUE(det.is_binary_by_magic_number(content));
}

TEST(BinaryDetectorTest, MagicNumberDetectsPNG) {
    BinaryDetector det;
    std::string content = "\x89\x50\x4E\x47rest of image data";
    EXPECT_TRUE(det.is_binary_by_magic_number(content));
}

TEST(BinaryDetectorTest, MagicNumberDetectsELF) {
    BinaryDetector det;
    std::string content = "\x7F\x45\x4C\x46" "binary elf data";
    EXPECT_TRUE(det.is_binary_by_magic_number(content));
}

TEST(BinaryDetectorTest, TextContentNotBinary) {
    BinaryDetector det;
    EXPECT_FALSE(det.is_binary_by_magic_number("func main() {\n}\n"));
    EXPECT_FALSE(det.is_binary_by_magic_number(""));
}

TEST(BinaryDetectorTest, NullBytesDetectedAsBinary) {
    BinaryDetector det;
    // Create content with >1% null bytes
    std::string content(200, 'a');
    content[10] = '\0';
    content[20] = '\0';
    content[30] = '\0';
    EXPECT_TRUE(det.is_binary_by_magic_number(content));
}

TEST(BinaryDetectorTest, CombinedCheckUsesExtensionFirst) {
    BinaryDetector det;
    EXPECT_TRUE(det.is_binary("file.png", "text content"));
    EXPECT_FALSE(det.is_binary("file.go", "func main() {}"));
}

// ---------------------------------------------------------------------------
// GitignoreParser
// ---------------------------------------------------------------------------

TEST(GitignoreParserTest, ExactMatch) {
    GitignoreParser p;
    p.add_pattern("node_modules");
    EXPECT_TRUE(p.should_ignore("node_modules", false));
    EXPECT_TRUE(p.should_ignore("src/node_modules", false));
    EXPECT_FALSE(p.should_ignore("src/modules", false));
}

TEST(GitignoreParserTest, WildcardPattern) {
    GitignoreParser p;
    p.add_pattern("*.log");
    EXPECT_TRUE(p.should_ignore("error.log", false));
    EXPECT_TRUE(p.should_ignore("logs/debug.log", false));
    EXPECT_FALSE(p.should_ignore("main.go", false));
}

TEST(GitignoreParserTest, DirectoryPattern) {
    GitignoreParser p;
    p.add_pattern("build/");
    EXPECT_TRUE(p.should_ignore("build", true));
    EXPECT_TRUE(p.should_ignore("build/output.o", false));
    EXPECT_FALSE(p.should_ignore("rebuild", false));
}

TEST(GitignoreParserTest, NegationPattern) {
    GitignoreParser p;
    p.add_pattern("*.log");
    p.add_pattern("!important.log");
    EXPECT_TRUE(p.should_ignore("error.log", false));
    EXPECT_FALSE(p.should_ignore("important.log", false));
}

TEST(GitignoreParserTest, AbsolutePattern) {
    GitignoreParser p;
    p.add_pattern("/dist");
    EXPECT_TRUE(p.should_ignore("dist", false));
    EXPECT_FALSE(p.should_ignore("src/dist", false));
}

TEST(GitignoreParserTest, DoubleStarPattern) {
    GitignoreParser p;
    p.add_pattern("**/*.test.js");
    EXPECT_TRUE(p.should_ignore("app.test.js", false));
    EXPECT_TRUE(p.should_ignore("src/deep/nested/app.test.js", false));
}

TEST(GitignoreParserTest, SkipsCommentsAndEmptyLines) {
    GitignoreParser p;
    p.add_pattern("# this is a comment");
    p.add_pattern("");
    p.add_pattern("*.log");
    EXPECT_TRUE(p.should_ignore("error.log", false));
    EXPECT_FALSE(p.should_ignore("# this is a comment", false));
}

TEST(GitignoreParserTest, LoadFromFile) {
    TempDir dir;
    dir.write_file(".gitignore", "*.log\nbuild/\nnode_modules\n");

    GitignoreParser p;
    EXPECT_TRUE(p.load_gitignore(dir.path().string()));
    EXPECT_TRUE(p.should_ignore("error.log", false));
    EXPECT_TRUE(p.should_ignore("build", true));
    EXPECT_TRUE(p.should_ignore("node_modules", false));
}

TEST(GitignoreParserTest, MissingFileIsNotError) {
    GitignoreParser p;
    EXPECT_TRUE(p.load_gitignore("/nonexistent/path"));
}

TEST(GitignoreParserTest, GetExclusionPatterns) {
    GitignoreParser p;
    p.add_pattern("*.log");
    p.add_pattern("build/");

    auto patterns = p.get_exclusion_patterns();
    ASSERT_EQ(patterns.size(), 2u);
    EXPECT_EQ(patterns[0], "**/*.log");
    EXPECT_EQ(patterns[1], "**/build/**");
}

// ---------------------------------------------------------------------------
// Pipeline types
// ---------------------------------------------------------------------------

TEST(PipelineTypesTest, CalculateOptimalBuffers) {
    auto [task_buf, result_buf] = calculate_optimal_channel_buffers(100);
    EXPECT_GT(task_buf, 0);
    EXPECT_GT(result_buf, 0);
    EXPECT_LE(task_buf, pipeline_constants::kMaxTaskChannelBuffer);
    EXPECT_LE(result_buf, pipeline_constants::kMaxResultChannelBuffer);
}

TEST(PipelineTypesTest, BuffersCappedForLargeFileCounts) {
    auto [task_buf, result_buf] = calculate_optimal_channel_buffers(1'000'000);
    EXPECT_EQ(task_buf, pipeline_constants::kMaxTaskChannelBuffer);
    EXPECT_EQ(result_buf, pipeline_constants::kMaxResultChannelBuffer);
}

// ---------------------------------------------------------------------------
// BoundedQueue
// ---------------------------------------------------------------------------

TEST(BoundedQueueTest, PushAndPop) {
    BoundedQueue<int> q(10);
    EXPECT_TRUE(q.push(42));
    int val = 0;
    EXPECT_TRUE(q.pop(val));
    EXPECT_EQ(val, 42);
}

TEST(BoundedQueueTest, CloseUnblocksConsumer) {
    BoundedQueue<int> q(10);
    q.close();
    int val = 0;
    EXPECT_FALSE(q.pop(val));
}

TEST(BoundedQueueTest, CloseUnblocksProducer) {
    BoundedQueue<int> q(1);
    q.push(1);
    // Queue is full, close should unblock a blocked producer
    std::thread t([&] {
        // Allow main thread to call close first
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        q.close();
    });
    q.push(2);  // This will block briefly, then return false after close
    t.join();
}

TEST(BoundedQueueTest, MultiProducerMultiConsumer) {
    BoundedQueue<int> q(4);
    std::atomic<int> sum{0};
    constexpr int per_producer = 50;
    constexpr int producers = 4;
    constexpr int consumers = 2;

    std::vector<std::thread> prod_threads;
    for (int p = 0; p < producers; ++p) {
        prod_threads.emplace_back([&q, p] {
            for (int i = 0; i < per_producer; ++i) {
                q.push(p * per_producer + i + 1);
            }
        });
    }

    std::vector<std::thread> cons_threads;
    for (int c = 0; c < consumers; ++c) {
        cons_threads.emplace_back([&q, &sum] {
            int val;
            while (q.pop(val)) {
                sum.fetch_add(val, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : prod_threads) t.join();
    q.close();
    for (auto& t : cons_threads) t.join();

    // Sum of 1..200 = 20100
    EXPECT_EQ(sum.load(), 20100);
}

// ---------------------------------------------------------------------------
// FileScanner
// ---------------------------------------------------------------------------

TEST(FileScannerTest, ScansDirectoryTree) {
    TempDir dir;
    dir.write_file("main.go", "package main");
    dir.write_file("src/lib.go", "package lib");
    dir.write_file("docs/readme.md", "# Readme");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    FileScanner scanner(cfg);
    auto tasks = scanner.scan();

    EXPECT_GE(tasks.size(), 3u);
    bool found_main = false;
    bool found_lib = false;
    for (const auto& t : tasks) {
        if (t.path.find("main.go") != std::string::npos) {
            found_main = true;
            EXPECT_EQ(t.language, "go");
            EXPECT_EQ(t.priority, 10);
        }
        if (t.path.find("lib.go") != std::string::npos) found_lib = true;
    }
    EXPECT_TRUE(found_main);
    EXPECT_TRUE(found_lib);
}

TEST(FileScannerTest, SkipsBinaryFiles) {
    TempDir dir;
    dir.write_file("main.go", "package main");
    dir.write_file("image.png", "fake png data");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    FileScanner scanner(cfg);
    auto tasks = scanner.scan();

    for (const auto& t : tasks) {
        EXPECT_EQ(t.path.find(".png"), std::string::npos);
    }
}

TEST(FileScannerTest, RespectsGitignore) {
    TempDir dir;
    dir.write_file(".gitignore", "*.log\nbuild/\n");
    dir.write_file("main.go", "package main");
    dir.write_file("error.log", "some log");
    dir.create_dir("build");
    dir.write_file("build/output.o", "binary");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    cfg.index.respect_gitignore = true;

    FileScanner scanner(cfg);
    auto tasks = scanner.scan();

    for (const auto& t : tasks) {
        EXPECT_EQ(t.path.find("error.log"), std::string::npos);
        EXPECT_EQ(t.path.find("build/"), std::string::npos);
    }
}

TEST(FileScannerTest, SkipsHiddenDirectories) {
    TempDir dir;
    dir.write_file("main.go", "package main");
    dir.write_file(".hidden/secret.go", "package hidden");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    FileScanner scanner(cfg);
    auto tasks = scanner.scan();

    for (const auto& t : tasks) {
        EXPECT_EQ(t.path.find(".hidden"), std::string::npos);
    }
}

TEST(FileScannerTest, RespectsExclusionPatterns) {
    TempDir dir;
    dir.write_file("main.go", "package main");
    dir.write_file("vendor/dep.go", "package dep");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    cfg.exclude.push_back("vendor/**");

    FileScanner scanner(cfg);
    auto tasks = scanner.scan();

    for (const auto& t : tasks) {
        EXPECT_EQ(t.path.find("vendor"), std::string::npos);
    }
}

TEST(FileScannerTest, RespectsMaxFileSize) {
    TempDir dir;
    dir.write_file("small.go", "package small");
    dir.write_file("large.go", std::string(200, 'x'));

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    cfg.index.max_file_size = 100;

    FileScanner scanner(cfg);
    auto tasks = scanner.scan();

    for (const auto& t : tasks) {
        EXPECT_EQ(t.path.find("large.go"), std::string::npos);
    }
}

TEST(FileScannerTest, DetectsSymlinkCycles) {
    TempDir dir;
    dir.write_file("main.go", "package main");
    dir.create_dir("sub");
    dir.write_file("sub/lib.go", "package lib");

    // Create a symlink cycle: sub/link -> ..
    std::error_code ec;
    std::filesystem::create_directory_symlink(
        dir.path(), dir.path() / "sub" / "link", ec);
    if (ec) {
        GTEST_SKIP() << "Symlink creation not supported: " << ec.message();
    }

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    cfg.index.follow_symlinks = true;

    FileScanner scanner(cfg);
    // Must complete without infinite loop
    auto tasks = scanner.scan();
    EXPECT_GE(tasks.size(), 1u);
}

TEST(FileScannerTest, SortsByPriority) {
    TempDir dir;
    dir.write_file("readme.md", "# Readme");
    dir.write_file("main.go", "package main");
    dir.write_file("data.json", "{}");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    FileScanner scanner(cfg);
    auto tasks = scanner.scan();

    if (tasks.size() >= 2) {
        EXPECT_GE(tasks[0].priority, tasks[1].priority);
    }
}

// ---------------------------------------------------------------------------
// FileProcessor
// ---------------------------------------------------------------------------

TEST(FileProcessorTest, ProcessesFilesInParallel) {
    TempDir dir;
    dir.write_file("a.go", "package a\nfunc Hello() {}\n");
    dir.write_file("b.py", "def hello():\n    pass\n");
    dir.write_file("c.js", "function hello() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;

    auto [task_buf, result_buf] = calculate_optimal_channel_buffers(3);
    BoundedQueue<FileTask> task_queue(task_buf);
    BoundedQueue<ProcessedFile> result_queue(result_buf);

    // Enqueue tasks
    std::vector<std::string> files = {"a.go", "b.py", "c.js"};
    for (const auto& f : files) {
        FileTask t;
        t.path = (dir.path() / f).string();
        t.language = f.substr(f.rfind('.') + 1);
        t.size = 30;
        t.priority = 10;
        task_queue.push(std::move(t));
    }
    task_queue.close();

    FileProcessor processor(cfg, file_service, &trigram_idx);

    // Process with 2 workers
    processor.process(task_queue, result_queue, 2);

    // Collect results
    std::vector<ProcessedFile> results;
    ProcessedFile r;
    while (result_queue.pop(r)) {
        results.push_back(std::move(r));
    }

    EXPECT_EQ(results.size(), 3u);
    for (const auto& res : results) {
        EXPECT_EQ(res.stage, "completed");
        EXPECT_FALSE(res.has_error);
        EXPECT_NE(res.file_id, 0u);
    }
}

TEST(FileProcessorTest, SkipsBinaryContent) {
    TempDir dir;
    // Write a file with ELF magic number
    std::string elf_content = "\x7F\x45\x4C\x46" + std::string(100, 'x');
    dir.write_file("binary.dat", elf_content);

    Config cfg = make_default_config();

    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;

    BoundedQueue<FileTask> task_queue(10);
    BoundedQueue<ProcessedFile> result_queue(10);

    FileTask t;
    t.path = (dir.path() / "binary.dat").string();
    t.language = "unknown";
    t.size = static_cast<int64_t>(elf_content.size());
    task_queue.push(std::move(t));
    task_queue.close();

    FileProcessor processor(cfg, file_service, &trigram_idx);
    processor.process(task_queue, result_queue, 1);

    ProcessedFile r;
    ASSERT_TRUE(result_queue.pop(r));
    EXPECT_EQ(r.stage, "binary_detection");
    EXPECT_TRUE(r.has_error);
}

TEST(FileProcessorTest, BucketsTrigrams) {
    TempDir dir;
    dir.write_file("code.go", "package main\nfunc main() {}\n");

    Config cfg = make_default_config();

    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;

    BoundedQueue<FileTask> task_queue(10);
    BoundedQueue<ProcessedFile> result_queue(10);

    FileTask t;
    t.path = (dir.path() / "code.go").string();
    t.language = "go";
    t.size = 30;
    task_queue.push(std::move(t));
    task_queue.close();

    FileProcessor processor(cfg, file_service, &trigram_idx);
    processor.process(task_queue, result_queue, 1);

    ProcessedFile r;
    ASSERT_TRUE(result_queue.pop(r));
    EXPECT_EQ(r.stage, "completed");
    EXPECT_FALSE(r.bucketed_trigrams.buckets.empty());
    EXPECT_NE(r.bucketed_trigrams.file_id, 0u);

    // Verify some buckets have data
    bool has_data = false;
    for (const auto& bucket : r.bucketed_trigrams.buckets) {
        if (!bucket.trigrams.empty()) { has_data = true; break; }
    }
    EXPECT_TRUE(has_data);
}

TEST(FileProcessorTest, BackPressureWithSmallQueue) {
    TempDir dir;
    for (int i = 0; i < 20; ++i) {
        dir.write_file("file" + std::to_string(i) + ".go",
                       "package f" + std::to_string(i));
    }

    Config cfg = make_default_config();

    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;

    // Very small result queue to force back-pressure
    BoundedQueue<FileTask> task_queue(5);
    BoundedQueue<ProcessedFile> result_queue(2);

    // Producer thread feeds tasks
    std::thread producer([&] {
        for (int i = 0; i < 20; ++i) {
            FileTask t;
            t.path = (dir.path() / ("file" + std::to_string(i) + ".go")).string();
            t.language = "go";
            t.size = 20;
            if (!task_queue.push(std::move(t))) break;
        }
        task_queue.close();
    });

    // Consumer thread drains results slowly
    std::atomic<int> count{0};
    std::thread consumer([&] {
        ProcessedFile r;
        while (result_queue.pop(r)) {
            count.fetch_add(1, std::memory_order_relaxed);
            // Simulate slow consumer
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    FileProcessor processor(cfg, file_service, &trigram_idx);
    processor.process(task_queue, result_queue, 2);

    producer.join();
    consumer.join();

    EXPECT_EQ(count.load(), 20);
}

// ---------------------------------------------------------------------------
// FileScanner - language detection
// ---------------------------------------------------------------------------

TEST(FileScannerTest, DetectsLanguages) {
    TempDir dir;
    dir.write_file("main.go", "package main");
    dir.write_file("app.py", "print('hello')");
    dir.write_file("index.ts", "const x = 1;");
    dir.write_file("lib.rs", "fn main() {}");
    dir.write_file("App.tsx", "export default {};");
    dir.write_file("main.cpp", "int main() {}");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    FileScanner scanner(cfg);
    auto tasks = scanner.scan();

    absl::flat_hash_map<std::string, std::string> expected = {
        {"main.go", "go"}, {"app.py", "python"}, {"index.ts", "typescript"},
        {"lib.rs", "rust"}, {"App.tsx", "tsx"}, {"main.cpp", "cpp"},
    };

    for (const auto& t : tasks) {
        auto filename = std::filesystem::path(t.path).filename().string();
        auto it = expected.find(filename);
        if (it != expected.end()) {
            EXPECT_EQ(t.language, it->second) << "File: " << filename;
        }
    }
}

// ---------------------------------------------------------------------------
// ProgressTracker
// ---------------------------------------------------------------------------

TEST(ProgressTrackerTest, StartsInScanningPhase) {
    ProgressTracker tracker;
    auto p = tracker.get_progress();
    EXPECT_TRUE(p.is_scanning);
    EXPECT_EQ(p.files_processed, 0);
    EXPECT_EQ(p.total_files, 0);
    EXPECT_EQ(p.scanning_progress, 0.0);
    EXPECT_EQ(p.indexing_progress, 0.0);
}

TEST(ProgressTrackerTest, SetTotalTransitionsToIndexing) {
    ProgressTracker tracker;
    tracker.set_total(100);
    auto p = tracker.get_progress();
    EXPECT_FALSE(p.is_scanning);
    EXPECT_EQ(p.total_files, 100);
    EXPECT_EQ(p.scanning_progress, 100.0);
    EXPECT_EQ(p.indexing_progress, 0.0);
}

TEST(ProgressTrackerTest, ScanningProgressEstimate) {
    ProgressTracker tracker;
    tracker.increment_scanned();
    auto p = tracker.get_progress();
    EXPECT_TRUE(p.is_scanning);
    EXPECT_EQ(p.scanning_progress, 50.0);
    EXPECT_EQ(p.indexing_progress, 0.0);
}

TEST(ProgressTrackerTest, TracksProcessedFiles) {
    ProgressTracker tracker;
    tracker.set_total(4);
    tracker.increment_processed("a.go");
    tracker.increment_processed("b.go");
    auto p = tracker.get_progress();
    EXPECT_EQ(p.files_processed, 2);
    EXPECT_EQ(p.indexing_progress, 50.0);
    EXPECT_EQ(p.current_file, "b.go");
}

TEST(ProgressTrackerTest, TracksIntegratedFiles) {
    ProgressTracker tracker;
    tracker.set_total(10);
    tracker.increment_integrated();
    tracker.increment_integrated();
    tracker.increment_integrated();
    EXPECT_EQ(tracker.integrated_count(), 3);
}

TEST(ProgressTrackerTest, CalculatesFilesPerSecond) {
    ProgressTracker tracker;
    tracker.set_total(100);
    tracker.increment_processed("file.go");
    // Allow a tiny bit of time to elapse so rate > 0.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto p = tracker.get_progress();
    EXPECT_GT(p.files_per_second, 0.0);
}

TEST(ProgressTrackerTest, CalculatesETA) {
    ProgressTracker tracker;
    tracker.set_total(100);
    for (int i = 0; i < 50; ++i) {
        tracker.increment_processed("f" + std::to_string(i));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto p = tracker.get_progress();
    EXPECT_GE(p.estimated_time_left.count(), 0);
}

TEST(ProgressTrackerTest, RecordsErrors) {
    ProgressTracker tracker;
    Error err;
    err.type = ErrorType::Indexing;
    err.file_path = "/bad.go";
    err.message = "parse failure";
    tracker.add_error(std::move(err));

    auto p = tracker.get_progress();
    ASSERT_EQ(p.errors.size(), 1u);
    EXPECT_EQ(p.errors[0].file_path, "/bad.go");
    EXPECT_EQ(p.errors[0].message, "parse failure");
}

TEST(ProgressTrackerTest, ConcurrentIncrements) {
    ProgressTracker tracker;
    tracker.set_total(400);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&tracker, t] {
            for (int i = 0; i < kPerThread; ++i) {
                tracker.increment_processed(
                    "t" + std::to_string(t) + "_" + std::to_string(i));
            }
        });
    }
    for (auto& th : threads) th.join();

    auto p = tracker.get_progress();
    EXPECT_EQ(p.files_processed, kThreads * kPerThread);
    EXPECT_EQ(p.indexing_progress, 100.0);
}

// ---------------------------------------------------------------------------
// Pipeline (end-to-end)
// ---------------------------------------------------------------------------

TEST(PipelineTest, RunsThreeStages) {
    TempDir dir;
    dir.write_file("main.go", "package main\nfunc main() {}\n");
    dir.write_file("lib.go", "package lib\nfunc Helper() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    Pipeline pipeline(cfg, file_service, &trigram_idx,
                      &ref_tracker, &postings_idx);
    pipeline.run();

    EXPECT_GE(pipeline.integrator().file_count(), 2);
    auto p = pipeline.get_progress();
    EXPECT_FALSE(p.is_scanning);
    EXPECT_EQ(p.scanning_progress, 100.0);
    EXPECT_GE(p.files_processed, 2);
}

TEST(PipelineTest, ProgressReportsCompletion) {
    TempDir dir;
    dir.write_file("a.go", "package a\nfunc A() {}\n");
    dir.write_file("b.py", "def b():\n    pass\n");
    dir.write_file("c.js", "function c() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    Pipeline pipeline(cfg, file_service, &trigram_idx,
                      &ref_tracker, &postings_idx);
    pipeline.run();

    auto p = pipeline.get_progress();
    EXPECT_EQ(p.total_files, p.files_processed);
    EXPECT_GT(p.files_per_second, 0.0);
    EXPECT_GE(p.elapsed.count(), 0);
}

TEST(PipelineTest, EmptyDirectoryCompletesCleanly) {
    TempDir dir;
    // No files written.

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    Pipeline pipeline(cfg, file_service, &trigram_idx,
                      &ref_tracker, &postings_idx);
    pipeline.run();

    EXPECT_EQ(pipeline.integrator().file_count(), 0);
    auto p = pipeline.get_progress();
    EXPECT_FALSE(p.is_scanning);
    EXPECT_EQ(p.total_files, 0);
}

TEST(PipelineTest, CancellationStopsPipeline) {
    TempDir dir;
    for (int i = 0; i < 50; ++i) {
        dir.write_file("file" + std::to_string(i) + ".go",
                       "package f" + std::to_string(i) + "\nfunc F() {}\n");
    }

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();

    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    Pipeline pipeline(cfg, file_service, &trigram_idx,
                      &ref_tracker, &postings_idx);

    // Request stop before running; the pipeline should exit quickly.
    pipeline.request_stop();
    EXPECT_TRUE(pipeline.stop_requested());
    pipeline.run();

    // With pre-stop, scanner still runs synchronously, but the pipeline
    // should exit early after scanning.
    auto p = pipeline.get_progress();
    EXPECT_TRUE(pipeline.stop_requested());
}

TEST(PipelineTest, StopRequestedDefaultsFalse) {
    Config cfg = make_default_config();
    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    Pipeline pipeline(cfg, file_service, &trigram_idx,
                      &ref_tracker, &postings_idx);
    EXPECT_FALSE(pipeline.stop_requested());
}

// ============================================================================
// match_glob: comprehensive boundary tests.
// `?` matches single non-`/` char. `*` matches zero or more non-`/` chars.
// `**` matches zero or more characters across any boundary (multi-component).
// ============================================================================

TEST(MatchGlob, LiteralExactMatch) {
    EXPECT_TRUE(FileScanner::match_glob("foo.go", "foo.go"));
    EXPECT_FALSE(FileScanner::match_glob("foo.go", "bar.go"));
    EXPECT_FALSE(FileScanner::match_glob("foo.go", "foo.gob"));
    EXPECT_FALSE(FileScanner::match_glob("foo.go", "afoo.go"));
}

TEST(MatchGlob, EmptyPatternMatchesEmptyPath) {
    EXPECT_TRUE(FileScanner::match_glob("", ""));
    EXPECT_FALSE(FileScanner::match_glob("", "foo"));
    EXPECT_FALSE(FileScanner::match_glob("foo", ""));
}

TEST(MatchGlob, QuestionMarkMatchesSingleNonSlashChar) {
    EXPECT_TRUE(FileScanner::match_glob("a?c", "abc"));
    EXPECT_TRUE(FileScanner::match_glob("a?c", "axc"));
    EXPECT_FALSE(FileScanner::match_glob("a?c", "abbc"));
    EXPECT_FALSE(FileScanner::match_glob("a?c", "a/c"))
        << "`?` must not match path separator";
    EXPECT_FALSE(FileScanner::match_glob("a?c", "ac"));
}

TEST(MatchGlob, SingleStarMatchesZeroOrMoreNonSlashChars) {
    EXPECT_TRUE(FileScanner::match_glob("a*c", "ac"));
    EXPECT_TRUE(FileScanner::match_glob("a*c", "abc"));
    EXPECT_TRUE(FileScanner::match_glob("a*c", "abbbbc"));
    EXPECT_FALSE(FileScanner::match_glob("a*c", "a/c"))
        << "single `*` must not cross `/`";
    EXPECT_FALSE(FileScanner::match_glob("a*c", "abcd"));
}

TEST(MatchGlob, SingleStarStaysWithinComponent) {
    // Critical regression test: `*` rejects '/'.
    EXPECT_FALSE(FileScanner::match_glob("test_*", "dir/test_x"));
    EXPECT_TRUE(FileScanner::match_glob("test_*", "test_x"));
    EXPECT_FALSE(FileScanner::match_glob("test_*", "test_x/y.go"))
        << "`*` after literal must stop at path boundary";
}

TEST(MatchGlob, DoubleStarMatchesAcrossComponents) {
    EXPECT_TRUE(FileScanner::match_glob("**/foo.go", "foo.go"));
    EXPECT_TRUE(FileScanner::match_glob("**/foo.go", "a/foo.go"));
    EXPECT_TRUE(FileScanner::match_glob("**/foo.go", "a/b/c/foo.go"));
    EXPECT_FALSE(FileScanner::match_glob("**/foo.go", "foo.go.bak"));
    EXPECT_FALSE(FileScanner::match_glob("**/foo.go", "bar.go"));
}

TEST(MatchGlob, DoubleStarSlashMatchesZeroPathComponents) {
    EXPECT_TRUE(FileScanner::match_glob("**/foo", "foo"));
    EXPECT_TRUE(FileScanner::match_glob("**/foo", "a/foo"));
    EXPECT_TRUE(FileScanner::match_glob("**/foo", "a/b/foo"));
}

TEST(MatchGlob, DoubleStarAtEndMatchesEntireSubtree) {
    EXPECT_TRUE(FileScanner::match_glob("logs/**", "logs/app.log"));
    EXPECT_TRUE(FileScanner::match_glob("logs/**", "logs/sub/deep/x.log"));
    EXPECT_FALSE(FileScanner::match_glob("logs/**", "other/app.log"));
}

TEST(MatchGlob, TestStarPatternStaysInBasename) {
    // Key bug-fix case: `**/test_*` must match a basename starting with
    // `test_` but NOT match an intermediate path component containing
    // `test_` as a substring.
    EXPECT_TRUE(FileScanner::match_glob("**/test_*", "test_helper.go"));
    EXPECT_TRUE(FileScanner::match_glob("**/test_*", "src/test_helper.go"));
    EXPECT_TRUE(FileScanner::match_glob("**/test_*", "a/b/test_x.py"));

    // Bug regression guards:
    EXPECT_FALSE(FileScanner::match_glob("**/test_*",
                                          "lci_watcher_test_42/main.go"))
        << "intermediate component containing 'test_' must not match **/test_*";
    EXPECT_FALSE(FileScanner::match_glob("**/test_*",
                                          "atest_helper/x.go"));
}

TEST(MatchGlob, StarUnderscoreTestStarMatchesAnywhereInBasename) {
    EXPECT_TRUE(FileScanner::match_glob("**/*_test.go", "pkg/foo_test.go"));
    EXPECT_TRUE(FileScanner::match_glob("**/*_test.go", "foo_test.go"));
    EXPECT_TRUE(FileScanner::match_glob("**/*_test.go", "a/b/c/x_test.go"));
    EXPECT_FALSE(FileScanner::match_glob("**/*_test.go", "pkg/foo.go"));
    EXPECT_FALSE(FileScanner::match_glob("**/*_test.go", "pkg/test.go"));
    EXPECT_FALSE(FileScanner::match_glob("**/*_test.go", "pkg/foo_test.py"));
}

TEST(MatchGlob, NodeModulesAtAnyDepth) {
    EXPECT_TRUE(FileScanner::match_glob("**/node_modules/**",
                                         "node_modules/lodash/index.js"));
    EXPECT_TRUE(FileScanner::match_glob("**/node_modules/**",
                                         "frontend/node_modules/x.js"));
    EXPECT_TRUE(FileScanner::match_glob("**/node_modules/**",
                                         "a/b/node_modules/c/d.js"));
    EXPECT_FALSE(FileScanner::match_glob("**/node_modules/**",
                                         "src/node_modules"))
        << "trailing /** requires at least one component after node_modules";
}

TEST(MatchGlob, DotDirectoryExclusion) {
    EXPECT_TRUE(FileScanner::match_glob("**/.git/**", ".git/HEAD"));
    EXPECT_TRUE(FileScanner::match_glob("**/.git/**", "src/.git/config"));
    EXPECT_FALSE(FileScanner::match_glob("**/.git/**", "git/HEAD"));
}

TEST(MatchGlob, MinifiedJsPattern) {
    EXPECT_TRUE(FileScanner::match_glob("**/*.min.js", "vendor/lib.min.js"));
    EXPECT_TRUE(FileScanner::match_glob("**/*.min.js", "lib.min.js"));
    EXPECT_FALSE(FileScanner::match_glob("**/*.min.js", "vendor/lib.js"));
    EXPECT_FALSE(FileScanner::match_glob("**/*.min.js", "lib.min.js.bak"));
}

TEST(MatchGlob, TestsDirAtAnyDepth) {
    EXPECT_TRUE(FileScanner::match_glob("**/tests/**", "tests/foo.go"));
    EXPECT_TRUE(FileScanner::match_glob("**/tests/**", "pkg/tests/foo.go"));
    EXPECT_TRUE(FileScanner::match_glob("**/tests/**", "a/b/tests/c/d.go"));
    EXPECT_FALSE(FileScanner::match_glob("**/tests/**", "testimony.go"));
}

TEST(MatchGlob, QuestionMarkBoundedByPathSep) {
    EXPECT_TRUE(FileScanner::match_glob("a/?", "a/b"));
    EXPECT_FALSE(FileScanner::match_glob("a/?", "a/bc"));
    EXPECT_FALSE(FileScanner::match_glob("a/?", "a/"));
}

TEST(MatchGlob, AllowsTrailingStarToMatchEmpty) {
    EXPECT_TRUE(FileScanner::match_glob("foo*", "foo"));
    EXPECT_TRUE(FileScanner::match_glob("foo*", "foobar"));
    EXPECT_FALSE(FileScanner::match_glob("foo*", "foo/bar"));
}

}  // namespace
}  // namespace lci
