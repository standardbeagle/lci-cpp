#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_explore.h>
#include <lci/mcp/handlers_index.h>
#include <lci/mcp/server.h>
#include <lci/symbol.h>

#include <nlohmann/json.hpp>

#include "test_git.h"
#include "unique_temp.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

#include <sys/wait.h>
#include <unistd.h>

// =============================================================================
// Scoped heap-allocation counter.
//
// lci_tests ships no other operator new override, so replacing it process-wide
// here is safe: every override below just forwards to malloc/free and bumps a
// counter when counting is armed (off by default). The list_symbols scaling
// test arms it around one handler call to prove the filter loop allocates
// nothing per symbol (P3: allocations per call must not scale with index size).
// =============================================================================

namespace {
std::atomic<bool> g_count_allocs{false};
std::atomic<long long> g_alloc_count{0};

void record_alloc_if_armed() {
    if (g_count_allocs.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
}
}  // namespace

void* operator new(std::size_t n) {
    record_alloc_if_armed();
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    record_alloc_if_armed();
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    record_alloc_if_armed();
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    record_alloc_if_armed();
    return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

namespace lci {
namespace mcp {
namespace {

// =============================================================================
// Test fixture with a populated index
// =============================================================================

class ExploreIndexTestFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        // Create temp directory with test source files
        tmp_dir_ = lci::test::unique_temp_dir("lci_explore_index_test_");
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);

        write_file("main.go",
                   "package main\n"
                   "\n"
                   "import \"fmt\"\n"
                   "\n"
                   "// HandleRequest processes incoming HTTP requests.\n"
                   "func HandleRequest(method string, path string) string {\n"
                   "    result := processPath(path)\n"
                   "    return fmt.Sprintf(\"%s: %s\", method, result)\n"
                   "}\n"
                   "\n"
                   "func processPath(path string) string {\n"
                   "    return path\n"
                   "}\n"
                   "\n"
                   "type Server struct {\n"
                   "    Port int\n"
                   "    Host string\n"
                   "}\n"
                   "\n"
                   "func (s *Server) Start() error {\n"
                   "    return nil\n"
                   "}\n"
                   "\n"
                   "var MaxConnections = 100\n");

        write_file("util.go",
                   "package main\n"
                   "\n"
                   "func helperFunc(input string) string {\n"
                   "    return input\n"
                   "}\n"
                   "\n"
                   "type Config struct {\n"
                   "    Timeout int\n"
                   "}\n");

        Config config;
        config.project.root = tmp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(tmp_dir_.string());
    }

    void TearDown() override {
        indexer_.reset();
        std::filesystem::remove_all(tmp_dir_);
    }

    void write_file(const std::string& name, const std::string& content) {
        auto path = tmp_dir_ / name;
        std::ofstream out(path);
        out << content;
    }

    std::filesystem::path tmp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

// =============================================================================
// list_symbols tests
// =============================================================================

// kind defaults to "all" — a bare call lists every symbol instead of
// bouncing agents off a required-param error (benchmark traces showed
// agents retrying exactly this shape).
TEST_F(ExploreIndexTestFixture, ListSymbolsDefaultsToAllKinds) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_list_symbols(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto j = nlohmann::json::parse(result.text);
    EXPECT_GT(j["total"].get<int>(), 0);
}

// Full pagination-edge coverage (shared semantics in lci/pagination.h).
// Windows must tile the result set exactly: no overlap, no gap, has_more
// exact at every boundary, and degenerate inputs (max=0, offset past end,
// negative offset) resolve to the documented normalization.
TEST_F(ExploreIndexTestFixture, ListSymbolsPaginationEdges) {
    auto call = [&](nlohmann::json params) {
        auto result = handle_list_symbols(params, *indexer_);
        EXPECT_FALSE(result.is_error) << result.text;
        return nlohmann::json::parse(result.text);
    };

    const auto all = call({{"max", 500}});
    const int total = all["total"].get<int>();
    ASSERT_GT(total, 2) << "fixture must hold enough symbols to paginate";
    ASSERT_EQ(all["showing"].get<int>(), total);
    EXPECT_FALSE(all["has_more"].get<bool>());

    // max=0 means "unset" -> default window (50), NOT a one-element page.
    const auto zero_max = call({{"max", 0}});
    EXPECT_EQ(zero_max["showing"].get<int>(), std::min(total, 50));

    // Negative offset normalizes to 0 -- identical to the first page.
    const auto neg_off = call({{"max", 2}, {"offset", -7}});
    EXPECT_EQ(neg_off["showing"].get<int>(), 2);
    EXPECT_EQ(neg_off["symbols"][0], all["symbols"][0]);
    EXPECT_TRUE(neg_off["has_more"].get<bool>());

    // Walk the whole set in windows of 2: exact tiling, has_more flips
    // false exactly on the window that exhausts the set.
    std::vector<nlohmann::json> seen;
    for (int offset = 0; offset < total;) {
        auto page = call({{"max", 2}, {"offset", offset}});
        EXPECT_EQ(page["total"].get<int>(), total);
        const int shown = page["showing"].get<int>();
        ASSERT_GT(shown, 0);
        ASSERT_LE(shown, 2);
        for (const auto& s : page["symbols"]) seen.push_back(s);
        offset += shown;
        EXPECT_EQ(page["has_more"].get<bool>(), offset < total)
            << "offset now " << offset << " of " << total;
    }
    ASSERT_EQ(static_cast<int>(seen.size()), total);
    for (int i = 0; i < total; ++i) {
        EXPECT_EQ(seen[i], all["symbols"][i]) << "window tiling broke at "
                                              << i;
    }

    // Offset exactly at / past the end: empty page, has_more false.
    for (int offset : {total, total + 5}) {
        auto page = call({{"max", 2}, {"offset", offset}});
        EXPECT_EQ(page["showing"].get<int>(), 0);
        EXPECT_TRUE(page["symbols"].empty());
        EXPECT_FALSE(page["has_more"].get<bool>());
    }
}

TEST_F(ExploreIndexTestFixture, ListSymbolsFunctions) {
    nlohmann::json params = nlohmann::json::object();
    params["kind"] = "func";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("symbols"));
    EXPECT_TRUE(j.contains("total"));
    EXPECT_TRUE(j.contains("showing"));
    EXPECT_TRUE(j.contains("has_more"));

    // Functions may or may not be found depending on Go parser availability
    EXPECT_GE(j["total"].get<int>(), 0);
}

TEST_F(ExploreIndexTestFixture, ListSymbolsAll) {
    nlohmann::json params = nlohmann::json::object();
    params["kind"] = "all";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_GE(j["total"].get<int>(), 0);
}

TEST_F(ExploreIndexTestFixture, ListSymbolsPagination) {
    nlohmann::json params;
    params["kind"] = "all";
    params["max"] = 2;
    params["offset"] = 0;
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_LE(j["showing"].get<int>(), 2);

    int total = j["total"].get<int>();
    if (total > 2) {
        EXPECT_TRUE(j["has_more"].get<bool>());

        // Page 2
        params["offset"] = 2;
        auto r2 = handle_list_symbols(params, *indexer_);
        auto j2 = nlohmann::json::parse(r2.text);
        EXPECT_GE(j2["showing"].get<int>(), 0);
    }
}

TEST_F(ExploreIndexTestFixture, ListSymbolsWithNameFilter) {
    nlohmann::json params;
    params["kind"] = "func";
    params["name"] = "Handle";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    for (const auto& sym : j["symbols"]) {
        auto name = sym["name"].get<std::string>();
        // Case-insensitive substring
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(),
                       lower_name.begin(), ::tolower);
        EXPECT_TRUE(lower_name.find("handle") != std::string::npos);
    }
}

TEST_F(ExploreIndexTestFixture, ListSymbolsSortByName) {
    nlohmann::json params;
    params["kind"] = "all";
    params["sort"] = "name";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    auto& syms = j["symbols"];
    for (size_t i = 1; i < syms.size(); ++i) {
        EXPECT_LE(syms[i - 1]["name"].get<std::string>(),
                  syms[i]["name"].get<std::string>());
    }
}

TEST_F(ExploreIndexTestFixture, ListSymbolsIncludeAll) {
    nlohmann::json params;
    params["kind"] = "func";
    params["include"] = "all";
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    if (!j["symbols"].empty()) {
        auto& first = j["symbols"][0];
        // "all" includes ids and signature
        EXPECT_TRUE(first.contains("object_id"));
    }
}

TEST_F(ExploreIndexTestFixture, ListSymbolsMaxClamped) {
    nlohmann::json params;
    params["kind"] = "all";
    params["max"] = 9999;
    auto result = handle_list_symbols(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_LE(j["showing"].get<int>(), 500);
}

// Pins the exact filter semantics of list_symbols so the hoisted-filter
// refactor (params read once, allocation-free compares) cannot drift the
// result set: each case asserts the full name multiset, not a count.
TEST_F(ExploreIndexTestFixture, ListSymbolsFilterSemanticsPinned) {
    auto names_of = [&](const nlohmann::json& params) {
        auto result = handle_list_symbols(params, *indexer_);
        EXPECT_FALSE(result.is_error) << result.text;
        auto j = nlohmann::json::parse(result.text);
        std::vector<std::string> names;
        for (const auto& s : j["symbols"]) {
            names.push_back(s["name"].get<std::string>());
        }
        std::sort(names.begin(), names.end());
        return names;
    };

    // Case-insensitive substring on name.
    EXPECT_EQ(names_of({{"kind", "all"}, {"name", "handle"}, {"max", 500}}),
              (std::vector<std::string>{"HandleRequest"}));
    EXPECT_EQ(names_of({{"kind", "all"}, {"name", "PATH"}, {"max", 500}}),
              (std::vector<std::string>{"processPath"}));

    // Receiver filter: case-insensitive equality on the receiver type.
    EXPECT_EQ(
        names_of({{"kind", "all"}, {"receiver", "server"}, {"max", 500}}),
        (std::vector<std::string>{"Start"}));
    EXPECT_EQ(
        names_of({{"kind", "all"}, {"receiver", "nosuch"}, {"max", 500}}),
        (std::vector<std::string>{}));

    // Exported visibility filter, both directions.
    EXPECT_EQ(names_of({{"kind", "var"}, {"exported", true}, {"max", 500}}),
              (std::vector<std::string>{"MaxConnections"}));
    auto unexported_funcs =
        names_of({{"kind", "func"}, {"exported", false}, {"max", 500}});
    EXPECT_EQ(unexported_funcs,
              (std::vector<std::string>{"helperFunc", "processPath"}));

    // Parameter-count filter.
    EXPECT_EQ(names_of({{"kind", "func"}, {"min_params", 2}, {"max", 500}}),
              (std::vector<std::string>{"HandleRequest"}));

    // Flag filter: no symbol in this fixture carries the method flag
    // (the Go extractor records receivers via receiver_type, not
    // function_flags) — pin the observed empty result so the refactor
    // cannot silently change flag semantics either.
    EXPECT_EQ(
        names_of({{"kind", "func"}, {"flags", "method"}, {"max", 500}}),
        (std::vector<std::string>{}));

    // Combined filters intersect.
    EXPECT_EQ(names_of({{"kind", "func"},
                        {"name", "o"},
                        {"exported", false},
                        {"max", 500}}),
              (std::vector<std::string>{"processPath"}));
}

// =============================================================================
// list_symbols read-path scaling (P3 perf).
//
// The filter loop walks EVERY indexed symbol on each list_symbols call. Two
// properties are pinned:
//   1. allocations per call do not grow with symbol count (1k vs 10k, same
//      file count): the loop may not copy or lower anything per symbol.
//   2. latency stays linear in symbol count: a 10x index costs < 12x time
//      (best-of-5 interleaved — relative, never absolute ns).
// Corpora share a file count so the measured dimension is symbols, and file
// paths exceed SSO so a per-symbol path copy would hit the heap and be
// counted.
// =============================================================================

class ListSymbolsScalingFixture : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        small_ = make_scaled_corpus(4, 250);   // 1 000 functions
        big_ = make_scaled_corpus(4, 2500);    // 10 000 functions
    }
    static void TearDownTestSuite() {
        std::filesystem::remove_all(small_->dir);
        std::filesystem::remove_all(big_->dir);
        small_.reset();
        big_.reset();
    }

