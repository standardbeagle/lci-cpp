#include <benchmark/benchmark.h>

#include <lci/config.h>
#include <lci/core/trigram.h>
#include <lci/indexing/master_index.h>
#include <lci/parser/parser_pool.h>
#include <lci/search/search_engine.h>
#include <lci/semantic/fuzzy_matcher.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <re2/re2.h>

namespace fs = std::filesystem;

// -- Helpers ------------------------------------------------------------------

namespace {

/// Generates a synthetic source file with typical Go-like content.
std::string generate_source(int line_count) {
    std::string content;
    content.reserve(static_cast<size_t>(line_count) * 40);
    content += "package main\n\nimport \"fmt\"\n\n";
    for (int i = 0; i < line_count; ++i) {
        content += "func handler_" + std::to_string(i) +
                   "(ctx context.Context, req *Request) error {\n";
        content += "    fmt.Println(\"processing request\")\n";
        content += "    return nil\n}\n\n";
    }
    return content;
}

/// Creates a temp directory populated with n synthetic source files.
struct TempBenchDir {
    fs::path root;

    explicit TempBenchDir(int file_count, int lines_per_file = 50) {
        root = fs::temp_directory_path() / ("lci_bench_" +
                   std::to_string(std::hash<std::thread::id>{}(
                       std::this_thread::get_id())));
        fs::create_directories(root / "src");
        for (int i = 0; i < file_count; ++i) {
            auto path = root / "src" / ("file_" + std::to_string(i) + ".go");
            std::ofstream ofs(path, std::ios::binary);
            ofs << generate_source(lines_per_file);
        }
    }

    ~TempBenchDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    TempBenchDir(const TempBenchDir&) = delete;
    TempBenchDir& operator=(const TempBenchDir&) = delete;
};

}  // namespace

// =============================================================================
// Trigram benchmarks
// =============================================================================

static void BM_TrigramExtractAscii(benchmark::State& state) {
    std::string content = generate_source(100);
    for (auto _ : state) {
        auto trigrams = lci::extract_simple_trigrams(content);
        benchmark::DoNotOptimize(trigrams);
    }
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(content.size()));
}
BENCHMARK(BM_TrigramExtractAscii);

static void BM_TrigramIndexFile(benchmark::State& state) {
    lci::TrigramIndex index;
    std::string content = generate_source(100);
    lci::FileID fid = 1;
    for (auto _ : state) {
        index.clear();
        index.index_file(fid, content);
        benchmark::DoNotOptimize(index.file_count());
    }
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(content.size()));
}
BENCHMARK(BM_TrigramIndexFile);

static void BM_TrigramFindCandidates(benchmark::State& state) {
    lci::TrigramIndex index;
    std::string content = generate_source(200);
    for (lci::FileID fid = 1; fid <= 500; ++fid) {
        index.index_file(fid, content);
    }
    for (auto _ : state) {
        auto candidates = index.find_candidates("handler_42");
        benchmark::DoNotOptimize(candidates);
    }
}
BENCHMARK(BM_TrigramFindCandidates);

// =============================================================================
// Search benchmarks
// =============================================================================

static void BM_SearchSmallIndex(benchmark::State& state) {
    lci::Config cfg = lci::make_default_config();
    TempBenchDir dir(50, 20);
    cfg.project.root = dir.root.string();
    lci::MasterIndex index(cfg);
    index.index_directory(dir.root.string());

    lci::SearchOptions opts;
    opts.max_results = 10;
    for (auto _ : state) {
        auto results = index.search_with_options("handler_5", opts);
        benchmark::DoNotOptimize(results);
    }
}
BENCHMARK(BM_SearchSmallIndex);

static void BM_SearchMediumIndex(benchmark::State& state) {
    lci::Config cfg = lci::make_default_config();
    TempBenchDir dir(200, 30);
    cfg.project.root = dir.root.string();
    lci::MasterIndex index(cfg);
    index.index_directory(dir.root.string());

    lci::SearchOptions opts;
    opts.max_results = 20;
    for (auto _ : state) {
        auto results = index.search_with_options("processing request", opts);
        benchmark::DoNotOptimize(results);
    }
}
BENCHMARK(BM_SearchMediumIndex);

// =============================================================================
// Regex filter benchmarks — std::regex vs RE2 on the live search hot path
// =============================================================================

