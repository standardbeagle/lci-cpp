#include <gtest/gtest.h>

#include <lci/core/file_content_store.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/trigram.h>
#include <lci/indexing/pipeline_integrator.h>
#include <lci/indexing/pipeline_processor.h>
#include <lci/indexing/pipeline_types.h>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

// -- Helpers ------------------------------------------------------------------

ProcessedFile make_processed_file(FileID file_id, const std::string& path,
                                  TrigramIndex& trigram_idx,
                                  std::string_view content) {
    (void)trigram_idx;
    (void)content;
    ProcessedFile pf;
    pf.file_id = file_id;
    pf.path = path;
    pf.stage = "completed";
    return pf;
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

// Pins the symbol-less-file fix: merge_symbols used to bail out on
// `file.symbols.empty()`, silently dropping the file's references, scopes
// and imports. A file with references but no symbols must still contribute
// those references to the tracker.
TEST(FileIntegratorTest, SymbolessFileStillContributesReferences) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);

    auto pf = make_processed_file(FileID{1}, "/src/caller.go", trigram_idx,
                                  "package main");
    Reference ref;
    ref.type = ReferenceType::Call;
    ref.referenced_name = "helper";
    ref.line = 3;
    ref.column = 5;
    pf.references.push_back(ref);

    integrator.integrate_file(pf);

    auto snap = ref_tracker.pin();
    auto it = snap->refs_by_file.find(FileID{1});
    ASSERT_NE(it, snap->refs_by_file.end())
        << "symbol-less file's references were dropped at integration";
    ASSERT_EQ(it->second.size(), 1u);
    EXPECT_FALSE(it->second[0].dead);
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

// Pins the symbol-id determinism fix: results arriving from the worker pool
// in scheduling order must be integrated in file_id order, so SymbolID
// assignment (next_symbol_id_ in ReferenceTracker) never depends on thread
// timing (karpathy #4).
TEST(FileIntegratorTest, IntegratesInFileIdOrderRegardlessOfArrival) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;
    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);

    auto with_symbol = [&](FileID fid, const std::string& path,
                           const std::string& name) {
        auto pf = make_processed_file(fid, path, trigram_idx, "package x");
        Symbol sym;
        sym.name = name;
        sym.type = SymbolType::Function;
        sym.file_id = fid;
        sym.line = 1;
        sym.end_line = 2;
        pf.symbols.push_back(sym);
        return pf;
    };

    BoundedQueue<ProcessedFile> results(10);
    // Deliberately out of order: file 2 arrives before file 1.
    results.push(with_symbol(FileID{2}, "/b.go", "BFunc"));
    results.push(with_symbol(FileID{1}, "/a.go", "AFunc"));
    results.close();

    integrator.integrate(results);

    auto snap = ref_tracker.pin();
    auto a = snap->find_symbols_by_name("AFunc");
    auto b = snap->find_symbols_by_name("BFunc");
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    // File 1's symbol must draw the lower SymbolID even though it arrived
    // second.
    EXPECT_LT(a[0]->id, b[0]->id);
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

TEST(FileIntegratorTest, IntegratesWithPostingsIndex) {
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;
    FileContentStore content_store;

    FileIntegrator integrator(&trigram_idx, &ref_tracker, &postings_idx);
    integrator.set_file_content_store(&content_store);

    // Load content into the store so the integrator can find it.
    FileID fid = content_store.add_file("/src/main.go", "package main\nfunc Hello() {}\n");

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