    struct ScaledCorpus {
        std::filesystem::path dir;
        std::unique_ptr<MasterIndex> indexer;
    };

    // Root-relative path deliberately longer than the 15-char SSO buffer.
    static std::string scaled_file_path(int file) {
        return "deeply/nested/source/dirs/beyond/short/string/opt/pkg" +
               std::to_string(file) + "/file_" + std::to_string(file) +
               ".go";
    }

    static std::unique_ptr<ScaledCorpus> make_scaled_corpus(
        int files, int syms_per_file) {
        auto corpus = std::make_unique<ScaledCorpus>();
        corpus->dir = lci::test::unique_temp_dir("lci_ls_scale_");
        std::filesystem::remove_all(corpus->dir);
        std::filesystem::create_directories(corpus->dir);
        for (int f = 0; f < files; ++f) {
            auto path = corpus->dir / scaled_file_path(f);
            std::filesystem::create_directories(path.parent_path());
            std::string content = "package pkg" + std::to_string(f) + "\n";
            content.reserve(content.size() +
                            static_cast<size_t>(syms_per_file) * 30);
            for (int i = 0; i < syms_per_file; ++i) {
                content += "func fn" + std::to_string(i) + "() { _ = 1 }\n";
            }
            std::ofstream out(path);
            out << content;
        }
        Config config;
        config.project.root = corpus->dir.string();
        corpus->indexer = std::make_unique<MasterIndex>(config);
        corpus->indexer->index_directory(corpus->dir.string());
        return corpus;
    }

    inline static std::unique_ptr<ScaledCorpus> small_;
    inline static std::unique_ptr<ScaledCorpus> big_;
};

