// Index performance regression tests.
//
// Mirrors the Go reference's TestIndexPerformanceRequirements
// (internal/indexing/performance_validation_test.go) for the parts that
// translate cleanly to the C++ port. The headline guarantee is the
// `File Access Performance` invariant: average lookup time across 1000
// random FileID -> FileContent reads must stay under 50µs on a 100-file
// project, since the store is supposed to serve everything from memory.
//
// Background: the original C++ port held entries in a `std::vector<Entry>`
// inside `FileContentSnapshot` and walked it linearly on every lookup. That
// produced O(n) reads (avg ~78µs at 100 files) versus Go's O(1) `sync.Map`
// lookups. The fix added id_index / path_index hash maps to the snapshot
// (file_content_store.h:39-50), and this test pins the regression so it
// can never silently slip back.

#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/core/portable.h>
#include <lci/indexing/master_index.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "performance_guards.h"

namespace lci {
namespace {

namespace fs = std::filesystem;

/// Generates a synthetic source file for indexing tests. The mix of Go,
/// JavaScript, and Python content matches the Go reference helper
/// `createLargeTestProject` so the per-file size and parser work are
/// comparable.
std::string make_synthetic_source(int idx, int variant) {
    char buf[2048];
    switch (variant) {
        case 0:  // Go
            std::snprintf(buf, sizeof(buf),
                          "package main\n\n"
                          "import (\n\t\"fmt\"\n\t\"errors\"\n)\n\n"
                          "// Function%d processes data\n"
                          "func Function%d() error {\n"
                          "\tdata := processData%d()\n"
                          "\tif err := validateData(data); err != nil {\n"
                          "\t\treturn fmt.Errorf(\"validation failed: %%w\", err)\n"
                          "\t}\n"
                          "\treturn nil\n"
                          "}\n\n"
                          "func processData%d() interface{} {\n"
                          "\treturn struct{ ID int }{ID: %d}\n"
                          "}\n",
                          idx, idx, idx, idx, idx);
            break;
        case 1:  // JS
            std::snprintf(buf, sizeof(buf),
                          "// Module %d\n"
                          "class Component%d {\n"
                          "  constructor() { this.id = %d; }\n"
                          "  process() { return this.processData(); }\n"
                          "  processData() { return { id: this.id }; }\n"
                          "}\n"
                          "function createComponent%d() { return new Component%d(); }\n"
                          "module.exports = { Component%d, createComponent%d };\n",
                          idx, idx, idx, idx, idx, idx, idx);
            break;
        default:  // Python
            std::snprintf(buf, sizeof(buf),
                          "# Module %d\n"
                          "import time\n\n"
                          "class DataProcessor%d:\n"
                          "    def __init__(self):\n"
                          "        self.id = %d\n"
                          "    def process(self):\n"
                          "        return self.process_data()\n"
                          "    def process_data(self):\n"
                          "        return {'id': self.id, 't': time.time()}\n\n"
                          "def create_processor_%d():\n"
                          "    return DataProcessor%d()\n",
                          idx, idx, idx, idx, idx);
            break;
    }
    return std::string(buf);
}

/// Owns a temp directory of synthetic files. Deletes the tree on
/// destruction so leaked test runs don't pile up.
struct LargeTestProject {
    fs::path root;

    explicit LargeTestProject(int file_count) {
        auto stamp =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        root = fs::temp_directory_path() /
               ("lci_perf_" + std::to_string(stamp) + "_" +
                std::to_string(static_cast<unsigned>(lci::portable::process_id())));
        fs::create_directories(root);

        for (int i = 0; i < file_count; ++i) {
            auto subdir = root / ("pkg" + std::to_string(i / 10));
            fs::create_directories(subdir);

            const char* ext = nullptr;
            switch (i % 3) {
                case 0: ext = ".go"; break;
                case 1: ext = ".js"; break;
                default: ext = ".py"; break;
            }
            auto path = subdir / ("file" + std::to_string(i) + ext);

            std::ofstream out(path, std::ios::binary);
            out << make_synthetic_source(i, i % 3);
        }
    }

