// Real-project advanced feature benchmarks
//
// Measures latency of code_insight, get_context, and find_files on actual
// codebases. Mirrors the Go profiling tests.

#include <benchmark/benchmark.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "helpers/real_project_helpers.h"

namespace lci {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Benchmark: code_insight on real projects
// ---------------------------------------------------------------------------

static void BM_RealProjectCodeInsightOverview(benchmark::State& state) {
    auto path = testing::find_real_project("go", "chi");
    if (!path) {
        state.SkipWithError("real_projects/go/chi not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, "chi");
    testing::RealProjectContext ctx;
    ctx.project_path = *path;
    ctx.project_name = "chi";
    ctx.config = cfg;
    ctx.indexer = std::make_unique<MasterIndex>(cfg);
    if (!ctx.indexer->index_directory(path->string())) {
        state.SkipWithError("Indexing failed");
        return;
    }
    ctx.ci_engine_ = std::make_unique<CodebaseIntelligenceEngine>();

    nlohmann::json params;
    params["mode"] = "overview";

    for (auto _ : state) {
        auto start = std::chrono::steady_clock::now();
        auto result = ctx.code_insight(params);
        auto elapsed = std::chrono::steady_clock::now() - start;
        benchmark::DoNotOptimize(result);
        state.SetIterationTime(
            std::chrono::duration<double>(elapsed).count());
    }
}
BENCHMARK(BM_RealProjectCodeInsightOverview)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

static void BM_RealProjectCodeInsightStatistics(benchmark::State& state) {
    auto path = testing::find_real_project("go", "chi");
    if (!path) {
        state.SkipWithError("real_projects/go/chi not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, "chi");
    testing::RealProjectContext ctx;
    ctx.project_path = *path;
    ctx.project_name = "chi";
    ctx.config = cfg;
    ctx.indexer = std::make_unique<MasterIndex>(cfg);
    if (!ctx.indexer->index_directory(path->string())) {
        state.SkipWithError("Indexing failed");
        return;
    }
    ctx.ci_engine_ = std::make_unique<CodebaseIntelligenceEngine>();

    nlohmann::json params;
    params["mode"] = "statistics";

    for (auto _ : state) {
        auto start = std::chrono::steady_clock::now();
        auto result = ctx.code_insight(params);
        auto elapsed = std::chrono::steady_clock::now() - start;
        benchmark::DoNotOptimize(result);
        state.SetIterationTime(
            std::chrono::duration<double>(elapsed).count());
    }
}
BENCHMARK(BM_RealProjectCodeInsightStatistics)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

// ---------------------------------------------------------------------------
// Benchmark: get_context on real projects
// ---------------------------------------------------------------------------

static void BM_RealProjectGetContext(benchmark::State& state) {
    auto path = testing::find_real_project("go", "chi");
    if (!path) {
        state.SkipWithError("real_projects/go/chi not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, "chi");
    testing::RealProjectContext ctx;
    ctx.project_path = *path;
    ctx.project_name = "chi";
    ctx.config = cfg;
    ctx.indexer = std::make_unique<MasterIndex>(cfg);
    if (!ctx.indexer->index_directory(path->string())) {
        state.SkipWithError("Indexing failed");
        return;
    }

    nlohmann::json params;
    params["name"] = "ServeHTTP";
    params["include_call_hierarchy"] = true;

    for (auto _ : state) {
        auto start = std::chrono::steady_clock::now();
        auto result = ctx.get_context(params);
        auto elapsed = std::chrono::steady_clock::now() - start;
        benchmark::DoNotOptimize(result);
        state.SetIterationTime(
            std::chrono::duration<double>(elapsed).count());
    }
}
BENCHMARK(BM_RealProjectGetContext)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

// ---------------------------------------------------------------------------
// Benchmark: find_files on real projects
// ---------------------------------------------------------------------------

static void BM_RealProjectFindFiles(benchmark::State& state) {
    auto path = testing::find_real_project("go", "chi");
    if (!path) {
        state.SkipWithError("real_projects/go/chi not found");
        return;
    }

    auto cfg = testing::make_real_project_config(*path, "chi");
    testing::RealProjectContext ctx;
    ctx.project_path = *path;
    ctx.project_name = "chi";
    ctx.config = cfg;
    ctx.indexer = std::make_unique<MasterIndex>(cfg);
    if (!ctx.indexer->index_directory(path->string())) {
        state.SkipWithError("Indexing failed");
        return;
    }

    nlohmann::json params;
    params["pattern"] = "router";

    for (auto _ : state) {
        auto start = std::chrono::steady_clock::now();
        auto result = ctx.find_files(params);
        auto elapsed = std::chrono::steady_clock::now() - start;
        benchmark::DoNotOptimize(result);
        state.SetIterationTime(
            std::chrono::duration<double>(elapsed).count());
    }
}
BENCHMARK(BM_RealProjectFindFiles)
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();

// ---------------------------------------------------------------------------
// Benchmark: search latency on real projects
// ---------------------------------------------------------------------------

static void BM_RealProjectSearchLatency(benchmark::State& state) {
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
    opts.max_results = 10;
    opts.merge_file_results = true;

    for (auto _ : state) {
        auto results = index.search_with_options("ServeHTTP", opts);
        benchmark::DoNotOptimize(results);
    }
}
BENCHMARK(BM_RealProjectSearchLatency)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace lci
