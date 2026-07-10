#include <gtest/gtest.h>

#include <lci/core/file_content_store.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/trigram.h>
#include <lci/indexing/pipeline_integrator.h>
#include <lci/indexing/pipeline_processor.h>
#include <lci/indexing/pipeline_types.h>
#include <lci/indexing/trigram_merger.h>

#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

// -- Helpers ------------------------------------------------------------------

ProcessedFile make_processed_file(FileID file_id, const std::string& path,
                                  TrigramIndex& trigram_idx,
                                  std::string_view content) {
    ProcessedFile pf;
    pf.file_id = file_id;
    pf.path = path;
    pf.stage = "completed";

    // Bucket trigrams from content.
    if (content.size() >= 3) {
        auto bucketed = trigram_idx.create_bucketed_result(file_id);
        auto bytes = reinterpret_cast<const uint8_t*>(content.data());
        for (size_t i = 0; i + 2 < content.size(); ++i) {
            uint32_t trigram = (uint32_t(bytes[i]) << 16) |
                               (uint32_t(bytes[i + 1]) << 8) |
                               uint32_t(bytes[i + 2]);
            uint16_t bucket_id = trigram_idx.get_bucket_for_trigram(trigram);
            bucketed.buckets[bucket_id].trigrams[trigram].push_back(
                static_cast<uint32_t>(i));
        }
        pf.bucketed_trigrams = std::move(bucketed);
    }

    return pf;
}

// -- TrigramMergerPipeline tests ----------------------------------------------

TEST(TrigramMergerPipelineTest, MergesTrigramsLockFree) {
    TrigramIndex trigram_idx;
    TrigramMergerPipeline merger(trigram_idx, 4);
    merger.start();

    // Create bucketed trigram data for two files.
    std::string_view content_a = "func main() {}";
    std::string_view content_b = "var result = 0";

    auto bucketed_a = trigram_idx.create_bucketed_result(FileID{1});
    {
        auto bytes = reinterpret_cast<const uint8_t*>(content_a.data());
        for (size_t i = 0; i + 2 < content_a.size(); ++i) {
            uint32_t tri = (uint32_t(bytes[i]) << 16) |
                           (uint32_t(bytes[i + 1]) << 8) |
                           uint32_t(bytes[i + 2]);
            uint16_t bid = trigram_idx.get_bucket_for_trigram(tri);
            bucketed_a.buckets[bid].trigrams[tri].push_back(
                static_cast<uint32_t>(i));
        }
    }
    auto bucketed_b = trigram_idx.create_bucketed_result(FileID{2});
    {
        auto bytes = reinterpret_cast<const uint8_t*>(content_b.data());
        for (size_t i = 0; i + 2 < content_b.size(); ++i) {
            uint32_t tri = (uint32_t(bytes[i]) << 16) |
                           (uint32_t(bytes[i + 1]) << 8) |
                           uint32_t(bytes[i + 2]);
            uint16_t bid = trigram_idx.get_bucket_for_trigram(tri);
            bucketed_b.buckets[bid].trigrams[tri].push_back(
                static_cast<uint32_t>(i));
        }
    }

    EXPECT_TRUE(merger.submit(std::move(bucketed_a)));
    EXPECT_TRUE(merger.submit(std::move(bucketed_b)));

    merger.shutdown();

    // Verify data was merged into storage.
    auto& storage = merger.storage();
    bool found_data = false;
    for (int i = 0; i < storage.get_bucket_count(); ++i) {
        auto& bucket = storage.get_bucket_by_id(i);
        if (!bucket.trigrams.empty()) {
            found_data = true;
            break;
        }
    }
    EXPECT_TRUE(found_data);
    EXPECT_FALSE(merger.has_failures());
}

TEST(TrigramMergerPipelineTest, ShutdownIsIdempotent) {
    TrigramIndex trigram_idx;
    TrigramMergerPipeline merger(trigram_idx, 2);
    merger.start();
    merger.shutdown();
    merger.shutdown();  // Must not crash.
    EXPECT_EQ(merger.get_failed_file_count(), 0);
}

TEST(TrigramMergerPipelineTest, RejectsAfterShutdown) {
    TrigramIndex trigram_idx;
    TrigramMergerPipeline merger(trigram_idx, 2);
    merger.start();
    merger.shutdown();

    auto bucketed = trigram_idx.create_bucketed_result(FileID{1});
    EXPECT_FALSE(merger.submit(BucketedTrigramResult(bucketed)));
}