    ~LargeTestProject() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    LargeTestProject(const LargeTestProject&) = delete;
    LargeTestProject& operator=(const LargeTestProject&) = delete;
};

Config make_test_config(const std::string& root) {
    Config cfg = make_default_config();
    cfg.project.root = root;
    cfg.index.max_file_size = 10 * 1024 * 1024;
    cfg.index.respect_gitignore = false;
    cfg.search.max_results = 100;
    cfg.include = {"**/*.go", "**/*.js", "**/*.py"};
    cfg.exclude = {};
    return cfg;
}

// ---------------------------------------------------------------------------
// File access performance: average lookup time must stay under 50µs.
//
// Mirrors Go's `t.Run("File Access Performance", ...)` block at
// internal/indexing/performance_validation_test.go:209-229. The Go test
// asserts `avgTime.Nanoseconds() < 50000` over 1000 iterations on a
// 100-file project; we use the same shape with the test helpers'
// PerformanceGuard so CI variability is automatically scaled (1.5x on CI
// or single-CPU machines, see helpers/performance_guards.h:46-56).
// ---------------------------------------------------------------------------
TEST(IndexPerformanceRequirements, FileAccessUnder50Microseconds) {
    constexpr int kFileCount = 100;
    constexpr int kIterations = 1000;

    LargeTestProject project(kFileCount);
    Config cfg = make_test_config(project.root.string());

    MasterIndex index(cfg);
    ASSERT_TRUE(index.index_directory(project.root.string()))
        << "index_directory failed for synthetic 100-file project";

    auto file_ids = index.get_all_file_ids();
    ASSERT_FALSE(file_ids.empty()) << "indexer reported zero files";

    using namespace std::chrono;
    using lci::testing::PerformanceGuard;
    using lci::testing::PerformanceScaler;
    using lci::testing::PerformanceThreshold;

    PerformanceScaler scaler;
    auto scaled_ns = [&](int64_t ns_base) {
        return nanoseconds{
            static_cast<int64_t>(static_cast<double>(ns_base) *
                                 scaler.scale_duration(1.0))};
    };

    PerformanceGuard guard;
    // Average <50µs is the headline invariant from the Go reference.
    // P95/P99 thresholds tolerate the occasional cache miss but still cap
    // tail latency well below where an O(n) scan over 100 entries would land.
    guard.add_threshold("file_access", PerformanceThreshold{
                                            .max_duration = scaled_ns(50'000),
                                            .p50_threshold = scaled_ns(20'000),
                                            .p95_threshold = scaled_ns(80'000),
                                            .p99_threshold = scaled_ns(150'000),
                                        });

    const auto& store = index.file_content_store();
    size_t cursor = 0;
    guard.measure_n("file_access", kIterations, [&] {
        FileID fid = file_ids[cursor % file_ids.size()];
        ++cursor;

        auto fc = store.get_file(fid);
        ASSERT_NE(fc, nullptr) << "file_content_store missing FileID " << fid;
        // Mirrors Go's `assert.NotEmpty(t, fileInfo.Content)` so the
        // optimizer can't elide the lookup.
        ASSERT_FALSE(fc->content.empty())
            << "FileContent for FileID " << fid << " unexpectedly empty";
    });

    auto result = guard.check("file_access");
    // Always emit the timing summary so CI captures it whether or not the
    // assertion passes. RecordProperty also makes the numbers available
    // to gtest XML consumers that look for performance trends.
    auto avg_ns = result.avg_duration.count();
    auto p50_ns = result.p50.count();
    auto p95_ns = result.p95.count();
    auto p99_ns = result.p99.count();
    ::testing::Test::RecordProperty("file_access_avg_ns", static_cast<int>(avg_ns));
    ::testing::Test::RecordProperty("file_access_p50_ns", static_cast<int>(p50_ns));
    ::testing::Test::RecordProperty("file_access_p95_ns", static_cast<int>(p95_ns));
    ::testing::Test::RecordProperty("file_access_p99_ns", static_cast<int>(p99_ns));
    std::printf("[ FileAccessPerf ] files=%zu iterations=%d "
                "avg=%lldns p50=%lldns p95=%lldns p99=%lldns\n",
                file_ids.size(), kIterations,
                static_cast<long long>(avg_ns),
                static_cast<long long>(p50_ns),
                static_cast<long long>(p95_ns),
                static_cast<long long>(p99_ns));
    std::fflush(stdout);
    if (!result.passed) {
        std::string violations;
        for (const auto& v : result.violations) {
            violations += "\n  - " + v;
        }
        FAIL() << "file_access threshold breached:" << violations;
    }
}

// ---------------------------------------------------------------------------
// Lookup is O(1) in file count: doubling the file count must not double
// the average lookup time. This is a structural guard against future code
// regressing back to a linear scan.
// ---------------------------------------------------------------------------
TEST(IndexPerformanceRequirements, FileAccessScalesSublinearly) {
    auto bench = [](int file_count, int iterations) {
        LargeTestProject project(file_count);
        Config cfg = make_test_config(project.root.string());
        MasterIndex index(cfg);
        EXPECT_TRUE(index.index_directory(project.root.string()));
        auto ids = index.get_all_file_ids();
        EXPECT_FALSE(ids.empty());

        const auto& store = index.file_content_store();
        // Warmup so we measure steady-state.
        for (int i = 0; i < 100; ++i) {
            (void)store.get_file(ids[i % ids.size()]);
        }

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            auto fc = store.get_file(ids[i % ids.size()]);
            EXPECT_NE(fc, nullptr);
        }
        auto dur = std::chrono::steady_clock::now() - start;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(dur) /
               iterations;
    };

    constexpr int kIterations = 5000;
    // Under parallel ctest (`-j`) every TEST case is its own process and the
    // CPU is oversubscribed, so a single timed run can be inflated by the
    // scheduler descheduling this thread mid-measurement. Take the MIN over a
    // few repetitions: the fastest run is the least-contended, closest to the
    // true single-threaded cost. A structural O(n) regression persists across
    // every repetition (the min still shows ~20x), so best-of-N filters
    // scheduling noise without hiding the signal.
    auto best_of = [&](int file_count) {
        auto best = bench(file_count, kIterations);
        for (int rep = 1; rep < 3; ++rep) {
            best = std::min(best, bench(file_count, kIterations));
        }
        return best;
    };
    // Wide spread (50 vs 1000) so a real O(n) regression produces a ~20x
    // ratio; hash-map lookups stay near 1x. Narrower spreads (e.g. 50 vs 200)
    // hide the regression on hot CPUs because a 200-entry linear scan still
    // fits in L1 and reads in ~1µs.
    auto avg_small = best_of(50);
    auto avg_large = best_of(1000);

    double ratio = static_cast<double>(avg_large.count()) /
                   static_cast<double>(std::max<int64_t>(avg_small.count(), 1));
    std::printf("[ FileAccessScales ] avg_50=%lldns avg_1000=%lldns ratio=%.2f\n",
                static_cast<long long>(avg_small.count()),
                static_cast<long long>(avg_large.count()), ratio);
    std::fflush(stdout);
    // A linear-scan implementation at n=1000 averages ~20x the cost of
    // n=50. Hash-map lookups stay near 1x; a ratio above 4x reliably
    // indicates the O(n) regression has returned (with 4x leaving plenty
    // of slack for cache effects and CI noise).
    EXPECT_LT(ratio, 4.0) << "lookup time scales near-linearly with file count "
                             "(O(n) regression suspected): n=50 avg "
                          << avg_small.count() << "ns, n=1000 avg "
                          << avg_large.count() << "ns";
}

}  // namespace
}  // namespace lci