// The filter loop walks every indexed symbol on each call. Allocations per
// call must not grow with symbol count: 10k symbols may not allocate ~10x
// what 1k allocates (the old loop copied the file path into every row — a
// malloc per symbol, invisible to a latency-only check but the defect the
// task names). Counted via the process-wide operator new hook above, armed
// only around the handler call. This is the AUTHORITATIVE, load-independent
// carrier of the per-symbol-heap regression class that 7031c53 removed; the
// wall-clock sibling below is a coarse backstop only, because a timing ratio
// cannot separate the regression from the fix under parallel load (see that
// test).
TEST_F(ListSymbolsScalingFixture,
       ListSymbolsAllocationsDoNotScaleWithSymbolCount) {
    auto allocs_for_list = [](MasterIndex& idx) {
        nlohmann::json params = nlohmann::json::object();
        params["max"] = 10;
        (void)handle_list_symbols(params, idx);  // warm-up (allocates freely)
        const long long before =
            g_alloc_count.load(std::memory_order_relaxed);
        g_count_allocs.store(true, std::memory_order_relaxed);
        auto result = handle_list_symbols(params, idx);
        g_count_allocs.store(false, std::memory_order_relaxed);
        EXPECT_FALSE(result.is_error) << result.text;
        auto j = nlohmann::json::parse(result.text);
        EXPECT_GT(j["total"].get<int>(), 0);
        return g_alloc_count.load(std::memory_order_relaxed) - before;
    };
    long long allocs_1k = allocs_for_list(*small_->indexer);
    long long allocs_10k = allocs_for_list(*big_->indexer);
    std::printf("[ ListSymbolsAlloc ] 1k=%lld 10k=%lld\n", allocs_1k,
                allocs_10k);
    EXPECT_LT(allocs_10k, allocs_1k * 2)
        << "allocations per list_symbols scale with symbol count: "
        << allocs_1k << " @1k vs " << allocs_10k << " @10k";
    EXPECT_LT(allocs_10k - allocs_1k, 200)
        << "filter loop allocates per symbol: " << allocs_1k
        << " @1k vs " << allocs_10k << " @10k";
}

// Coarse superlinear-latency backstop: 10x symbols must not cost a grossly
// superlinear multiple of wall time (a relative assertion only — never an
// absolute ns bound). This is NOT the regression detector. The precise
// per-symbol-heap shape that 7031c53 removed is carried load-independently and
// deterministically by ListSymbolsAllocationsDoNotScaleWithSymbolCount above
// (the operator-new counter reads ~186 vs ~10 189 allocs on the fix vs the
// regression — a >50x gap no scheduler noise can bridge). A wall-clock ratio
// CANNOT separate regression from fix on a shared suite: the two measured only
// 10.69x vs 13.26x on a quiet box (7031c53), a margin smaller than ordinary
// jitter, and the previous best-of-5 / 12.0 tripped under `ctest -j4` when one
// scheduling stall on the ~3.5ms 10k sample — while the ~0.3ms 1k sample
// caught a clean run — pushed the ratio to 13.62. Wall-clock is therefore kept
// only as a quadratric-blowup net: interleave many rounds so each side's
// minimum converges on its uncontended floor where one exists, and hold the
// bound above the largest contention-inflated reading rather than above the
// quiet mean. Quiet-machine baseline here: best-of-25 floor ~11x (12-core box);
// under sustained external load the ratio was measured up to ~17x, so the bound
// is set at 25x — ~2.3x the quiet floor and ~47% clear of the worst load spike,
// yet far below the ~100x a genuine O(n^2) collect walk would cost.
TEST_F(ListSymbolsScalingFixture,
       ListSymbolsTenfoldSymbolsScaleLinearly) {
    auto time_list_once = [](MasterIndex& idx) {
        nlohmann::json params = nlohmann::json::object();
        params["max"] = 10;
        auto start = std::chrono::steady_clock::now();
        auto result = handle_list_symbols(params, idx);
        auto ns = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
        EXPECT_FALSE(result.is_error) << result.text;
        return ns;
    };
    for (int warm = 0; warm < 3; ++warm) {  // retire cold-cache/branch outliers
        (void)time_list_once(*small_->indexer);
        (void)time_list_once(*big_->indexer);
    }
    long long best_small = std::numeric_limits<long long>::max();
    long long best_big = std::numeric_limits<long long>::max();
    for (int round = 0; round < 25; ++round) {  // interleave: equal exposure
        best_small = std::min(best_small, time_list_once(*small_->indexer));
        best_big = std::min(best_big, time_list_once(*big_->indexer));
    }
    // best_small is a single list_symbols call over a 1k-symbol index
    // (collect + sort + JSON, ~ms), an absolute reading that cannot measure
    // 0ns; the clamp never substitutes a fabricated denominator.
    double ratio = static_cast<double>(best_big) /
                   static_cast<double>(std::max<long long>(best_small, 1));
    std::printf("[ ListSymbolsLatency ] 1k=%.3fus 10k=%.3fus ratio=%.2f\n",
                best_small / 1e3, best_big / 1e3, ratio);
    EXPECT_LT(ratio, 25.0)
        << "10x symbols cost " << ratio << "x time (1k=" << best_small
        << "ns 10k=" << best_big << "ns)";
}

namespace {

// Mean per-call latency (microseconds) of a batch of k_calls list_symbols
// calls over `idx`, max=10 page (the shape every list_symbols scaling guard
// uses). A single call over a 10k-symbol index already costs a few
// milliseconds — above the scheduler quantum — so a modest batch widens each
// sample across many switch windows and the best-of-rounds minimum, taken
// inside an interleaved loop, drives each side to its uncontended floor even
// under `ctest -j4` contention. The handler result is checked but not parsed
// inside the timed loop: JSON parse cost is symmetric between corpora and
// would only inflate the numbers without sharpening the ratio.
double batch_list_symbols_us(MasterIndex& idx, int k_calls) {
    nlohmann::json params = nlohmann::json::object();
    params["max"] = 10;
    (void)handle_list_symbols(params, idx);  // retire cold-start allocations
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < k_calls; ++i) {
        EXPECT_FALSE(handle_list_symbols(params, idx).is_error);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count() /
           static_cast<double>(k_calls);
}

}  // namespace

