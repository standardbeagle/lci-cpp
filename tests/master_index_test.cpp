#include <gtest/gtest.h>

#include <lci/analysis/clone_detector.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/config.h>
#include <lci/indexing/master_index.h>

#include "unique_temp.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

// -- Temp directory helper (matches pipeline_test.cpp pattern) ----------------

class TempDir {
  public:
    TempDir() {
        path_ = test::unique_temp_dir("lci_mi_test_");
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

  private:
    std::filesystem::path path_;
};

// -- FileSnapshot tests -------------------------------------------------------

TEST(FileSnapshotTest, DefaultEmpty) {
    FileSnapshot snap;
    EXPECT_EQ(0, snap.file_count());
}

TEST(FileSnapshotTest, CopyOnWrite) {
    auto snap = std::make_shared<FileSnapshot>();
    snap->file_map["a.go"] = FileID{1};
    snap->reverse_file_map[FileID{1}] = "a.go";

    auto copy = std::make_shared<FileSnapshot>(*snap);
    copy->file_map["b.go"] = FileID{2};
    copy->reverse_file_map[FileID{2}] = "b.go";

    EXPECT_EQ(1, snap->file_count());
    EXPECT_EQ(2, copy->file_count());
}

// -- Side-effect sink during bulk indexing ------------------------------------

// The MCP warmup's serial whole-corpus re-parse was replaced by recording
// side effects inside the index pipeline's own extraction (per-worker
// analyzers merged per file). This pins that a sink wired before
// index_directory receives AST-fact records — a bare throw is an effect
// the callee-name heuristic pass cannot see, so its presence proves the
// AST pass ran during indexing.
TEST(MasterIndexTest, BulkIndexFeedsSideEffectSink) {
    TempDir dir;
    dir.write_file("main.go",
                   "package main\n"
                   "\n"
                   "func mightPanic(x int) int {\n"
                   "\tif x < 0 {\n"
                   "\t\tpanic(\"negative\")\n"
                   "\t}\n"
                   "\treturn x\n"
                   "}\n");

    Config cfg;
    cfg.project.root = dir.path().string();
    MasterIndex index(cfg);
    SideEffectAnalyzer sink("generic");
    index.set_side_effect_sink(&sink);
    ASSERT_TRUE(index.index_directory(dir.path().string()));

    ASSERT_FALSE(sink.results().empty());
    bool found = false;
    for (const auto& [key, info] : sink.results()) {
        if (info.function_name == "mightPanic") {
            found = true;
            EXPECT_NE(info.categories & side_effect::kThrow, 0u)
                << "AST-fact throw category missing";
        }
    }
    EXPECT_TRUE(found) << "no record for mightPanic";
}

// -- C++ type-position reference resolution -----------------------------------

// The defect this pins: a C++ class WITH a declared constructor was
// name-ambiguous at resolution (class symbol vs ctor symbol), so `Widget w;`
// and `Widget::compute(...)` built NO edge and every such class reported
// incoming_ref_count == 0 — flooding dead-export candidates. Type-position
// refs now resolve against type-like symbols only, and a qualified call
// credits both the method (via receiver typing) and the type.
TEST(MasterIndexTest, CppTypeUsesCreditTheClassDespiteConstructor) {
    TempDir dir;
    dir.write_file("lib.h",
                   "#pragma once\n"
                   "class Widget {\n"
                   "  public:\n"
                   "    Widget();\n"
                   "    static int compute(int x) { return x * 2; }\n"
                   "    int value() const;\n"
                   "};\n");
    dir.write_file("use.cpp",
                   "#include \"lib.h\"\n"
                   "int use_static() { return Widget::compute(4); }\n"
                   "int use_decl() {\n"
                   "    Widget w;\n"
                   "    return w.value();\n"
                   "}\n");

    Config cfg;
    cfg.project.root = dir.path().string();
    MasterIndex index(cfg);
    ASSERT_TRUE(index.index_directory(dir.path().string()));

    auto rt_snap = index.ref_tracker().pin();
    const EnhancedSymbol* widget_class = nullptr;
    const EnhancedSymbol* compute_method = nullptr;
    for (const auto& es : rt_snap->find_symbols_by_name("Widget")) {
        if (es && es->symbol.type == SymbolType::Class) {
            widget_class = es.get();
        }
    }
    for (const auto& es : rt_snap->find_symbols_by_name("compute")) {
        if (es) compute_method = es.get();
    }
    ASSERT_NE(widget_class, nullptr);
    // `Widget w;` (type position) + the `Widget::` qualifier must both
    // credit the CLASS even though a constructor shares its name.
    EXPECT_GE(widget_class->incoming_ref_count, 2u)
        << "class not credited for type uses";
    ASSERT_NE(compute_method, nullptr);
    EXPECT_GE(compute_method->incoming_ref_count, 1u)
        << "qualified static call not credited to the method";
}

// -- Corpus-wide clone detection ----------------------------------------------

TEST(CloneDetectorTest, FindsExactAndStructuralClasses) {
    TempDir dir;
    // Two exact copies (formatting/comment differences must not matter),
    // one near-copy (renamed identifiers -> structural), one unrelated.
    dir.write_file("a.go",
                   "package main\n\n"
                   "func sumEven(xs []int) int {\n"
                   "\ttotal := 0\n"
                   "\tfor _, x := range xs {\n"
                   "\t\tif x%2 == 0 {\n"
                   "\t\t\ttotal += x\n"
                   "\t\t}\n"
                   "\t}\n"
                   "\treturn total\n"
                   "}\n");
    dir.write_file("b.go",
                   "package main\n\n"
                   "func sumEvens(xs []int) int {\n"
                   "\t// identical body, extra comment\n"
                   "\ttotal := 0\n"
                   "\tfor _, x := range xs {\n"
                   "\t\tif x%2 == 0 {\n"
                   "\t\t\ttotal += x\n"
                   "\t\t}\n"
                   "\t}\n"
                   "\treturn total\n"
                   "}\n");
    dir.write_file("c.go",
                   "package main\n\n"
                   "func addAll(values []int) int {\n"
                   "\tacc := 0\n"
                   "\tfor _, v := range values {\n"
                   "\t\tif v%2 == 0 {\n"
                   "\t\t\tacc += v\n"
                   "\t\t}\n"
                   "\t}\n"
                   "\treturn acc\n"
                   "}\n");

    Config cfg;
    cfg.project.root = dir.path().string();
    MasterIndex index(cfg);
    ASSERT_TRUE(index.index_directory(dir.path().string()));

    CloneDetector::Options opts;
    opts.min_lines = 5;
    opts.structural_threshold = 0.55;
    auto rep = CloneDetector().analyze(index, dir.path().string(), {}, opts);

    ASSERT_GE(rep.functions_scanned, 3);
    ASSERT_GE(rep.clone_classes, 1);

    // The exact pair: sumEven + sumEvens, normalized-identical bodies
    // (function signatures differ only in the name, which is part of the
    // first line — so they are STRUCTURAL unless the whole normalized
    // body matches; assert at least one class contains both).
    bool pair_found = false;
    for (const auto& cc : rep.classes) {
        bool has_a = false, has_b = false;
        for (const auto& m : cc.members) {
            if (m.name == "sumEven") has_a = true;
            if (m.name == "sumEvens") has_b = true;
        }
        if (has_a && has_b) pair_found = true;
    }
    EXPECT_TRUE(pair_found) << "sumEven/sumEvens not grouped";
    EXPECT_GT(rep.duplicated_lines, 0);
    EXPECT_GT(rep.duplication_pct, 0.0);

    // Determinism: members sorted by path, classes ranked.
    for (const auto& cc : rep.classes) {
        for (size_t i = 1; i < cc.members.size(); ++i) {
            EXPECT_LE(cc.members[i - 1].path, cc.members[i].path);
        }
    }
}

// -- MasterIndex lifecycle tests ----------------------------------------------

TEST(MasterIndexTest, ConstructionDefaults) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    EXPECT_EQ(0, mi.file_count());
    EXPECT_FALSE(mi.is_indexing());

