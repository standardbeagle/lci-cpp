// Index-size smoke tests over large long-tail repos across languages.
//
// The 2026-08-04 incident class: lci servers reached 26 GB RSS on a 2 GB
// corpus and crashed the host. These tests index real oversized repos
// (fetched by scripts/fetch-smoke-corpora.sh, skipped when absent — local
// runs only, like the real-project suites) with the DEFAULT config, so
// they exercise the whole defense stack end-to-end: default + manifest
// excludes, minified/hostile skips, and the enforced corpus budget.
//
// The asserted invariant is the one that matters for host survival: RSS
// growth for one index run stays under a hard ceiling REGARDLESS of repo
// size, because the budget caps indexable bytes. The per-corpus ratio is
// logged for trend-watching; tighten kMaxGrowthBytes as the per-byte cost
// improves (target: ≤2x of the budgeted corpus).

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/indexing/pipeline_scanner.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace lci {
namespace {

namespace fs = std::filesystem;

// Hard ceiling for one default-config index run. The budget admits at most
// 500 MB / 50k files, so growth beyond this is a per-byte regression, not
// "the repo is big". Current worst measured: ~3.3 GB on next.js before the
// generated-artifact excludes landed.
constexpr int64_t kMaxGrowthBytes = int64_t{3} * 1024 * 1024 * 1024;

long read_rss_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    long rss_kb = -1;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::sscanf(line, "VmRSS: %ld kB", &rss_kb) == 1) break;
    }
    std::fclose(f);
    return rss_kb;
}

fs::path corpora_root() {
    if (const char* env = std::getenv("LCI_SMOKE_CORPORA_DIR")) {
        return fs::path(env);
    }
    return fs::path(LCI_PROJECT_ROOT) / ".work" / "smoke-corpora";
}

class IndexSizeSmoke : public ::testing::TestWithParam<const char*> {};

TEST_P(IndexSizeSmoke, RssGrowthStaysUnderCeiling) {
    const fs::path corpus = corpora_root() / GetParam();
    if (!fs::exists(corpus / ".git")) {
        GTEST_SKIP() << "smoke corpus not fetched: " << corpus
                     << " (run scripts/fetch-smoke-corpora.sh)";
    }
    const long rss_before_kb = read_rss_kb();
    ASSERT_GT(rss_before_kb, 0) << "smoke test is Linux-only";

    Config cfg = make_default_config();
    cfg.project.root = corpus.string();
    cfg.index.watch_mode = false;

    int64_t indexed_bytes = 0;
    int64_t growth = 0;
    {
        // Scanner run mirrors what the index admits, for the ratio log.
        auto scan = FileScanner(cfg).scan();
        for (const auto& t : scan.tasks) indexed_bytes += t.size;

        MasterIndex index(cfg);
        ASSERT_TRUE(index.index_directory(corpus.string()));
        growth = (read_rss_kb() - rss_before_kb) * 1024;
    }

    const double ratio =
        indexed_bytes > 0
            ? static_cast<double>(growth) / static_cast<double>(indexed_bytes)
            : 0.0;
    std::printf("[smoke] %-10s indexed %6.1f MB, rss growth %7.1f MB "
                "(%.1fx)\n",
                GetParam(),
                static_cast<double>(indexed_bytes) / (1024.0 * 1024.0),
                static_cast<double>(growth) / (1024.0 * 1024.0), ratio);

    EXPECT_LE(growth, kMaxGrowthBytes)
        << "index RSS growth exceeded the smoke ceiling on " << GetParam()
        << " — per-byte memory regression (26 GB incident class)";
}

INSTANTIATE_TEST_SUITE_P(LongTailRepos, IndexSizeSmoke,
                         ::testing::Values("nextjs", "dotnet", "rails",
                                           "symfony", "sklearn", "kubernetes",
                                           "cargo", "spring"),
                         [](const auto& info) { return info.param; });

}  // namespace
}  // namespace lci
