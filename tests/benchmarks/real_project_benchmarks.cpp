// Real-project indexing benchmarks
//
// Mirrors the Go reference's benchmarks in tests/benchmarks/indexing_profiling_test.go.
// These benchmarks measure indexing throughput and search performance against
// actual open-source codebases rather than synthetic data.
//
// Usage:
//   ./lci_benchmarks --benchmark_filter="RealProject"
//
// All benchmarks skip gracefully if real_projects/ is not populated.

#include <benchmark/benchmark.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>

#include <filesystem>
#include <string>
#include <vector>

#include "helpers/real_project_helpers.h"

namespace lci {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Benchmark: indexing throughput on real projects
// ---------------------------------------------------------------------------

static void BM_RealProjectIndexChi(benchmark::State& state) {
    auto path = testing::find_real_project("go", "chi");
    if (!path) {
        state.SkipWithError("real_projects/go/chi not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, "chi");

    for (auto _ : state) {
        MasterIndex index(cfg);
        auto start = std::chrono::steady_clock::now();
        bool ok = index.index_directory(path->string());
        auto elapsed = std::chrono::steady_clock::now() - start;

        if (!ok) {
            state.SkipWithError("Indexing failed");
            return;
        }

        int files = index.file_count();
        state.SetIterationTime(
            std::chrono::duration<double>(elapsed).count());
        state.counters["files"] = benchmark::Counter(
            files, benchmark::Counter::kAvgThreads);
    }
}
BENCHMARK(BM_RealProjectIndexChi)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

static void BM_RealProjectIndexPocketbase(benchmark::State& state) {
    auto path = testing::find_real_project("go", "pocketbase");
    if (!path) {
        state.SkipWithError("real_projects/go/pocketbase not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, "pocketbase");

    for (auto _ : state) {
        MasterIndex index(cfg);
        auto start = std::chrono::steady_clock::now();
        bool ok = index.index_directory(path->string());
        auto elapsed = std::chrono::steady_clock::now() - start;

        if (!ok) {
            state.SkipWithError("Indexing failed");
            return;
        }

        int files = index.file_count();
        state.SetIterationTime(
            std::chrono::duration<double>(elapsed).count());
        state.counters["files"] = benchmark::Counter(
            files, benchmark::Counter::kAvgThreads);
    }
}
BENCHMARK(BM_RealProjectIndexPocketbase)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

static void BM_RealProjectIndexFastapi(benchmark::State& state) {
    auto path = testing::find_real_project("python", "fastapi");
    if (!path) {
        state.SkipWithError("real_projects/python/fastapi not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, "fastapi");

    for (auto _ : state) {
        MasterIndex index(cfg);
        auto start = std::chrono::steady_clock::now();
        bool ok = index.index_directory(path->string());
        auto elapsed = std::chrono::steady_clock::now() - start;

        if (!ok) {
            state.SkipWithError("Indexing failed");
            return;
        }

        int files = index.file_count();
        state.SetIterationTime(
            std::chrono::duration<double>(elapsed).count());
        state.counters["files"] = benchmark::Counter(
            files, benchmark::Counter::kAvgThreads);
    }
}
BENCHMARK(BM_RealProjectIndexFastapi)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

// ---------------------------------------------------------------------------
// Benchmark: search performance on indexed real project
// ---------------------------------------------------------------------------

static void BM_RealProjectSearchChi(benchmark::State& state) {
    auto path = testing::find_real_project("go", "chi");
    if (!path) {
        state.SkipWithError("real_projects/go/chi not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, "chi");
    MasterIndex index(cfg);
    if (!index.index_directory(path->string())) {
        state.SkipWithError("Indexing failed");
        return;
    }

    SearchOptions opts;
    opts.max_results = 20;
    opts.merge_file_results = true;

    for (auto _ : state) {
        auto results = index.search_with_options("ServeHTTP", opts);
        benchmark::DoNotOptimize(results);
    }
}
BENCHMARK(BM_RealProjectSearchChi)
    ->Unit(benchmark::kMicrosecond);

static void BM_RealProjectSearchFastapi(benchmark::State& state) {
    auto path = testing::find_real_project("python", "fastapi");
    if (!path) {
        state.SkipWithError("real_projects/python/fastapi not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, "fastapi");
    MasterIndex index(cfg);
    if (!index.index_directory(path->string())) {
        state.SkipWithError("Indexing failed");
        return;
    }

    SearchOptions opts;
    opts.max_results = 20;
    opts.merge_file_results = true;

    for (auto _ : state) {
        auto results = index.search_with_options("Depends", opts);
        benchmark::DoNotOptimize(results);
    }
}
BENCHMARK(BM_RealProjectSearchFastapi)
    ->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------
// Benchmark: parametric indexing across all available real projects
// ---------------------------------------------------------------------------

static void BM_RealProjectIndexAll(benchmark::State& state) {
    auto projects = testing::list_available_real_projects();
    if (projects.empty()) {
        state.SkipWithError("No real projects found");
        return;
    }

    int idx = static_cast<int>(state.range(0));
    if (idx < 0 || idx >= static_cast<int>(projects.size())) {
        state.SkipWithError("Project index out of range");
        return;
    }

    const auto& [lang, name] = projects[idx];
    auto path = testing::find_real_project(lang, name);
    if (!path) {
        state.SkipWithError("Project not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, name);

    for (auto _ : state) {
        MasterIndex index(cfg);
        auto start = std::chrono::steady_clock::now();
        bool ok = index.index_directory(path->string());
        auto elapsed = std::chrono::steady_clock::now() - start;

        if (!ok) {
            state.SkipWithError("Indexing failed");
            return;
        }

        int files = index.file_count();
        state.SetIterationTime(
            std::chrono::duration<double>(elapsed).count());
        state.counters["files"] = benchmark::Counter(
            files, benchmark::Counter::kAvgThreads);
        state.counters["files/sec"] = benchmark::Counter(
            files / std::max(std::chrono::duration<double>(elapsed).count(),
                             0.001),
            benchmark::Counter::kAvgThreads);
    }
}

// Register one benchmark per available project at binary load time.
// We do this with a simple struct whose constructor runs before main().
struct RegisterRealProjectBenchmarks {
    RegisterRealProjectBenchmarks() {
        auto projects = testing::list_available_real_projects();
        for (size_t i = 0; i < projects.size(); ++i) {
            const auto& [lang, name] = projects[i];
            std::string bench_name =
                "BM_RealProjectIndexAll/" + lang + "/" + name;
            benchmark::RegisterBenchmark(bench_name.c_str(),
                                          BM_RealProjectIndexAll)
                ->Arg(static_cast<int>(i))
                ->Unit(benchmark::kMillisecond)
                ->UseManualTime();
        }
    }
} register_real_project_benchmarks;

}  // namespace
}  // namespace lci