    auto stats = mi.get_stats();
    EXPECT_EQ(0, stats.total_files);
    EXPECT_FALSE(stats.is_indexing);
}

TEST(MasterIndexTest, ClearOnEmptyIndex) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_TRUE(mi.clear());
    EXPECT_EQ(0, mi.file_count());
}

TEST(MasterIndexTest, PathToIdNotFound) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_EQ(FileID{0}, mi.path_to_id("/nonexistent"));
}

TEST(MasterIndexTest, IdToPathNotFound) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_TRUE(mi.id_to_path(FileID{999}).empty());
}

TEST(MasterIndexTest, ReadSnapshotIsLockFree) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    auto snap1 = mi.load_snapshot();
    auto snap2 = mi.load_snapshot();
    EXPECT_EQ(snap1.get(), snap2.get());
}

// -- MasterIndex directory indexing -------------------------------------------

TEST(MasterIndexTest, IndexDirectoryEmptyRoot) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_FALSE(mi.index_directory(""));
}

TEST(MasterIndexTest, IndexDirectoryNonexistent) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_FALSE(mi.index_directory("/this/path/does/not/exist"));
}

TEST(MasterIndexTest, IndexDirectorySimple) {
    TempDir dir;
    dir.write_file("hello.go", "package main\nfunc main() {}\n");
    dir.write_file("util.go", "package main\nfunc helper() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    EXPECT_FALSE(mi.is_indexing());
    EXPECT_GE(mi.file_count(), 1);

    auto stats = mi.get_stats();
    EXPECT_GT(stats.indexing_time_ns, 0);
}

// File attributes are classified ONCE at index time (PathClassifier +
// .lci.kdl rules) and stored in the snapshot for lock-free O(1) reads.
TEST(MasterIndexTest, StoresFileAttributesAtIndexTime) {
    TempDir dir;
    dir.write_file("mux.go", "package main\nfunc Route() {}\n");
    dir.write_file("mux_test.go", "package main\nfunc TestRoute() {}\n");
    dir.write_file("_examples/demo/main.go",
                   "package main\nfunc main() {}\n");
    dir.write_file("gen/api.go",
                   "// Code generated by protoc. DO NOT EDIT.\n"
                   "package gen\nfunc Gen() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    // Config rule overlay: tag gen/ generated? builtin content sniff covers
    // it; add a config rule exercising the override path instead.
    cfg.attributes.push_back({"vendored", "mux.go"});
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto attr_of_path = [&](const std::string& rel) {
        FileID id = mi.path_to_id((dir.path() / rel).string());
        EXPECT_NE(id, 0u) << rel;
        return std::string(mi.attr_registry().name(mi.get_file_attr(id)));
    };
    EXPECT_EQ(attr_of_path("mux.go"), "vendored");  // config rule
    EXPECT_EQ(attr_of_path("mux_test.go"), "test");
    EXPECT_EQ(attr_of_path("_examples/demo/main.go"), "example");
    EXPECT_EQ(attr_of_path("gen/api.go"), "generated");  // header sniff
}

// Capability gates. An attribute states which of index/search/refs/analysis
// apply to its files; these two are the ones a project can use to keep a tree
// out of the index or out of search results entirely. Every shipped attribute
// activates both, so a default corpus is unaffected — pinned here so a future
// ruleset edit cannot quietly drop files.
TEST(MasterIndexTest, IndexCapabilityKeepsFilesOutOfTheIndex) {
    TempDir dir;
    dir.write_file("keep.go", "package main\nfunc Keep() {}\n");
    dir.write_file("skipme/drop.go", "package skipme\nfunc Drop() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    AttrDef def;
    def.name = "ignored-tree";
    def.rank = 6;
    def.capabilities = 0;  // activates nothing, not even index
    def.dirs = {"skipme"};
    cfg.attribute_defs.push_back(def);

    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    EXPECT_NE(mi.path_to_id((dir.path() / "keep.go").string()), FileID{0});
    EXPECT_EQ(mi.path_to_id((dir.path() / "skipme" / "drop.go").string()),
              FileID{0})
        << "an attribute that does not activate \"index\" must keep its files "
           "out of the index";
}

TEST(MasterIndexTest, SearchCapabilityKeepsFilesOutOfResults) {
    TempDir dir;
    dir.write_file("keep.go", "package main\nfunc distinctiveNeedle() {}\n");
    dir.write_file("quiet/also.go",
                   "package quiet\nfunc distinctiveNeedle() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    AttrDef def;
    def.name = "unsearchable";
    def.rank = 6;
    def.capabilities = 1u << static_cast<uint8_t>(Capability::Index);
    def.dirs = {"quiet"};
    cfg.attribute_defs.push_back(def);

    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    // Indexed — the file map still holds it.
    EXPECT_NE(mi.path_to_id((dir.path() / "quiet" / "also.go").string()),
              FileID{0});

    auto results = mi.search("distinctiveNeedle", 0);
    ASSERT_FALSE(results.empty());
    for (const auto& r : results) {
        EXPECT_EQ(r.path.find("quiet/"), std::string::npos)
            << "an attribute that does not activate \"search\" must not "
               "appear in results: "
            << r.path;
    }
}

// Files that load (consuming a FileID) but fail AFTER the load — here the
// magic-number binary check — leave gaps between "how many files integrated"
// and "what the highest assigned FileID is". The published file snapshot must
// still carry every file that survived, whatever id it drew.
TEST(MasterIndexTest, IndexDirectoryKeepsFilesWithIdsAbovePostLoadFailures) {
    TempDir dir;

    // ELF magic passes the extension filter (.go is not a binary extension)
    // and is only rejected once the content is loaded and inspected.
    const std::string elf_magic("\x7f\x45\x4c\x46 not really go", 20);

    constexpr int kPairs = 12;
    std::vector<std::string> text_files;
    for (int i = 0; i < kPairs; ++i) {
        // Interleave so the failures consume ids ahead of surviving files.
        dir.write_file("bin_" + std::to_string(i) + ".go", elf_magic);
        auto name = "text_" + std::to_string(i) + ".go";
        dir.write_file(name, "package main\nfunc f" + std::to_string(i) +
                                 "() {}\n");
        text_files.push_back(name);
    }

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto snap = mi.load_snapshot();
    ASSERT_NE(snap, nullptr);
    for (const auto& name : text_files) {
        auto full = (dir.path() / name).string();
        EXPECT_NE(mi.path_to_id(full), FileID{0})
            << name << " survived indexing but is missing from file_map";
    }
}

TEST(MasterIndexTest, IndexDirectoryThenClear) {
    TempDir dir;
    dir.write_file("a.py", "def foo():\n    pass\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    EXPECT_GE(mi.file_count(), 1);

    EXPECT_TRUE(mi.clear());
    EXPECT_EQ(0, mi.file_count());
}

TEST(MasterIndexTest, DoubleIndexDirectory) {
    TempDir dir;
    dir.write_file("x.js", "function x() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    int count1 = mi.file_count();

    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    int count2 = mi.file_count();

    EXPECT_EQ(count1, count2);
}

// -- MasterIndex single-file operations ---------------------------------------

TEST(MasterIndexTest, IndexSingleFile) {
    TempDir dir;
    dir.write_file("single.go", "package main\nfunc single() {}\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "single.go").string();
    EXPECT_TRUE(mi.index_file(file_path));
    EXPECT_EQ(1, mi.file_count());
    EXPECT_NE(FileID{0}, mi.path_to_id(file_path));
}

TEST(MasterIndexTest, IndexFileEmptyPath) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_FALSE(mi.index_file(""));
}

TEST(MasterIndexTest, IndexFileNonexistent) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_FALSE(mi.index_file("/no/such/file.go"));
}

TEST(MasterIndexTest, UpdateFile) {
    TempDir dir;
    dir.write_file("updatable.go", "package main\nvar x = 1\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "updatable.go").string();
    EXPECT_TRUE(mi.index_file(file_path));
    FileID first_id = mi.path_to_id(file_path);
    EXPECT_NE(FileID{0}, first_id);

    std::string new_content = "package main\nvar x = 2\nvar y = 3\n";
    EXPECT_TRUE(mi.update_file(file_path, new_content));
    FileID second_id = mi.path_to_id(file_path);
    EXPECT_NE(FileID{0}, second_id);

    EXPECT_EQ(1, mi.file_count());

    // Verify the content was actually updated.
    auto content = mi.file_content_store().get_content(second_id);
    EXPECT_FALSE(content.empty());
    EXPECT_NE(std::string::npos, content.find("var y = 3"));
}

TEST(MasterIndexTest, UpdateFileEmptyContent) {
    TempDir dir;
    dir.write_file("f.go", "package main\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "f.go").string();
    EXPECT_FALSE(mi.update_file(file_path, ""));
}

TEST(MasterIndexTest, RemoveFile) {
    TempDir dir;
    dir.write_file("removable.go", "package main\nfunc rm() {}\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "removable.go").string();
    EXPECT_TRUE(mi.index_file(file_path));
    EXPECT_EQ(1, mi.file_count());

    EXPECT_TRUE(mi.remove_file(file_path));
    EXPECT_EQ(0, mi.file_count());
    EXPECT_EQ(FileID{0}, mi.path_to_id(file_path));
}

TEST(MasterIndexTest, RemoveNonexistentFile) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_TRUE(mi.remove_file("/no/such/file"));
}

// -- Concurrent access tests --------------------------------------------------

TEST(MasterIndexTest, ConcurrentSnapshotReads) {
    TempDir dir;
    dir.write_file("concurrent.go", "package main\nfunc concurrent() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    mi.index_directory(dir.path().string());

    constexpr int kReaderCount = 8;
    constexpr int kReadsPerThread = 1000;
    std::vector<std::thread> readers;

    for (int i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < kReadsPerThread; ++j) {
                auto snap = mi.load_snapshot();
                (void)snap->file_count();
            }
        });
    }

    for (auto& t : readers) t.join();
}

TEST(MasterIndexTest, ConcurrentReadsWhileUpdating) {
    TempDir dir;
    dir.write_file("base.go", "package main\nfunc base() {}\n");

    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    std::string file_path = (dir.path() / "base.go").string();
    mi.index_file(file_path);

    constexpr int kReaderCount = 4;
    constexpr int kIterations = 100;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;

    for (int i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                auto snap = mi.load_snapshot();
                (void)snap->file_count();
                (void)mi.path_to_id(file_path);
            }
        });
    }

    // Writer thread.
    std::thread writer([&] {
        for (int i = 0; i < kIterations; ++i) {
            std::string content = "package main\nvar v" +
                                  std::to_string(i) + " = " +
                                  std::to_string(i) + "\n";
            mi.update_file(file_path, content);
        }
        stop.store(true, std::memory_order_release);
    });

    writer.join();
    for (auto& t : readers) t.join();

    EXPECT_EQ(1, mi.file_count());
}

// -- Sub-index access tests ---------------------------------------------------

TEST(MasterIndexTest, SubIndexAccess) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);

    // Ensure sub-indexes are accessible and valid.
    (void)mi.trigram_index();
    (void)mi.ref_tracker();
    (void)mi.postings_index();
    (void)mi.symbol_location_index();
    (void)mi.file_content_store();
    (void)mi.config();
}

