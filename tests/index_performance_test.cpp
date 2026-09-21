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
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_explore.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
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
        // view(), not the content vector: mapped entries hold their bytes
        // in the retained mmap and the vector is legitimately empty.
        ASSERT_FALSE(fc->view().empty())
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

// Random traversal of pagination windows through the real MCP list_symbols
// handler. Two properties, both contention-robust (best-of-N minimum):
//   1. absolute: a random page over a ~700-symbol index costs low
//      single-digit milliseconds at worst (the handler is O(total) per call
//      -- collect + sort -- so this is the per-request tax a paging client
//      pays);
//   2. relative: page cost is offset-INDEPENDENT. A deep window (near the
//      end) must cost about the same as page one; a super-linear ratio
//      means someone made the offset walk quadratic.
TEST(IndexPerformanceRequirements, PaginationRandomTraversalIsFlat) {
    LargeTestProject project(200);
    Config cfg = make_test_config(project.root.string());
    MasterIndex index(cfg);
    ASSERT_TRUE(index.index_directory(project.root.string()));

    auto page_once = [&](int offset) {
        nlohmann::json params = {{"max", 25}, {"offset", offset}};
        auto result = mcp::handle_list_symbols(params, index);
        EXPECT_FALSE(result.is_error);
        return result;
    };
    auto first = nlohmann::json::parse(page_once(0).text);
    const int total = first["total"].get<int>();
    ASSERT_GT(total, 100) << "fixture too small to make deep offsets deep";

    auto bench = [&](auto&& next_offset, int iterations) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            (void)page_once(next_offset(i));
        }
        auto dur = std::chrono::steady_clock::now() - start;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(dur) /
               iterations;
    };
    constexpr int kIterations = 40;
    auto best_of = [&](auto&& next_offset) {
        auto best = bench(next_offset, kIterations);
        for (int rep = 1; rep < 3; ++rep) {
            best = std::min(best, bench(next_offset, kIterations));
        }
        return best;
    };

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> anywhere(0, total - 1);
    auto random_cost = best_of([&](int) { return anywhere(rng); });
    auto shallow_cost = best_of([](int) { return 0; });
    auto deep_cost = best_of([&](int) { return total - 10; });

    double ratio = static_cast<double>(deep_cost.count()) /
                   static_cast<double>(
                       std::max<int64_t>(shallow_cost.count(), 1));
    std::printf(
        "[ PaginationTraversal ] shallow=%lldns deep=%lldns random=%lldns "
        "ratio=%.2f total=%d\n",
        static_cast<long long>(shallow_cost.count()),
        static_cast<long long>(deep_cost.count()),
        static_cast<long long>(random_cost.count()), ratio, total);
    std::fflush(stdout);

    EXPECT_LT(ratio, 4.0)
        << "deep pages cost >> shallow pages: offset handling went "
           "super-linear";
    // O(total) collect+sort per call. The ceiling is a pathology tripwire
    // (accidental O(total^2) walk), not a latency SLO — the ratio above is
    // the real property. A -O0 debug build measures ~30ms/page on a loaded
    // CI runner, so the debug ceiling carries 5x the release one.
#ifdef NDEBUG
    constexpr int64_t kRandomCeilingNs = 20'000'000;
#else
    constexpr int64_t kRandomCeilingNs = 100'000'000;
#endif
    EXPECT_LT(random_cost.count(), kRandomCeilingNs)
        << "random page window cost regressed past the tripwire";
}

// ---------------------------------------------------------------------------
// Postings remove_file is O(file's token count), not O(files sharing a token).
//
// On d84a94a remove_file walked every token's posting vector with a linear
// scan + middle erase to find the removed file's entry. A token present in
// 5000 files (e.g. "function") therefore cost ~5000 element shifts per
// removal regardless of how few tokens the removed file has -- the S3
// update path pays this once per reindexed file.
//
// Shape of the measurement: only remove_file and the re-add that restores
// identical state are timed, inside a bulk window (set_bulk_indexing) so the
// clone-mutate-publish RCU cost -- which is proportional to the WHOLE index
// by design and is not what this guards -- stays out of the timed region.
// This matches the integrator path (S3 update/remove run against the staging
// snapshot). One remove of a 1-token file is faster than a steady_clock
// tick, so each timed region covers `pairs` consecutive remove/re-add pairs
// and the per-pair average is returned; see
// PostingsRemoveFileScalesWithTokenCountNotFileCount for why a single-op
// baseline is unmeasurable (the 2026-09-20 flake grid files=5000:104ns
// files=50:0ns).
// ---------------------------------------------------------------------------
namespace {

// Index `n_files` files, each contributing the hot token "function" plus
// `extra_tokens` distinct tokens, then best-of-5 a region of `pairs`
// consecutive remove/re-add pairs of one representative file (identical
// state at each pair boundary). Returns ns per pair; 0 means the average
// still fell below clock resolution -- callers must treat that as
// unmeasurable, never as a denominator.
long long time_remove_with_hot_token(int n_files, int extra_tokens,
                                     int pairs) {
    auto tokens_for = [](int file_idx, int extra) {
        std::vector<PostingsToken> toks;
        toks.push_back(PostingsToken{"function", 0});
        for (int j = 0; j < extra; ++j) {
            toks.push_back(PostingsToken{
                "tok_" + std::to_string(file_idx) + "_" + std::to_string(j),
                j});
        }
        return toks;
    };
    PostingsIndex index;
    // Bulk window for the whole fixture: keeps the O(snapshot) RCU clone out
    // of both the setup and the timed removals (the clone is a per-publish
    // property of the update path, not what this test guards).
    index.set_bulk_indexing(true);
    for (int i = 0; i < n_files; ++i) {
        index.index_file_pretokenized(static_cast<FileID>(i),
                                      tokens_for(i, extra_tokens));
    }
    const FileID victim = static_cast<FileID>(n_files / 2);
    // Built once, outside every timed region: pair cost must be the two
    // index operations, not per-token string allocation.
    const std::vector<PostingsToken> victim_toks =
        tokens_for(static_cast<int>(victim), extra_tokens);
    // Warmup reps (page faults, allocator warm).
    for (int r = 0; r < pairs; ++r) {
        index.remove_file(victim);
        index.index_file_pretokenized(victim, victim_toks);
    }
    long long best = std::numeric_limits<long long>::max();
    for (int rep = 0; rep < 5; ++rep) {
        auto start = std::chrono::steady_clock::now();
        for (int r = 0; r < pairs; ++r) {
            index.remove_file(victim);
            index.index_file_pretokenized(victim, victim_toks);
        }
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        best = std::min<long long>(best, ns / pairs);
    }
    index.set_bulk_indexing(false);
    return best;
}

// The scaling ratio between the timed 5000-file removal and the 50-file
// baseline. A 1-token file removal from the 50-file index is a hash lookup
// plus one swap-pop: it can take less than one steady_clock tick, which is
// exactly the 2026-09-20 flake grid files=5000:104ns files=50:0ns. A 0ns
// baseline is not a measurement: clamping it to 1 collapses the ratio onto
// hot_5000 itself (104.00 above) and the scaling test reports a false 104x
// regression. nullopt = unmeasurable, the caller must widen the timed region
// or skip -- never substitute a fabricated denominator.
std::optional<double> postings_remove_scaling_ratio(long long hot_5000_ns,
                                                    long long hot_50_ns) {
    if (hot_5000_ns < 0 || hot_50_ns <= 0) return std::nullopt;
    return static_cast<double>(hot_5000_ns) /
           static_cast<double>(hot_50_ns);
}

}  // namespace