// The list_symbols surface's LINEAR-IN-FILES negative control — the throat
// the symbol-scaling guards above cannot have. Those hold the file count
// constant (4 vs 4) and vary symbols, so a per-file corpus walk is invisible
// to them by construction; and the O(n^2) collect walk they DO reject against
// is a superlinear-SYMBOLS tripwire, not the file-count regression this task's
// acceptance names ("must still FAIL against a deliberately linear-in-files
// regression"). Here the two corpora share an identical total symbol count
// (10 000) but differ 10x in FILE count (10 files x 1000 vs 100 files x 100).
// The correct collect loop costs ~O(total symbols) per call — the outer file
// loop does only O(1) work per file — so latency must be flat in file count
// and the true ratio is ~1. A genuine linear-in-files regression (a corpus
// walk repeated per file, the exact shape get_entry_point_dependencies used
// to have) reads ~10x: the per-file scan runs 10x more often on the
// 100-file corpus while the symbol work is unchanged. Threshold 2.0 sits
// between the two, unchanged from the get_context guard, and is a RELATIVE
// bound only — never an absolute ns/µs SLO. Measurement is hardened the same
// way (interleaved batched samples, best-of-rounds minimum), and an
// unmeasurable small-side floor is skipped rather than fabricated into a
// denominator (index_performance_test.cpp's postings_remove_scaling_ratio
// rule). RED/GREEN evidence in the commit message.
TEST_F(ListSymbolsScalingFixture,
       ListSymbolsTenfoldFilesFlatInFileCount) {
    auto file_small = make_scaled_corpus(10, 1000);   // 10 files, 10 000 syms
    auto file_big = make_scaled_corpus(100, 100);     // 100 files, 10 000 syms
    ASSERT_TRUE(file_small != nullptr);
    ASSERT_TRUE(file_big != nullptr);

    // Pin the corpus invariant once, outside the timed loop: both sides walk
    // the SAME 10 000 symbols, only the file count differs (10x). If this
    // ever breaks the ratio below would compare apples to oranges.
    {
        nlohmann::json params = nlohmann::json::object();
        params["max"] = 1;
        EXPECT_EQ(nlohmann::json::parse(
                      handle_list_symbols(params, *file_small->indexer).text)
                      ["total"]
                          .get<int>(),
                  10000)
            << "10-file corpus must carry 10 000 symbols";
        EXPECT_EQ(nlohmann::json::parse(
                      handle_list_symbols(params, *file_big->indexer).text)
                      ["total"]
                          .get<int>(),
                  10000)
            << "100-file corpus must carry 10 000 symbols";
    }

    // A few-ms-per-call measurement: a 20-call batch (~50ms) spans ~50
    // scheduler windows, and 20 interleaved rounds let each side's minimum
    // settle on its uncontended floor under load. Kept far smaller than the
    // get_context guard's 512-call batch precisely because this per-call cost
    // is already ~300x larger.
    constexpr int kBatch = 20;
    constexpr int kRounds = 20;
    for (int warm = 0; warm < 3; ++warm) {  // retire cold-cache/branch outliers
        (void)batch_list_symbols_us(*file_small->indexer, 1);
        (void)batch_list_symbols_us(*file_big->indexer, 1);
    }
    double best_small = std::numeric_limits<double>::infinity();
    double best_big = std::numeric_limits<double>::infinity();
    for (int round = 0; round < kRounds; ++round) {  // interleave: equal exposure
        best_small = std::min(
            best_small, batch_list_symbols_us(*file_small->indexer, kBatch));
        best_big =
            std::min(best_big, batch_list_symbols_us(*file_big->indexer, kBatch));
    }
    // A 0µs/call floor is below clock resolution, not a fast measurement: it
    // would collapse the ratio onto best_big alone and report a false
    // regression. Widen the batch before concluding; skip honestly if still 0,
    // never substitute a fabricated denominator.
    if (best_small <= 0.0) {
        std::error_code ec;
        std::filesystem::remove_all(file_small->dir, ec);
        std::filesystem::remove_all(file_big->dir, ec);
        GTEST_SKIP() << "10-file list_symbols floor measured " << best_small
                     << "us/call: below this host's clock resolution, "
                        "scaling ratio unmeasurable (no fabricated "
                        "denominator substituted)";
    }
    double ratio = best_big / best_small;
    std::printf(
        "[ ListSymbolsFileScaling ] 10file=%.3fus/call 100file=%.3fus/call "
        "ratio=%.2f\n",
        best_small, best_big, ratio);
    std::fflush(stdout);
    EXPECT_LT(ratio, 2.0)
        << "list_symbols cost scaled with file count, not symbol count: "
        << "10x files at constant 10k symbols cost " << best_big
        << "us/call vs " << best_small << "us/call (per-file scan suspected)";

    std::error_code ec;
    std::filesystem::remove_all(file_small->dir, ec);
    std::filesystem::remove_all(file_big->dir, ec);
}

// =============================================================================
// inspect_symbol tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, InspectSymbolRequiresNameOrId) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_inspect_symbol(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["error"].get<std::string>().find("name") !=
                std::string::npos);
}

TEST_F(ExploreIndexTestFixture, InspectSymbolByName) {
    nlohmann::json params;
    params["name"] = "HandleRequest";
    auto result = handle_inspect_symbol(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("symbols"));
    EXPECT_TRUE(j.contains("count"));

    // If the symbol was found, verify structure
    if (j["count"].get<int>() > 0) {
        auto& first = j["symbols"][0];
        EXPECT_EQ(first["name"].get<std::string>(), "HandleRequest");
        EXPECT_TRUE(first.contains("object_id"));
        EXPECT_TRUE(first.contains("type"));
        EXPECT_TRUE(first.contains("file"));
        EXPECT_TRUE(first.contains("line"));
        EXPECT_TRUE(first.contains("is_exported"));
        ASSERT_TRUE(first.contains("source_excerpt"));
        EXPECT_LE(first["source_excerpt"]["lines"].size(), 12u);
        ASSERT_TRUE(first.contains("source_hint"));
        EXPECT_NE(first["source_hint"].get<std::string>().find(
                      "read the file only"),
                  std::string::npos);
    }
}

TEST_F(ExploreIndexTestFixture, InspectSymbolNotFound) {
    nlohmann::json params;
    params["name"] = "NonExistentSymbol99";
    auto result = handle_inspect_symbol(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["count"].get<int>(), 0);
}

TEST_F(ExploreIndexTestFixture, InspectSymbolWithTypeFilter) {
    nlohmann::json params;
    params["name"] = "Server";
    params["type"] = "struct";
    auto result = handle_inspect_symbol(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    for (const auto& sym : j["symbols"]) {
        EXPECT_EQ(sym["type"].get<std::string>(), "struct");
    }
}

// =============================================================================
// browse_file tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, BrowseFileRequiresFileOrId) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["error"].get<std::string>().find("file") !=
                std::string::npos);
}

TEST_F(ExploreIndexTestFixture, BrowseFileByName) {
    nlohmann::json params;
    params["file"] = "main.go";
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("file"));
    EXPECT_TRUE(j.contains("symbols"));
    EXPECT_TRUE(j.contains("total"));
    EXPECT_TRUE(j["file"].contains("path"));
    EXPECT_TRUE(j["file"].contains("file_id"));
    EXPECT_TRUE(j["file"].contains("language"));
    EXPECT_EQ(j["file"]["language"].get<std::string>(), "go");
}

TEST_F(ExploreIndexTestFixture, BrowseFileNotFound) {
    // A lookup miss is a definitive negative answer, not a tool error
    // (matches the inspect_symbol not-found shape): found=false + hint.
    nlohmann::json params;
    params["file"] = "nonexistent_file.go";
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j.value("found", true), false);
    EXPECT_NE(j.value("reason", std::string()).find("not found"),
              std::string::npos);
    EXPECT_NE(j.value("hint", std::string()).find("find_files"),
              std::string::npos);
}

TEST_F(ExploreIndexTestFixture, BrowseFileWithKindFilter) {
    nlohmann::json params;
    params["file"] = "main.go";
    params["kind"] = "func";
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    for (const auto& sym : j["symbols"]) {
        auto type = sym["type"].get<std::string>();
        EXPECT_TRUE(type == "function" || type == "method");
    }
}

TEST_F(ExploreIndexTestFixture, BrowseFileKindTypeIncludesStructs) {
    nlohmann::json params;
    params["file"] = "main.go";
    params["kind"] = "type";
    params["exported"] = true;
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    ASSERT_EQ(j["total"].get<int>(), 1);
    ASSERT_EQ(j["symbols"].size(), 1);
    EXPECT_EQ(j["symbols"][0]["name"].get<std::string>(), "Server");
    EXPECT_EQ(j["symbols"][0]["type"].get<std::string>(), "struct");
    ASSERT_TRUE(j.contains("hint"));
    EXPECT_NE(j["hint"].get<std::string>().find("answer from this response"),
              std::string::npos);
}

TEST_F(ExploreIndexTestFixture, BrowseFileWithStats) {
    nlohmann::json params;
    params["file"] = "main.go";
    params["show_stats"] = true;
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("stats"));
    EXPECT_TRUE(j["stats"].contains("symbol_count"));
    EXPECT_TRUE(j["stats"].contains("function_count"));
    EXPECT_TRUE(j["stats"].contains("type_count"));
    EXPECT_TRUE(j["stats"].contains("exported_count"));
}