TEST(TrigramMergerPipelineTest, GetStatsReportsUsage) {
    TrigramIndex trigram_idx;
    TrigramMergerPipeline merger(trigram_idx, 4);
    merger.start();

    auto stats = merger.get_stats();
    EXPECT_EQ(stats.merger_count, 4);
    EXPECT_GT(stats.buffer_capacity, 0);

    merger.shutdown();
}

TEST(TrigramMergerPipelineTest, MultipleFilesParallel) {
    TrigramIndex trigram_idx;
    TrigramMergerPipeline merger(trigram_idx, 4);
    merger.start();

    // Submit 20 files from multiple threads.
    constexpr int kFilesPerThread = 5;
    constexpr int kThreads = 4;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int f = 0; f < kFilesPerThread; ++f) {
                FileID fid{static_cast<uint32_t>(t * kFilesPerThread + f + 1)};
                std::string content =
                    "package p" + std::to_string(t) + std::to_string(f);
                auto bucketed = trigram_idx.create_bucketed_result(fid);
                auto bytes =
                    reinterpret_cast<const uint8_t*>(content.data());
                for (size_t i = 0; i + 2 < content.size(); ++i) {
                    uint32_t tri = (uint32_t(bytes[i]) << 16) |
                                   (uint32_t(bytes[i + 1]) << 8) |
                                   uint32_t(bytes[i + 2]);
                    uint16_t bid = trigram_idx.get_bucket_for_trigram(tri);
                    bucketed.buckets[bid].trigrams[tri].push_back(
                        static_cast<uint32_t>(i));
                }
                merger.submit(BucketedTrigramResult(bucketed));
            }
        });
    }

    for (auto& thr : threads) thr.join();
    merger.shutdown();
    EXPECT_FALSE(merger.has_failures());
}

// -- FileIntegrator tests -----------------------------------------------------

TEST(FileIntegratorTest, IntegratesTrigramsIntoIndex) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);

    auto pf = make_processed_file(
        FileID{1}, "/src/main.go", trigram_idx, "package main\nfunc main() {}\n");
    integrator.integrate_file(pf);

    EXPECT_EQ(integrator.file_count(), 1);
    EXPECT_EQ(integrator.path_to_id("/src/main.go"), FileID{1});
    EXPECT_EQ(integrator.id_to_path(FileID{1}), "/src/main.go");
}

TEST(FileIntegratorTest, IntegratesSymbols) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);

    auto pf = make_processed_file(
        FileID{1}, "/src/main.go", trigram_idx, "package main");

    Symbol sym;
    sym.name = "main";
    sym.type = SymbolType::Function;
    sym.file_id = FileID{1};
    sym.line = 2;
    sym.end_line = 4;
    pf.symbols.push_back(sym);

    integrator.integrate_file(pf);

    // The reference tracker should now know about the symbol.
    auto snapshot = ref_tracker.pin();
    auto found = snapshot->find_symbols_by_name("main");
    EXPECT_GE(found.size(), 1u);
}

TEST(FileIntegratorTest, HandlesFileUpdate) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);

    // First version.
    auto pf1 = make_processed_file(
        FileID{1}, "/src/main.go", trigram_idx, "package main");
    Symbol sym1;
    sym1.name = "OldFunc";
    sym1.type = SymbolType::Function;
    sym1.file_id = FileID{1};
    sym1.line = 1;
    sym1.end_line = 3;
    pf1.symbols.push_back(sym1);
    integrator.integrate_file(pf1);

    EXPECT_EQ(integrator.file_count(), 1);

    // Updated version with new FileID (simulating re-index).
    auto pf2 = make_processed_file(
        FileID{2}, "/src/main.go", trigram_idx, "package main\nfunc New() {}");
    Symbol sym2;
    sym2.name = "New";
    sym2.type = SymbolType::Function;
    sym2.file_id = FileID{2};
    sym2.line = 2;
    sym2.end_line = 4;
    pf2.symbols.push_back(sym2);
    integrator.integrate_file(pf2);

    // Should still have 1 file (updated, not added).
    EXPECT_EQ(integrator.file_count(), 1);
    EXPECT_EQ(integrator.path_to_id("/src/main.go"), FileID{2});

    // Old symbol should be gone, new one present.
    auto snapshot = ref_tracker.pin();
    auto old_syms = snapshot->find_symbols_by_name("OldFunc");
    EXPECT_EQ(old_syms.size(), 0u);

    auto new_syms = snapshot->find_symbols_by_name("New");
    EXPECT_GE(new_syms.size(), 1u);
}