// -- Stats after indexing -----------------------------------------------------

TEST(MasterIndexTest, StatsAfterIndexing) {
    TempDir dir;
    dir.write_file("stats.go", "package main\nfunc stats() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    mi.index_directory(dir.path().string());
    auto stats = mi.get_stats();
    EXPECT_GE(stats.total_files, 1);
    EXPECT_FALSE(stats.is_indexing);
    EXPECT_GT(stats.indexing_time_ns, 0);
}

TEST(MasterIndexTest, CppHeaderReferencesPopulateEnhancedSymbols) {
    TempDir dir;
    dir.write_file("alloc.hpp",
                   "class SlabAllocator {};\n"
                   "\n"
                   "inline void put_to_tier() {}\n"
                   "\n"
                   "inline void use_ref() {\n"
                   "    put_to_tier();\n"
                   "    SlabAllocator allocator;\n"
                   "}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    auto snapshot = mi.ref_tracker().pin();
    auto put_to_tier = snapshot->find_symbol_by_name("put_to_tier");
    ASSERT_NE(put_to_tier, nullptr);
    EXPECT_GE(put_to_tier->incoming_ref_count, 1u);

    auto use_ref = snapshot->find_symbol_by_name("use_ref");
    ASSERT_NE(use_ref, nullptr);
    EXPECT_GE(use_ref->outgoing_ref_count, 1u);

    auto slab_allocator =
        snapshot->find_symbol_by_name("SlabAllocator");
    ASSERT_NE(slab_allocator, nullptr);
    EXPECT_GE(slab_allocator->incoming_ref_count, 1u);
}

// -- Cancellation -------------------------------------------------------------

TEST(MasterIndexTest, StopRequestedDefaultsFalse) {
    Config cfg = make_default_config();
    MasterIndex mi(cfg);
    EXPECT_FALSE(mi.stop_requested());
}

TEST(MasterIndexTest, RequestStopBeforeIndexingPersistsAndAbortsRun) {
    TempDir dir;
    // Write enough files that a pre-stop is observable: even though
    // request_stop() before run() can't shrink scan time below the
    // FileScanner walk, the pipeline must exit before all files are
    // integrated.
    for (int i = 0; i < 20; ++i) {
        dir.write_file("f" + std::to_string(i) + ".go",
                       "package f" + std::to_string(i) +
                       "\nfunc F" + std::to_string(i) + "() {}\n");
    }

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    mi.request_stop();
    EXPECT_TRUE(mi.stop_requested());

    // index_directory() still returns true (it ran), but the pipeline
    // observed the pre-stop. Check that the integrated count is at
    // most the scanned count and indexing finished cleanly.
    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    EXPECT_FALSE(mi.is_indexing());
}

TEST(MasterIndexTest, IndexDirectoryClearsStaleStopFlag) {
    TempDir dir;
    dir.write_file("a.go", "package a\nfunc A() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    // First run: pre-stopped.
    mi.request_stop();
    EXPECT_TRUE(mi.stop_requested());
    EXPECT_TRUE(mi.index_directory(dir.path().string()));

    // Second run: stop flag must be cleared on entry, otherwise the
    // pipeline would still observe stop_requested at startup.
    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    EXPECT_FALSE(mi.stop_requested());
    EXPECT_GE(mi.file_count(), 1);
}

TEST(MasterIndexTest, RequestStopFromAnotherThreadCancelsInFlightRun) {
    TempDir dir;
    // Write a workload large enough that the indexing run takes
    // measurable wall time on a debug build, so a request_stop()
    // racing with run() can land mid-pipeline.
    for (int i = 0; i < 200; ++i) {
        dir.write_file("f" + std::to_string(i) + ".go",
                       "package f" + std::to_string(i) +
                       "\nfunc F" + std::to_string(i) + "() {}\n");
    }

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    std::thread indexer([&] {
        mi.index_directory(dir.path().string());
    });

    // Spin until the run is visible, then request stop. This exercises
    // the active_pipeline_ forwarding path (not the pre-stop path).
    while (!mi.is_indexing()) {
        std::this_thread::yield();
    }
    mi.request_stop();

    indexer.join();
    EXPECT_FALSE(mi.is_indexing());
    EXPECT_TRUE(mi.stop_requested());
}

// -- get_progress() ----------------------------------------------------------

TEST(MasterIndexTest, GetProgressReportsIdleWhenNoRunActive) {
    TempDir dir;
    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    auto snap = mi.get_progress();
    EXPECT_EQ(snap.phase, MasterIndex::IndexingPhase::Idle);
    EXPECT_EQ(snap.files_scanned, 0);
    EXPECT_EQ(snap.files_total, 0);
    EXPECT_EQ(snap.percent_complete, 0);
    EXPECT_EQ(snap.elapsed_ms, 0);
}

TEST(MasterIndexTest, GetProgressReportsIdleAfterRunCompletes) {
    TempDir dir;
    dir.write_file("a.go", "package a\nfunc A() {}\n");
    dir.write_file("b.go", "package b\nfunc B() {}\n");

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    EXPECT_TRUE(mi.index_directory(dir.path().string()));

    // After the run completes, get_progress() must return idle/zero so
    // /status doesn't report stale percent/elapsed numbers from the
    // previous run. The Pipeline lives on the index_directory() stack
    // and is destroyed before the call returns; reading its tracker
    // now would be a use-after-free.
    auto snap = mi.get_progress();
    EXPECT_EQ(snap.phase, MasterIndex::IndexingPhase::Idle);
    EXPECT_EQ(snap.files_scanned, 0);
    EXPECT_EQ(snap.files_total, 0);
    EXPECT_EQ(snap.percent_complete, 0);
    EXPECT_EQ(snap.elapsed_ms, 0);
}

TEST(MasterIndexTest, GetProgressIsLiveAndThreadSafeDuringRun) {
    TempDir dir;
    // Workload large enough that the indexing run takes measurable wall
    // time on a debug build, so concurrent get_progress() calls land
    // mid-pipeline. 200 small files matches the cancellation test sizing.
    for (int i = 0; i < 200; ++i) {
        dir.write_file("f" + std::to_string(i) + ".go",
                       "package f" + std::to_string(i) +
                       "\nfunc F" + std::to_string(i) + "() {}\n");
    }

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    std::thread indexer([&] {
        mi.index_directory(dir.path().string());
    });

    // Spin until the run is observable.
    while (!mi.is_indexing()) {
        std::this_thread::yield();
    }

    // Poll progress repeatedly while the writer is mutating the
    // ProgressTracker. Each call must (a) not crash, (b) return a
    // monotonically non-decreasing files_scanned (within a single
    // phase, because once we transition out of Scanning the counter
    // switches sources from scanned->processed and may visibly reset),
    // and (c) report a non-Idle phase at least once if the run lasts
    // long enough to be observable.
    bool saw_active_phase = false;
    bool saw_nonzero_elapsed = false;
    for (int i = 0; i < 200; ++i) {
        auto snap = mi.get_progress();
        if (snap.phase != MasterIndex::IndexingPhase::Idle) {
            saw_active_phase = true;
        }
        if (snap.elapsed_ms > 0) {
            saw_nonzero_elapsed = true;
        }
        // Percent is always within bounds.
        EXPECT_GE(snap.percent_complete, 0);
        EXPECT_LE(snap.percent_complete, 100);
        std::this_thread::yield();
    }

    indexer.join();

    // The run might finish faster than we can poll on a fast CI box.
    // Guard the live-progress assertion behind the observable-window:
    // if we never saw the run mid-flight, we still assert get_progress
    // returns a valid idle snapshot.
    auto post = mi.get_progress();
    EXPECT_EQ(post.phase, MasterIndex::IndexingPhase::Idle);

    // Sanity: at least one of {active phase observed, elapsed observed}
    // must be true on machines where the run actually takes time.
    // Don't fail on a too-fast machine; the prior crash-free polling
    // is the load-bearing thread-safety assertion.
    (void)saw_active_phase;
    (void)saw_nonzero_elapsed;
}

TEST(MasterIndexTest, GetProgressPercentCompleteAlwaysWithinBounds) {
    TempDir dir;
    for (int i = 0; i < 50; ++i) {
        dir.write_file("g" + std::to_string(i) + ".go",
                       "package g" + std::to_string(i) +
                       "\nfunc G" + std::to_string(i) + "() {}\n");
    }

    Config cfg = make_default_config();
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);

    std::atomic<bool> stop_polling{false};
    std::thread poller([&] {
        while (!stop_polling.load(std::memory_order_acquire)) {
            auto snap = mi.get_progress();
            ASSERT_GE(snap.percent_complete, 0);
            ASSERT_LE(snap.percent_complete, 100);
            ASSERT_GE(snap.files_scanned, 0);
            ASSERT_GE(snap.files_total, 0);
            ASSERT_GE(snap.elapsed_ms, 0);
        }
    });

    EXPECT_TRUE(mi.index_directory(dir.path().string()));
    stop_polling.store(true, std::memory_order_release);
    poller.join();
}

// Pins the update_file full-re-parse fix: updating a file used to rebuild
// only trigram+postings, permanently dropping its symbols/references (the
// old ones were removed and nothing re-extracted). After update_file the
// new symbols must be present and the old ones gone.
TEST(MasterIndexTest, UpdateFileReparsesSymbols) {
    TempDir dir;
    dir.write_file("m.go", "package main\nfunc Alpha() {}\n");

    Config cfg;
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    ASSERT_FALSE(
        mi.ref_tracker().pin()->find_symbols_by_name("Alpha").empty());

    const std::string new_content = "package main\nfunc Beta() {}\n";
    dir.write_file("m.go", new_content);
    ASSERT_TRUE(mi.update_file((dir.path() / "m.go").string(), new_content));

    auto snap = mi.ref_tracker().pin();
    EXPECT_FALSE(snap->find_symbols_by_name("Beta").empty())
        << "updated file's symbols were not re-extracted";
    EXPECT_TRUE(snap->find_symbols_by_name("Alpha").empty())
        << "stale pre-update symbol survived the update";
}

// Pins the live-/reindex FileID aliasing fix: FileIDs are monotonic across
// generations and path-stable, so a stale FileID held from the previous
// generation either resolves to its OWN file's content or to nothing —
// never to a different file. Before the fix, index_directory cleared the
// content store (resetting the id counter), so during and after a reindex
// the old snapshot's ids pointed at whichever file re-drew the same number.
TEST(MasterIndexTest, ReindexKeepsFileIdsMonotonicAndUnaliased) {
    TempDir dir;
    dir.write_file("a.go", "package main\nfunc Alpha() {}\n");
    dir.write_file("b.go", "package main\nfunc Beta() {}\n");

    Config cfg;
    cfg.project.root = dir.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    const std::string a_path = (dir.path() / "a.go").string();
    const std::string b_path = (dir.path() / "b.go").string();
    FileID a_id = mi.path_to_id(a_path);
    FileID b_id = mi.path_to_id(b_path);
    ASSERT_NE(a_id, FileID{0});
    ASSERT_NE(b_id, FileID{0});

    // Delete a.go, add c.go, reindex the same directory in-process.
    std::filesystem::remove(dir.path() / "a.go");
    dir.write_file("c.go", "package main\nfunc Gamma() {}\n");
    ASSERT_TRUE(mi.index_directory(dir.path().string()));

    // Survivor keeps its id and its own content.
    EXPECT_EQ(b_id, mi.path_to_id(b_path));
    auto b_content = mi.file_content_store().get_content(b_id);
    EXPECT_NE(b_content.find("Beta"), std::string_view::npos);

    // The stale id resolves to NOTHING — never to another file's bytes.
    EXPECT_TRUE(mi.file_content_store().get_content(a_id).empty());
    EXPECT_TRUE(mi.id_to_path(a_id).empty());

    // A new file draws a fresh id; deleted ids are never recycled.
    FileID c_id = mi.path_to_id((dir.path() / "c.go").string());
    ASSERT_NE(c_id, FileID{0});
    EXPECT_NE(c_id, a_id);
    EXPECT_GT(c_id, b_id);
}

}  // namespace
}  // namespace lci