TEST_F(ExploreIndexTestFixture, BrowseFileSortByName) {
    nlohmann::json params;
    params["file"] = "main.go";
    params["sort"] = "name";
    auto result = handle_browse_file(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    auto& syms = j["symbols"];
    for (size_t i = 1; i < syms.size(); ++i) {
        EXPECT_LE(syms[i - 1]["name"].get<std::string>(),
                  syms[i]["name"].get<std::string>());
    }
}

// =============================================================================
// browse_file basename determinism
// =============================================================================

// Runs browse_file for "util.go" against a two-candidate corpus in a fresh
// child process (fresh absl hash salt -> fresh get_all_file_ids order) and
// returns the raw response text. Returns empty on child failure.
static std::string run_browse_ambiguous_in_child(const std::string& root) {
    int fds[2];
    if (pipe(fds) != 0) return {};
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        Config config;
        config.project.root = root;
        MasterIndex indexer(config);
        if (!indexer.index_directory(root)) _exit(1);
        nlohmann::json params = {{"file", "util.go"}};
        auto result = handle_browse_file(params, indexer);
        const std::string& text = result.text;
        size_t off = 0;
        while (off < text.size()) {
            ssize_t n = write(fds[1], text.data() + off, text.size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        _exit(0);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return out;
}

// Karpathy #4 (determinism): a bare basename matching two files must resolve
// to the lexicographically smallest relative path with an explicit
// `ambiguous` list — identically in every process. The pre-fix code broke at
// the first hit in get_all_file_ids() hash order, so the answer depended on
// the per-process hash salt.
TEST(BrowseFileDeterminismTest, AmbiguousBasenameResolvesSmallestPath) {
    auto dir = lci::test::unique_temp_dir("lci_browse_ambig_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "zeta");
    std::filesystem::create_directories(dir / "alpha");
    const char* src = "package p\n\nfunc helper() int { return 1 }\n";
    {
        std::ofstream f(dir / "zeta" / "util.go");
        f << src;
    }
    {
        std::ofstream f(dir / "alpha" / "util.go");
        f << src;
    }

    std::string reference;
    for (int run = 0; run < 5; ++run) {
        std::string text = run_browse_ambiguous_in_child(dir.string());
        ASSERT_FALSE(text.empty()) << "child process produced no output";
        if (run == 0) {
            reference = text;
        } else {
            EXPECT_EQ(text, reference) << "run " << run << " diverged";
        }
    }

    auto j = nlohmann::json::parse(reference);
    EXPECT_EQ(j["file"]["path"].get<std::string>(), "alpha/util.go");
    ASSERT_TRUE(j.contains("ambiguous")) << j.dump();
    std::vector<std::string> ambig;
    for (const auto& p : j["ambiguous"]) ambig.push_back(p.get<std::string>());
    EXPECT_EQ(ambig, (std::vector<std::string>{"alpha/util.go",
                                               "zeta/util.go"}));

    std::filesystem::remove_all(dir);
}

// A `file` filter carrying a wildcard must actually glob. Before this,
// path_matches_glob only did equality / basename / suffix, so
// `file="svc/*.go"` matched nothing: list_symbols answered an empty list
// and browse_file answered found=false, both silently (karpathy #6). The
// contract pinned here is wildcard_match's, the one find_files already
// ships (src/mcp/handlers_find_files.cpp): '*' matches any run of
// characters INCLUDING '/', '?' matches exactly one.
class ExploreGlobFilterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = lci::test::unique_temp_dir("lci_explore_glob_test_");
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "svc" / "deep");
        std::filesystem::create_directories(dir_ / "other");
        write(dir_ / "svc" / "alpha.go",
              "package svc\n\nfunc AlphaOne() int { return 1 }\n");
        write(dir_ / "svc" / "beta.go",
              "package svc\n\nfunc BetaOne() int { return 2 }\n");
        write(dir_ / "svc" / "deep" / "nested.go",
              "package deep\n\nfunc NestedOne() int { return 3 }\n");
        write(dir_ / "other" / "gamma.go",
              "package other\n\nfunc GammaOne() int { return 4 }\n");

        Config config;
        config.project.root = dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(dir_.string());
    }

    void TearDown() override {
        indexer_.reset();
        std::filesystem::remove_all(dir_);
    }

    static void write(const std::filesystem::path& p,
                      const std::string& content) {
        std::ofstream out(p);
        out << content;
    }

    std::vector<std::string> list_names(const std::string& file_filter) {
        nlohmann::json params;
        params["file"] = file_filter;
        params["kind"] = "all";
        params["max"] = 200;
        auto result = handle_list_symbols(params, *indexer_);
        EXPECT_FALSE(result.is_error) << result.text;
        auto j = nlohmann::json::parse(result.text);
        std::vector<std::string> names;
        for (const auto& s : j["symbols"]) {
            names.push_back(s["name"].get<std::string>());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    std::filesystem::path dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

TEST_F(ExploreGlobFilterTest, ListSymbolsGlobMatchesEveryFileInDirectory) {
    auto names = list_names("svc/*.go");
    // '*' spans '/', so the nested file is in scope too — that is
    // wildcard_match's shipped contract, not an invention here.
    EXPECT_NE(std::find(names.begin(), names.end(), "AlphaOne"), names.end())
        << ::testing::PrintToString(names);
    EXPECT_NE(std::find(names.begin(), names.end(), "BetaOne"), names.end())
        << ::testing::PrintToString(names);
    EXPECT_NE(std::find(names.begin(), names.end(), "NestedOne"), names.end())
        << ::testing::PrintToString(names);
    // Non-matching directory contributes nothing.
    EXPECT_EQ(std::find(names.begin(), names.end(), "GammaOne"), names.end())
        << ::testing::PrintToString(names);
}

TEST_F(ExploreGlobFilterTest, ListSymbolsQuestionMarkMatchesOneCharacter) {
    auto names = list_names("svc/?lpha.go");
    EXPECT_EQ(names, (std::vector<std::string>{"AlphaOne"}));
}

TEST_F(ExploreGlobFilterTest, ListSymbolsPlainNameKeepsBasenameBehaviour) {
    auto names = list_names("alpha.go");
    EXPECT_EQ(names, (std::vector<std::string>{"AlphaOne"}));
}

TEST_F(ExploreGlobFilterTest, BrowseFileResolvesAGlob) {
    nlohmann::json params;
    params["file"] = "svc/al*.go";
    auto result = handle_browse_file(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto j = nlohmann::json::parse(result.text);
    ASSERT_TRUE(j.contains("file")) << j.dump();
    EXPECT_EQ(j["file"]["path"].get<std::string>(), "svc/alpha.go");
}

// Unread params must not sit in the schema as silent no-ops: a param the
// handler never reads (browse_file show_imports, inspect_symbol max_depth)
// is removed from the schema, and the server's unknown-param guard then
// rejects it loudly (karpathy #6) instead of accepting and ignoring it.
TEST_F(ExploreIndexTestFixture, UnreadParamsRejectedByUnknownParamGuard) {
    Config config;
    config.project.root = tmp_dir_.string();
    McpServer server(config, *indexer_, nullptr);
    register_explore_handlers(server, indexer_.get());

    auto call = [&](const std::string& tool, const nlohmann::json& args) {
        nlohmann::json req = {{"jsonrpc", "2.0"},
                              {"id", 1},
                              {"method", "tools/call"},
                              {"params",
                               {{"name", tool}, {"arguments", args}}}};
        return nlohmann::json::parse(server.dispatch_wire(req.dump()));
    };

    auto expect_unknown_param = [&](const nlohmann::json& resp,
                                    const std::string& param) {
        ASSERT_TRUE(resp.contains("result")) << resp.dump();
        ASSERT_TRUE(resp["result"].value("isError", false)) << resp.dump();
        const auto text =
            resp["result"]["content"][0]["text"].get<std::string>();
        EXPECT_NE(text.find("unknown parameter"), std::string::npos) << text;
        EXPECT_NE(text.find(param), std::string::npos) << text;
    };

    expect_unknown_param(
        call("browse_file", {{"file", "main.go"}, {"show_imports", true}}),
        "show_imports");
    expect_unknown_param(
        call("inspect_symbol", {{"name", "Server"}, {"max_depth", 2}}),
        "max_depth");
}

// =============================================================================
// index_stats tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, IndexStatsSummary) {
    nlohmann::json params;
    params["mode"] = "summary";
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["status"].get<std::string>(), "ready");
    EXPECT_TRUE(j["server_ready"].get<bool>());
    EXPECT_TRUE(j.contains("file_count"));
    EXPECT_TRUE(j.contains("symbol_count"));
    EXPECT_TRUE(j.contains("reference_count"));
    EXPECT_TRUE(j.contains("index_time_ms"));
    EXPECT_GE(j["file_count"].get<int>(), 2);
}

TEST_F(ExploreIndexTestFixture, IndexStatsDefault) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("status"));
}

TEST_F(ExploreIndexTestFixture, IndexStatsDetailed) {
    nlohmann::json params;
    params["mode"] = "detailed";
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("component_health"));
    EXPECT_TRUE(j.contains("memory_usage"));
}