TEST(IndexPerformanceRequirements,
     PostingsRemoveScalingRefusesUnmeasurableBaseline) {
    // The observed flake grid (.tman/ctest.log:426-429, 2026-09-20 22:38Z,
    // 2700/2701 otherwise green): 104ns at 5000 files, 0ns at 50 files.
    // An unmeasurable baseline must not yield a ratio EXPECT_LT can fail.
    EXPECT_EQ(postings_remove_scaling_ratio(104, 0), std::nullopt)
        << "a 0ns baseline was clamped to 1ns and reported as a 104x "
           "regression";
    // Measurable baseline: plain ratio, no clamping.
    const auto ok = postings_remove_scaling_ratio(104, 50);
    ASSERT_TRUE(ok.has_value());
    EXPECT_NEAR(*ok, 104.0 / 50.0, 1e-9);
    // A negative reading means the measurement is broken, not merely fast.
    EXPECT_EQ(postings_remove_scaling_ratio(-1, 50), std::nullopt);
}

TEST(IndexPerformanceRequirements, PostingsRemoveFileScalesWithTokenCountNotFileCount) {
    // Removing a 1-token file from an index where "function" is shared by
    // 5000 files must not cost ~5000 scans+shifts more than the same
    // removal from a 50-file index. Pre-fix the ratio is ~50-100x (linear
    // in the hot token's file count); post-fix it is ~1x (bounded by the
    // removed file's own token count).
    //
    // The 50-file baseline (one hash lookup + swap-pop, SSO "function"
    // re-add) fits inside a single steady_clock tick, so a one-shot timing
    // of it measures 0ns -- the 2026-09-20 flake that broke other tasks'
    // full-suite gates with ratio=104.00. Each timed region therefore covers
    // kPairs consecutive remove/re-add pairs and reports the per-pair
    // average. If even that reads 0 on a coarse host, widen the region
    // before drawing any conclusion; only a baseline that is STILL 0 at
    // 20000-pair regions is honestly skipped (conditional, never
    // unconditional), and no clamped 1 ever becomes a denominator.
    constexpr int kPairs = 200;
    int pairs = kPairs;
    long long hot_5000 = time_remove_with_hot_token(5000, 0, pairs);
    long long hot_50 = time_remove_with_hot_token(50, 0, pairs);
    while (hot_50 <= 0 && pairs < 20000) {
        pairs *= 100;
        hot_5000 = time_remove_with_hot_token(5000, 0, pairs);
        hot_50 = time_remove_with_hot_token(50, 0, pairs);
    }
    const auto ratio = postings_remove_scaling_ratio(hot_5000, hot_50);
    std::printf(
        "[ PostingsRemoveScaling ] pairs=%d files=5000:%lldns files=50:%lldns "
        "ratio=%.2f\n",
        pairs, hot_5000, hot_50, ratio.value_or(-1.0));
    std::fflush(stdout);
    if (!ratio.has_value()) {
        GTEST_SKIP() << "50-file remove baseline measured " << hot_50
                     << "ns even at " << pairs
                     << "-pair regions: below this host's clock resolution, "
                        "scaling ratio unmeasurable (no fabricated "
                        "denominator is substituted)";
    }
    EXPECT_LT(*ratio, 5.0)
        << "remove_file cost scaled with the number of files sharing the "
           "hot token (linear posting-vector scan suspected): 5000-file "
        << hot_5000 << "ns vs 50-file " << hot_50 << "ns";
}

}  // namespace
}  // namespace lci