TEST(FileIntegratorTest, RemoveFileRemovesAllData) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);

    auto pf = make_processed_file(
        FileID{1}, "/src/lib.go", trigram_idx, "package lib\nfunc Helper() {}\n");
    Symbol sym;
    sym.name = "Helper";
    sym.type = SymbolType::Function;
    sym.file_id = FileID{1};
    sym.line = 2;
    sym.end_line = 4;
    pf.symbols.push_back(sym);
    integrator.integrate_file(pf);

    EXPECT_EQ(integrator.file_count(), 1);
    EXPECT_GE(ref_tracker.pin()->find_symbols_by_name("Helper").size(), 1u);

    integrator.remove_file("/src/lib.go");

    EXPECT_EQ(integrator.file_count(), 0);
    EXPECT_EQ(integrator.path_to_id("/src/lib.go"), FileID{0});
    EXPECT_EQ(ref_tracker.pin()->find_symbols_by_name("Helper").size(), 0u);
}

TEST(FileIntegratorTest, IntegrateFromQueue) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);

    BoundedQueue<ProcessedFile> results(10);
    results.push(make_processed_file(
        FileID{1}, "/a.go", trigram_idx, "package a\nfunc A() {}"));
    results.push(make_processed_file(
        FileID{2}, "/b.go", trigram_idx, "package b\nfunc B() {}"));
    results.close();

    integrator.integrate(results);

    EXPECT_EQ(integrator.file_count(), 2);
    EXPECT_EQ(integrator.path_to_id("/a.go"), FileID{1});
    EXPECT_EQ(integrator.path_to_id("/b.go"), FileID{2});
}

TEST(FileIntegratorTest, SkipsErrorFiles) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);

    BoundedQueue<ProcessedFile> results(10);

    // Error file.
    ProcessedFile err;
    err.path = "/bad.go";
    err.has_error = true;
    results.push(std::move(err));

    // Zero-ID file.
    ProcessedFile zero;
    zero.path = "/zero.go";
    zero.file_id = FileID{0};
    results.push(std::move(zero));

    // Valid file.
    results.push(make_processed_file(
        FileID{3}, "/good.go", trigram_idx, "package good"));
    results.close();

    integrator.integrate(results);
    EXPECT_EQ(integrator.file_count(), 1);
}

TEST(FileIntegratorTest, PathToIdAndIdToPath) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);

    // Unknown file returns zero/empty.
    EXPECT_EQ(integrator.path_to_id("/unknown"), FileID{0});
    EXPECT_TRUE(integrator.id_to_path(FileID{99}).empty());

    auto pf = make_processed_file(
        FileID{5}, "/tracked.go", trigram_idx, "package tracked");
    integrator.integrate_file(pf);

    EXPECT_EQ(integrator.path_to_id("/tracked.go"), FileID{5});
    EXPECT_EQ(integrator.id_to_path(FileID{5}), "/tracked.go");
}

TEST(FileIntegratorTest, MergerPipelineIntegration) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);
    integrator.enable_merger_pipeline(4);

    auto pf = make_processed_file(
        FileID{1}, "/merged.go", trigram_idx, "package merged\nfunc Merge() {}\n");
    integrator.integrate_file(pf);

    auto stats = integrator.get_merger_stats();
    EXPECT_EQ(stats.merger_count, 4);

    integrator.disable_merger_pipeline();

    EXPECT_EQ(integrator.file_count(), 1);
}

TEST(FileIntegratorTest, IntegratesWithPostingsIndex) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;
    FileContentStore content_store;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);
    integrator.set_file_content_store(&content_store);

    // Load content into the store so the integrator can find it.
    FileID fid = content_store.load_file("/src/main.go", "package main\nfunc Hello() {}\n");

    auto pf = make_processed_file(
        fid, "/src/main.go", trigram_idx, "package main\nfunc Hello() {}\n");
    integrator.integrate_file(pf);

    // Postings should have been indexed.
    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    postings_idx.find("package", false, files, offsets);
    EXPECT_GE(files.size(), 1u);
}

}  // namespace
}  // namespace lci