TEST_F(ExploreIndexTestFixture, IndexStatsHealth) {
    nlohmann::json params;
    params["mode"] = "health";
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("component_health"));
    auto& health = j["component_health"];
    EXPECT_TRUE(health.contains("symbol_index_ready"));
    EXPECT_TRUE(health.contains("ref_tracker_ready"));
}

TEST_F(ExploreIndexTestFixture, IndexStatsWithComponents) {
    nlohmann::json params;
    params["include_components"] = true;
    auto result = handle_index_stats(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("component_health"));
}

// =============================================================================
// debug_info tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, DebugInfoOverview) {
    nlohmann::json params;
    params["mode"] = "overview";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["mode"].get<std::string>(), "overview");
    EXPECT_TRUE(j.contains("overview"));
    auto& ov = j["overview"];
    EXPECT_TRUE(ov.contains("total_files"));
    EXPECT_TRUE(ov.contains("total_symbols"));
    EXPECT_TRUE(ov.contains("total_references"));
    EXPECT_TRUE(ov.contains("unique_languages"));
    EXPECT_TRUE(ov.contains("language_breakdown"));
    EXPECT_TRUE(ov.contains("type_breakdown"));
}

TEST_F(ExploreIndexTestFixture, DebugInfoDefault) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["mode"].get<std::string>(), "overview");
}

TEST_F(ExploreIndexTestFixture, DebugInfoSymbols) {
    nlohmann::json params;
    params["mode"] = "symbols";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("symbols_by_type"));
}

TEST_F(ExploreIndexTestFixture, DebugInfoReferences) {
    nlohmann::json params;
    params["mode"] = "references";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("top_referenced_symbols"));
}

TEST_F(ExploreIndexTestFixture, DebugInfoFiles) {
    nlohmann::json params;
    params["mode"] = "files";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("files_by_language"));
}

// references mode must emit project-root-relative file_path (the absolute
// stored path was leaking — token bloat + machine-dependent output).
TEST_F(ExploreIndexTestFixture, DebugInfoReferencesPathsAreRootRelative) {
    nlohmann::json params;
    params["mode"] = "references";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    ASSERT_TRUE(j.contains("top_referenced_symbols"));
    ASSERT_FALSE(j["top_referenced_symbols"].empty());
    for (const auto& s : j["top_referenced_symbols"]) {
        auto fp = s["file_path"].get<std::string>();
        EXPECT_NE(fp.front(), '/') << "absolute path leaked: " << fp;
        EXPECT_EQ(fp.find(tmp_dir_.string()), std::string::npos);
    }
}

// files mode accepts a root-relative file_path and returns file_info with a
// root-relative file_path (both directions were broken: rel lookup missed,
// output was absolute).
TEST_F(ExploreIndexTestFixture, DebugInfoFilesByRelativePath) {
    nlohmann::json params;
    params["mode"] = "files";
    params["file_path"] = "main.go";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    ASSERT_TRUE(j.contains("file_info")) << result.text;
    EXPECT_EQ(j["file_info"]["file_path"].get<std::string>(), "main.go");
}

// files mode also accepts the absolute stored path, normalising the output.
TEST_F(ExploreIndexTestFixture, DebugInfoFilesByAbsolutePath) {
    nlohmann::json params;
    params["mode"] = "files";
    params["file_path"] = (tmp_dir_ / "main.go").string();
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    ASSERT_TRUE(j.contains("file_info"));
    EXPECT_EQ(j["file_info"]["file_path"].get<std::string>(), "main.go");
}

// Fail loud (Karpathy #6): a named-but-unindexed file yields a hint, not a
// silently-omitted file_info.
TEST_F(ExploreIndexTestFixture, DebugInfoFilesUnknownPathEmitsHint) {
    nlohmann::json params;
    params["mode"] = "files";
    params["file_path"] = "does/not/exist.go";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_FALSE(j.contains("file_info"));
    ASSERT_TRUE(j.contains("hint"));
    EXPECT_NE(j["hint"].get<std::string>().find("does/not/exist.go"),
              std::string::npos);
}

TEST_F(ExploreIndexTestFixture, DebugInfoFilesUnknownIdEmitsHint) {
    nlohmann::json params;
    params["mode"] = "files";
    params["file_id"] = 999999;
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_FALSE(j.contains("file_info"));
    EXPECT_TRUE(j.contains("hint"));
}

TEST_F(ExploreIndexTestFixture, DebugInfoUnknownMode) {
    nlohmann::json params;
    params["mode"] = "invalid_mode";
    auto result = handle_debug_info(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["error"].get<std::string>().find("unknown mode") !=
                std::string::npos);
}

// =============================================================================
// git_analysis tests
// =============================================================================

// The fixture's tmp_dir_ is a bare scratch directory, not a git repo. That is
// an absent environmental precondition, not a tool error: the handler returns
// a successful, self-describing not-applicable payload (available=false +
// reason) — NOT isError (reads as a code failure to agents), and NOT the old
// zeroed "not_available" stub that mimicked real analysis data.
TEST_F(ExploreIndexTestFixture, GitAnalysisReportsUnavailableOnNonGitDir) {
    nlohmann::json params;
    params["scope"] = "wip";
    auto result = handle_git_analysis(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j.value("available", true), false);
    EXPECT_NE(j.value("reason", std::string()).find("not a git repository"),
              std::string::npos);
    EXPECT_EQ(j.value("status", std::string()), std::string());  // no stub key
    EXPECT_FALSE(j.contains("summary"));  // no fabricated report data
}

TEST_F(ExploreIndexTestFixture, GitAnalysisRejectsBadScope) {
    nlohmann::json params;
    params["scope"] = "bogus";
    auto result = handle_git_analysis(params, *indexer_);
    EXPECT_TRUE(result.is_error);
}