// Head-to-head: std::regex vs RE2 on the search.cpp regex_filter_results
// scenario — match a moderately complex pattern against 100 short code lines.
// Karpathy: this is the hot loop that runs once per result row, so a single-
// digit-microsecond improvement compounds across thousands of search hits.
static const std::vector<std::string>& bench_lines() {
    static const std::vector<std::string> lines = []() {
        std::vector<std::string> v;
        v.reserve(100);
        for (int i = 0; i < 100; ++i) {
            v.push_back("    handler_" + std::to_string(i) +
                        "_process(req, &resp);");
        }
        // Force a non-match line every 5th element so both engines walk
        // beyond a trivial early-match.
        for (int i = 0; i < 100; i += 5) {
            v[static_cast<size_t>(i)] =
                "    // commented out — no handler here";
        }
        return v;
    }();
    return lines;
}

#include <regex>  // benchmark-only — measures the OLD path we replaced
static void BM_StdRegexFilterRows(benchmark::State& state) {
    const auto& lines = bench_lines();
    const std::regex re(R"(handler_\d+_process)",
                        std::regex::ECMAScript | std::regex::multiline);
    for (auto _ : state) {
        int hits = 0;
        std::smatch m;
        for (const auto& line : lines) {
            if (std::regex_search(line, m, re)) ++hits;
        }
        benchmark::DoNotOptimize(hits);
    }
}
BENCHMARK(BM_StdRegexFilterRows);

static void BM_Re2FilterRows(benchmark::State& state) {
    const auto& lines = bench_lines();
    RE2::Options opts(RE2::Quiet);
    opts.set_log_errors(false);
    // Match the search.cpp wrapper's (?m) prefix so we benchmark the same
    // pattern shape the engine actually compiles.
    const RE2 re(R"((?m)handler_\d+_process)", opts);
    for (auto _ : state) {
        int hits = 0;
        for (const auto& line : lines) {
            if (RE2::PartialMatch(line, re)) ++hits;
        }
        benchmark::DoNotOptimize(hits);
    }
}
BENCHMARK(BM_Re2FilterRows);

// =============================================================================
// Parser benchmarks
// =============================================================================

static void BM_ParserPoolAcquireRelease(benchmark::State& state) {
    auto& pool = lci::parser::thread_pool();
    for (auto _ : state) {
        auto* p = pool.acquire(lci::parser::Language::Go);
        benchmark::DoNotOptimize(p);
        if (p) pool.release(lci::parser::Language::Go, p);
    }
}
BENCHMARK(BM_ParserPoolAcquireRelease);

// =============================================================================
// Semantic benchmarks
// =============================================================================

static void BM_FuzzyMatchJaroWinkler(benchmark::State& state) {
    lci::FuzzyMatcher matcher(true, 0.7, "jaro_winkler");
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            matcher.similarity("handleRequest", "handleResponse"));
    }
}
BENCHMARK(BM_FuzzyMatchJaroWinkler);

static void BM_FuzzyMatchLevenshtein(benchmark::State& state) {
    lci::FuzzyMatcher matcher(true, 0.7, "levenshtein");
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            matcher.similarity("processEvent", "processEvents"));
    }
}
BENCHMARK(BM_FuzzyMatchLevenshtein);

static void BM_FuzzyFindMatches(benchmark::State& state) {
    lci::FuzzyMatcher matcher(true, 0.5, "jaro_winkler");
    std::vector<std::string> candidates;
    candidates.reserve(200);
    for (int i = 0; i < 200; ++i) {
        candidates.push_back("symbol_" + std::to_string(i));
    }
    for (auto _ : state) {
        auto matches = matcher.find_matches("symbol_42", candidates);
        benchmark::DoNotOptimize(matches);
    }
}
BENCHMARK(BM_FuzzyFindMatches);

// =============================================================================
// Indexing throughput benchmark (parametric)
// =============================================================================

static void BM_IndexingThroughput(benchmark::State& state) {
    int file_count = static_cast<int>(state.range(0));
    lci::Config cfg = lci::make_default_config();
    TempBenchDir dir(file_count, 30);
    cfg.project.root = dir.root.string();

    for (auto _ : state) {
        lci::MasterIndex index(cfg);
        index.index_directory(dir.root.string());
        benchmark::DoNotOptimize(index.file_count());
    }
    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * file_count);
}
BENCHMARK(BM_IndexingThroughput)
    ->Arg(100)
    ->Arg(500)
    ->Arg(1000)
    ->Unit(benchmark::kMillisecond);