// Real-repo happy path: git-init a throwaway repo, commit a baseline file, make
// an uncommitted long function (a WIP change), index it, and run the handler.
// Asserts the canonical report shape (summary + metadata) and a metrics finding
// for the over-length function — no mocks, real Provider + Analyzer.
TEST_F(ExploreIndexTestFixture, GitAnalysisRealRepoReturnsReport) {
    auto repo = lci::test::unique_temp_dir("lci_git_analysis_real_test_");
    std::filesystem::remove_all(repo);
    std::filesystem::create_directories(repo);

    auto git = [&](const std::string& args) {
        return test::run_git(repo, args);
    };
    ASSERT_TRUE(git("init"));
    git("config user.email test@test.com");
    git("config user.name test");

    // Baseline committed file (a short, tracked function).
    {
        std::ofstream f(repo / "base.go");
        f << "package main\n\nfunc Huge() int { return 1 }\n";
    }
    ASSERT_TRUE(git("add ."));
    ASSERT_TRUE(git("commit -m baseline"));

    // Uncommitted WIP modification of the tracked file: grow Huge() past 100
    // lines so the metrics analyzer raises a long_function finding. WIP scope
    // compares the working tree against HEAD, so the change must be to a
    // tracked file (an untracked new file is not part of the WIP diff).
    {
        std::ofstream f(repo / "base.go");
        f << "package main\n\nfunc Huge() int {\n\tx := 0\n";
        for (int i = 0; i < 130; ++i) f << "\tx += " << i << "\n";
        f << "\treturn x\n}\n";
    }

    Config cfg;
    cfg.project.root = repo.string();
    MasterIndex idx(cfg);
    idx.index_directory(repo.string());

    nlohmann::json params;
    params["scope"] = "wip";
    auto result = handle_git_analysis(params, idx);
    ASSERT_FALSE(result.is_error) << result.text;

    auto j = nlohmann::json::parse(result.text);
    ASSERT_TRUE(j.contains("summary"));
    ASSERT_TRUE(j.contains("metadata"));
    EXPECT_EQ(j["metadata"].value("scope", std::string()), "wip");
    EXPECT_GE(j["summary"].value("files_changed", 0), 1);
    // The long function must surface as a metrics issue.
    ASSERT_TRUE(j.contains("metrics_issues"));
    EXPECT_FALSE(j["metrics_issues"].empty());

    std::filesystem::remove_all(repo);
}

// Staged scope + duplicate detection — the analyzer's headline feature. This
// leg can't get an e2e golden (the spec corpora sit inside the live repo, so
// staged state is developer-dependent); the deterministic home is this
// throwaway out-of-tree repo. A staged new file re-declares a committed
// function body verbatim; the report must carry an exact duplicate finding
// and count the added symbol.
TEST_F(ExploreIndexTestFixture, GitAnalysisStagedDetectsExactDuplicate) {
    auto repo = lci::test::unique_temp_dir("lci_git_analysis_staged_test_");
    std::filesystem::remove_all(repo);
    std::filesystem::create_directories(repo);

    auto git = [&](const std::string& args) {
        return test::run_git(repo, args);
    };
    ASSERT_TRUE(git("init"));
    git("config user.email test@test.com");
    git("config user.name test");

    const std::string add_fn =
        "func Add(a, b int) int {\n"
        "\tresult := a + b\n"
        "\treturn result\n"
        "}\n";
    {
        std::ofstream f(repo / "math.go");
        f << "package main\n\n" << add_fn;
    }
    ASSERT_TRUE(git("add ."));
    ASSERT_TRUE(git("commit -m baseline"));

    // Staged (not committed) file duplicating the committed body.
    {
        std::ofstream f(repo / "copy.go");
        f << "package main\n\n" << add_fn;
    }
    ASSERT_TRUE(git("add copy.go"));

    Config cfg;
    cfg.project.root = repo.string();
    MasterIndex idx(cfg);
    idx.index_directory(repo.string());

    // Empty OBJECT, matching what dispatch passes when the client omits
    // arguments (a default-constructed json is null and value() throws).
    auto params = nlohmann::json::object();  // scope defaults to staged
    auto result = handle_git_analysis(params, idx);
    ASSERT_FALSE(result.is_error) << result.text;

    auto j = nlohmann::json::parse(result.text);
    ASSERT_TRUE(j.contains("metadata") && j["metadata"].is_object())
        << result.text;
    ASSERT_TRUE(j.contains("summary") && j["summary"].is_object())
        << result.text;
    EXPECT_EQ(j["metadata"].value("scope", std::string()), "staged");
    EXPECT_EQ(j["metadata"].value("target_ref", std::string()), "STAGED");
    EXPECT_GE(j["summary"].value("files_changed", 0), 1);
    EXPECT_GE(j["summary"].value("symbols_added", 0), 1);
    ASSERT_TRUE(j.contains("duplicates")) << result.text;
    ASSERT_FALSE(j["duplicates"].empty());
    // Exactly the one real finding: the staged copy.go duplicating the
    // committed math.go Add. A symbol matching ITSELF (identical
    // new_code/existing_code location) is noise the same-location guard
    // must suppress -- it failed to when index paths were absolute while
    // diff paths were repo-relative.
    bool found_cross_file_exact = false;
    for (const auto& dup : j["duplicates"]) {
        const auto& existing = dup.contains("existing_code")
                                   ? dup["existing_code"]
                                   : nlohmann::json(nullptr);
        ASSERT_TRUE(existing.is_object()) << result.text;
        const auto& fresh = dup["new_code"];
        EXPECT_FALSE(existing.value("file_path", std::string()) ==
                         fresh.value("file_path", std::string()) &&
                     existing.value("start_line", -1) ==
                         fresh.value("start_line", -2))
            << "self-match finding: " << dup.dump();
        if (existing.value("file_path", std::string()) == "math.go" &&
            existing.value("symbol_name", std::string()) == "Add" &&
            dup.value("type", std::string()) == "exact" &&
            dup.value("similarity", 0.0) == 1.0) {
            found_cross_file_exact = true;
        }
    }
    EXPECT_TRUE(found_cross_file_exact) << result.text;

    std::filesystem::remove_all(repo);
}

// macOS CI shape reproduced on any platform: the project root reaches the
// repo through a SYMLINK (/var/folders -> /private/var/folders), so git's
// rev-parse --show-toplevel (symlinks resolved) never prefix-matches the
// index's file paths. The duplicate finder's same-location guard then
// compares an absolute existing path against a repo-relative new path and
// misses, emitting a self-match finding for every changed symbol.
TEST_F(ExploreIndexTestFixture, GitAnalysisSymlinkedRootSuppressesSelfMatch) {
    auto real_repo =
        lci::test::unique_temp_dir("lci_git_analysis_symlink_real_");
    std::filesystem::remove_all(real_repo);
    std::filesystem::create_directories(real_repo);
    auto link = lci::test::unique_temp_dir("lci_git_analysis_symlink_link_");
    std::filesystem::remove_all(link);
    std::error_code ec;
    std::filesystem::create_directory_symlink(real_repo, link, ec);
    if (ec) GTEST_SKIP() << "symlinks unavailable: " << ec.message();

    auto git = [&](const std::string& args) {
        return test::run_git(real_repo, args);
    };
    ASSERT_TRUE(git("init"));
    git("config user.email test@test.com");
    git("config user.name test");

    const std::string add_fn =
        "func Add(a, b int) int {\n"
        "\tresult := a + b\n"
        "\treturn result\n"
        "}\n";
    {
        std::ofstream f(real_repo / "math.go");
        f << "package main\n\n" << add_fn;
    }
    ASSERT_TRUE(git("add ."));
    ASSERT_TRUE(git("commit -m baseline"));
    {
        std::ofstream f(real_repo / "copy.go");
        f << "package main\n\n" << add_fn;
    }
    ASSERT_TRUE(git("add copy.go"));

    // Index THROUGH the symlink: file paths carry the link spelling while
    // git reports the resolved root.
    Config cfg;
    cfg.project.root = link.string();
    MasterIndex idx(cfg);
    idx.index_directory(link.string());

    auto params = nlohmann::json::object();  // scope defaults to staged
    auto result = handle_git_analysis(params, idx);
    ASSERT_FALSE(result.is_error) << result.text;

    auto j = nlohmann::json::parse(result.text);
    ASSERT_TRUE(j.contains("duplicates")) << result.text;
    bool found_cross_file_exact = false;
    for (const auto& dup : j["duplicates"]) {
        const auto& existing = dup["existing_code"];
        const auto& fresh = dup["new_code"];
        EXPECT_FALSE(existing.value("file_path", std::string()) ==
                         fresh.value("file_path", std::string()) &&
                     existing.value("start_line", -1) ==
                         fresh.value("start_line", -2))
            << "self-match finding: " << dup.dump();
        if (existing.value("file_path", std::string()) == "math.go" &&
            dup.value("type", std::string()) == "exact") {
            found_cross_file_exact = true;
        }
    }
    EXPECT_TRUE(found_cross_file_exact) << result.text;

    std::filesystem::remove_all(link);
    std::filesystem::remove_all(real_repo);
}

// Commit and range scopes walk ref resolution paths the staged/wip legs never
// touch (rev-parse of base/target, HEAD~1..HEAD diffs).
TEST_F(ExploreIndexTestFixture, GitAnalysisCommitAndRangeScopes) {
    auto repo = lci::test::unique_temp_dir("lci_git_analysis_commit_test_");
    std::filesystem::remove_all(repo);
    std::filesystem::create_directories(repo);

    auto git = [&](const std::string& args) {
        return test::run_git(repo, args);
    };
    ASSERT_TRUE(git("init"));
    git("config user.email test@test.com");
    git("config user.name test");

    {
        std::ofstream f(repo / "a.go");
        f << "package main\n\nfunc One() int { return 1 }\n";
    }
    ASSERT_TRUE(git("add ."));
    ASSERT_TRUE(git("commit -m first"));
    {
        std::ofstream f(repo / "b.go");
        f << "package main\n\nfunc Two() int { return 2 }\n";
    }
    ASSERT_TRUE(git("add ."));
    ASSERT_TRUE(git("commit -m second"));

    Config cfg;
    cfg.project.root = repo.string();
    MasterIndex idx(cfg);
    idx.index_directory(repo.string());

    {
        nlohmann::json params;
        params["scope"] = "commit";  // last commit vs its parent
        auto result = handle_git_analysis(params, idx);
        ASSERT_FALSE(result.is_error) << result.text;
        auto j = nlohmann::json::parse(result.text);
        EXPECT_EQ(j["metadata"].value("scope", std::string()), "commit");
        EXPECT_GE(j["summary"].value("files_changed", 0), 1);
        EXPECT_GE(j["summary"].value("symbols_added", 0), 1);
    }
    {
        nlohmann::json params;
        params["scope"] = "range";
        params["base_ref"] = "HEAD~1";
        params["target_ref"] = "HEAD";
        auto result = handle_git_analysis(params, idx);
        ASSERT_FALSE(result.is_error) << result.text;
        auto j = nlohmann::json::parse(result.text);
        EXPECT_EQ(j["metadata"].value("scope", std::string()), "range");
        EXPECT_GE(j["summary"].value("files_changed", 0), 1);
    }

    std::filesystem::remove_all(repo);
}

// =============================================================================
// Registration tests
// =============================================================================

TEST_F(ExploreIndexTestFixture, RegisterExploreHandlers) {
    Config config;
    config.project.root = tmp_dir_.string();
    McpServer server(config, *indexer_, nullptr);
    size_t before = server.tool_count();
    register_explore_handlers(server, indexer_.get());
    EXPECT_EQ(server.tool_count(), before + 4);
}

TEST_F(ExploreIndexTestFixture, RegisterIndexHandlers) {
    Config config;
    config.project.root = tmp_dir_.string();
    McpServer server(config, *indexer_, nullptr);
    size_t before = server.tool_count();
    register_index_handlers(server, indexer_.get());
    EXPECT_EQ(server.tool_count(), before + 3);
}

// =============================================================================
// Extension -> language centralization (regression guard)
// =============================================================================

// A .mjs file must classify as "javascript" in the index language breakdown.
// Before ext->language centralization reached this handler, language_from_path
// had no .mjs case and returned "unknown", so index-summary disagreed with the
// analysis-summary (which routes through the central table). This pins the
// index handler to the central lci::language_map classification.
TEST(IndexLanguageCentralization, MjsFileClassifiesAsJavaScript) {
    auto dir = lci::test::unique_temp_dir("lci_mjs_lang_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream out(dir / "foo.mjs");
        out << "export function greet(name) {\n"
               "    return `hello ${name}`;\n"
               "}\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());

    nlohmann::json params;
    params["mode"] = "files";
    auto result = handle_debug_info(params, indexer);
    ASSERT_FALSE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    ASSERT_TRUE(j.contains("files_by_language"));
    const auto& langs = j["files_by_language"];
    EXPECT_TRUE(langs.contains("javascript"))
        << "files_by_language=" << langs.dump();
    EXPECT_FALSE(langs.contains("unknown"))
        << "files_by_language=" << langs.dump();

    std::filesystem::remove_all(dir);
}

// =============================================================================
// callers tool
// =============================================================================

TEST_F(ExploreIndexTestFixture, CallersRequiresName) {
    auto result = handle_callers(nlohmann::json::object(), *indexer_);
    EXPECT_TRUE(result.is_error);
}

TEST_F(ExploreIndexTestFixture, CallersGroupsConfirmedByEnclosingFunction) {
    auto result = handle_callers({{"name", "processPath"}}, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto j = nlohmann::json::parse(result.text);

    ASSERT_EQ(j["definitions"].size(), 1u);
    EXPECT_EQ(j["definitions"][0]["name"], "processPath");
    EXPECT_EQ(j["definitions"][0]["file_path"], "main.go");

    ASSERT_EQ(j["total_callers"].get<int>(), 1);
    ASSERT_EQ(j["callers"].size(), 1u);
    const auto& c = j["callers"][0];
    EXPECT_EQ(c["caller"], "HandleRequest");
    EXPECT_EQ(c["file_path"], "main.go");
    EXPECT_EQ(c["call_count"].get<int>(), 1);
    ASSERT_EQ(c["call_lines"].size(), 1u);
    EXPECT_EQ(c["call_lines"][0].get<int>(), 7);
    EXPECT_FALSE(j["truncated"].get<bool>());
}

TEST_F(ExploreIndexTestFixture, CallersUnknownNameFailsLoudWithHint) {
    auto result = handle_callers({{"name", "definitelyMissing"}}, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["definitions"].empty());
    EXPECT_EQ(j["total_callers"].get<int>(), 0);
    EXPECT_TRUE(j.contains("hint"));
}

}  // namespace
}  // namespace mcp
}  // namespace lci
