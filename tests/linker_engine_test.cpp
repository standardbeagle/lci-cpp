#include <gtest/gtest.h>

#include <lci/symbollinker/linker_engine.h>

#include <string>
#include <string_view>
#include <vector>

namespace lci::symbollinker {
namespace {

// ---------------------------------------------------------------------------
// Stub extractor for testing the engine dispatch and lifecycle.
// ---------------------------------------------------------------------------
class StubExtractor : public SymbolExtractor {
  public:
    explicit StubExtractor(parser::Language lang,
                           std::vector<std::string> exts)
        : lang_(lang), exts_(std::move(exts)) {}

    SymbolTable extract_symbols(FileID file_id, std::string_view /*content*/,
                                TSTree* /*tree*/) override {
        SymbolTable table;
        table.file_id = file_id;
        table.language = lang_;
        table.symbol_ids.push_back(static_cast<SymbolID>(file_id) * 1000 + 1);
        table.symbol_names.push_back("stub_symbol");
        table.next_local_id = 2;
        ++extract_count;
        return table;
    }

    parser::Language language() const override { return lang_; }

    bool can_handle(std::string_view path) const override {
        for (const auto& ext : exts_) {
            if (path.size() >= ext.size() &&
                path.substr(path.size() - ext.size()) == ext) {
                return true;
            }
        }
        return false;
    }

    int extract_count{};

  private:
    parser::Language lang_;
    std::vector<std::string> exts_;
};

// ---------------------------------------------------------------------------
// Stub resolver for testing import resolution.
// ---------------------------------------------------------------------------
class StubResolver : public ImportResolver {
  public:
    explicit StubResolver(parser::Language lang) : lang_(lang) {}

    ModuleResolution resolve_import(std::string_view import_path,
                                    FileID /*from_file*/) override {
        ModuleResolution res;
        res.request_path = std::string(import_path);

        auto it = resolved_files.find(std::string(import_path));
        if (it != resolved_files.end()) {
            res.file_id = it->second;
            res.resolved_path = std::string(import_path);
            res.is_external = false;
        } else {
            res.is_external = true;
        }

        ++resolve_count;
        return res;
    }

    parser::Language language() const override { return lang_; }

    absl::flat_hash_map<std::string, FileID> resolved_files;
    int resolve_count{};

  private:
    parser::Language lang_;
};

// ---------------------------------------------------------------------------
// Extractor that produces imports and exports for linking tests.
// ---------------------------------------------------------------------------
class LinkableExtractor : public SymbolExtractor {
  public:
    explicit LinkableExtractor(parser::Language lang,
                               std::vector<std::string> exts)
        : lang_(lang), exts_(std::move(exts)) {}

    SymbolTable extract_symbols(FileID file_id, std::string_view /*content*/,
                                TSTree* /*tree*/) override {
        auto it = tables.find(file_id);
        if (it != tables.end()) {
            return it->second;
        }

        SymbolTable table;
        table.file_id = file_id;
        table.language = lang_;
        return table;
    }

    parser::Language language() const override { return lang_; }

    bool can_handle(std::string_view path) const override {
        for (const auto& ext : exts_) {
            if (path.size() >= ext.size() &&
                path.substr(path.size() - ext.size()) == ext) {
                return true;
            }
        }
        return false;
    }

    absl::flat_hash_map<FileID, SymbolTable> tables;

  private:
    parser::Language lang_;
    std::vector<std::string> exts_;
};

// ---------------------------------------------------------------------------
// LinkerEngine - construction and basic operations
// ---------------------------------------------------------------------------

TEST(LinkerEngineTest, EmptyEngine) {
    LinkerEngine engine("/project");
    auto s = engine.stats();
    EXPECT_EQ(s.files, 0);
    EXPECT_EQ(s.symbol_links, 0);
    EXPECT_EQ(s.extractors, 0);
    EXPECT_EQ(s.resolvers, 0);
    EXPECT_EQ(engine.root_path(), "/project");
}

TEST(LinkerEngineTest, RegisterExtractor) {
    LinkerEngine engine("/project");
    engine.register_extractor(std::make_unique<StubExtractor>(
        parser::Language::Go, std::vector<std::string>{".go"}));

    auto s = engine.stats();
    EXPECT_EQ(s.extractors, 1);
}

TEST(LinkerEngineTest, RegisterMultipleExtractors) {
    LinkerEngine engine("/project");
    engine.register_extractor(std::make_unique<StubExtractor>(
        parser::Language::Go, std::vector<std::string>{".go"}));
    engine.register_extractor(std::make_unique<StubExtractor>(
        parser::Language::Python, std::vector<std::string>{".py"}));
    engine.register_extractor(std::make_unique<StubExtractor>(
        parser::Language::JavaScript, std::vector<std::string>{".js"}));

    auto s = engine.stats();
    EXPECT_EQ(s.extractors, 3);
}

TEST(LinkerEngineTest, RegisterResolver) {
    LinkerEngine engine("/project");
    engine.register_resolver(
        std::make_unique<StubResolver>(parser::Language::Go));

    auto s = engine.stats();
    EXPECT_EQ(s.resolvers, 1);
}

// ---------------------------------------------------------------------------
// LinkerEngine - file ID management
// ---------------------------------------------------------------------------

TEST(LinkerEngineTest, GetOrCreateFileId) {
    LinkerEngine engine("/project");

    FileID id1 = engine.get_or_create_file_id("/project/main.go");
    FileID id2 = engine.get_or_create_file_id("/project/main.go");
    FileID id3 = engine.get_or_create_file_id("/project/util.go");

    EXPECT_EQ(id1, id2);
    EXPECT_NE(id1, id3);
}

TEST(LinkerEngineTest, GetFilePath) {
    LinkerEngine engine("/project");

    FileID id = engine.get_or_create_file_id("/project/main.go");
    EXPECT_EQ(engine.get_file_path(id), "/project/main.go");
    EXPECT_TRUE(engine.get_file_path(999).empty());
}

// ---------------------------------------------------------------------------
// LinkerEngine - indexing with no extractor returns false
// ---------------------------------------------------------------------------

TEST(LinkerEngineTest, IndexFileNoExtractor) {
    LinkerEngine engine("/project");
    EXPECT_FALSE(engine.index_file("/project/main.go", "package main"));
}

// ---------------------------------------------------------------------------
// LinkerEngine - linking dispatches to extractors
// ---------------------------------------------------------------------------

TEST(LinkerEngineTest, LinkSymbolsEmpty) {
    LinkerEngine engine("/project");
    EXPECT_TRUE(engine.link_symbols());
    auto s = engine.stats();
    EXPECT_EQ(s.symbol_links, 0);
}

// ---------------------------------------------------------------------------
// LinkerEngine - incremental updates
// ---------------------------------------------------------------------------

TEST(LinkerEngineTest, UpdateFileUnchangedNoOp) {
    LinkerEngine engine("/project");
    auto ext = std::make_unique<StubExtractor>(
        parser::Language::Go, std::vector<std::string>{".go"});
    auto* ext_ptr = ext.get();
    engine.register_extractor(std::move(ext));

    // First update indexes the file.
    UpdateResult r1 = engine.update_file("/project/main.go", "package main");
    EXPECT_EQ(r1.updated_files.size(), 1u);
    EXPECT_EQ(ext_ptr->extract_count, 1);

    // Same content again should be a no-op.
    UpdateResult r2 = engine.update_file("/project/main.go", "package main");
    EXPECT_TRUE(r2.updated_files.empty());
    EXPECT_EQ(ext_ptr->extract_count, 1);
}

TEST(LinkerEngineTest, UpdateFileModified) {
    LinkerEngine engine("/project");
    auto ext = std::make_unique<StubExtractor>(
        parser::Language::Go, std::vector<std::string>{".go"});
    auto* ext_ptr = ext.get();
    engine.register_extractor(std::move(ext));

    engine.update_file("/project/main.go", "package main");
    EXPECT_EQ(ext_ptr->extract_count, 1);

    UpdateResult r = engine.update_file("/project/main.go", "package main\n");
    EXPECT_EQ(r.updated_files.size(), 1u);
    EXPECT_EQ(ext_ptr->extract_count, 2);
}

TEST(LinkerEngineTest, RemoveFile) {
    LinkerEngine engine("/project");
    engine.register_extractor(std::make_unique<StubExtractor>(
        parser::Language::Go, std::vector<std::string>{".go"}));

    engine.update_file("/project/main.go", "package main");
    EXPECT_EQ(engine.stats().files, 1);

    UpdateResult r = engine.remove_file("/project/main.go");
    EXPECT_EQ(r.updated_files.size(), 1u);
    EXPECT_EQ(engine.stats().files, 0);
}

TEST(LinkerEngineTest, RemoveNonexistentFileNoOp) {
    LinkerEngine engine("/project");
    UpdateResult r = engine.remove_file("/project/nonexistent.go");
    EXPECT_TRUE(r.updated_files.empty());
}

// ---------------------------------------------------------------------------
// LinkerEngine - dependency graph and cascading
// ---------------------------------------------------------------------------

TEST(LinkerEngineTest, DependencyTracking) {
    LinkerEngine engine("/project");

    auto ext = std::make_unique<LinkableExtractor>(
        parser::Language::Go, std::vector<std::string>{".go"});
    auto* ext_ptr = ext.get();

    auto resolver = std::make_unique<StubResolver>(parser::Language::Go);
    auto* resolver_ptr = resolver.get();

    engine.register_extractor(std::move(ext));
    engine.register_resolver(std::move(resolver));

    FileID util_id = engine.get_or_create_file_id("/project/util.go");
    FileID main_id = engine.get_or_create_file_id("/project/main.go");

    // util.go exports a symbol.
    SymbolTable util_table;
    util_table.file_id = util_id;
    util_table.language = parser::Language::Go;
    util_table.symbol_ids.push_back(1001);
    util_table.symbol_names.push_back("Helper");
    ExportInfo exp;
    exp.exported_name = "Helper";
    exp.local_name = "Helper";
    util_table.exports.push_back(exp);
    ext_ptr->tables[util_id] = util_table;

    // main.go imports from util.go.
    SymbolTable main_table;
    main_table.file_id = main_id;
    main_table.language = parser::Language::Go;
    main_table.symbol_ids.push_back(2001);
    main_table.symbol_names.push_back("main");
    ImportInfo imp;
    imp.import_path = "/project/util.go";
    imp.imported_names.push_back("Helper");
    main_table.imports.push_back(imp);
    ext_ptr->tables[main_id] = main_table;

    resolver_ptr->resolved_files["/project/util.go"] = util_id;

    engine.index_file("/project/util.go", "dummy");
    engine.index_file("/project/main.go", "dummy");
    engine.link_symbols();

    auto deps = engine.get_file_dependencies(main_id);
    EXPECT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0], util_id);

    auto dependents = engine.get_file_dependents(util_id);
    EXPECT_EQ(dependents.size(), 1u);
    EXPECT_EQ(dependents[0], main_id);

    auto imports = engine.get_file_imports(main_id);
    EXPECT_EQ(imports.size(), 1u);
    EXPECT_EQ(imports[0]->import_path, "/project/util.go");
    EXPECT_FALSE(imports[0]->is_external);
}

TEST(LinkerEngineTest, FileHashTracking) {
    LinkerEngine engine("/project");
    engine.register_extractor(std::make_unique<StubExtractor>(
        parser::Language::Go, std::vector<std::string>{".go"}));

    EXPECT_EQ(engine.get_file_hash(1), 0u);

    engine.update_file("/project/main.go", "package main");
    FileID fid = engine.get_or_create_file_id("/project/main.go");
    EXPECT_NE(engine.get_file_hash(fid), 0u);
}

TEST(LinkerEngineTest, GetSymbolTable) {
    LinkerEngine engine("/project");
    engine.register_extractor(std::make_unique<StubExtractor>(
        parser::Language::Go, std::vector<std::string>{".go"}));

    EXPECT_EQ(engine.get_symbol_table(1), nullptr);

    engine.update_file("/project/main.go", "package main");
    FileID fid = engine.get_or_create_file_id("/project/main.go");
    const SymbolTable* table = engine.get_symbol_table(fid);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->file_id, fid);
    EXPECT_EQ(table->symbol_names.size(), 1u);
}

TEST(LinkerEngineTest, StatsAfterIndexing) {
    LinkerEngine engine("/project");
    engine.register_extractor(std::make_unique<StubExtractor>(
        parser::Language::Go, std::vector<std::string>{".go"}));
    engine.register_extractor(std::make_unique<StubExtractor>(
        parser::Language::Python, std::vector<std::string>{".py"}));
    engine.register_resolver(
        std::make_unique<StubResolver>(parser::Language::Go));

    engine.update_file("/project/main.go", "package main");
    engine.update_file("/project/app.py", "import os");

    auto s = engine.stats();
    EXPECT_EQ(s.files, 2);
    EXPECT_EQ(s.extractors, 2);
    EXPECT_EQ(s.resolvers, 1);
}

// -- register_all_linkers (multi-language debug-path wiring) -------------------

TEST(LinkerEngineTest, RegisterAllLinkersCanIndexAllLanguages) {
    LinkerEngine engine("/project");
    register_all_linkers(engine, "/project");

    // Every built-in linker language is indexable...
    EXPECT_TRUE(engine.can_index("/p/main.go"));
    EXPECT_TRUE(engine.can_index("/p/app.py"));
    EXPECT_TRUE(engine.can_index("/p/app.js"));
    EXPECT_TRUE(engine.can_index("/p/app.jsx"));
    EXPECT_TRUE(engine.can_index("/p/app.ts"));
    EXPECT_TRUE(engine.can_index("/p/app.tsx"));
    EXPECT_TRUE(engine.can_index("/p/Program.cs"));
    EXPECT_TRUE(engine.can_index("/p/index.php"));
    // ...languages without a linker pair are not.
    EXPECT_FALSE(engine.can_index("/p/main.rs"));
    EXPECT_FALSE(engine.can_index("/p/Main.java"));
    EXPECT_FALSE(engine.can_index("/p/notes.txt"));

    auto s = engine.stats();
    EXPECT_EQ(s.extractors, 6);  // go, py, js, ts, cs, php
    EXPECT_EQ(s.resolvers, 6);
}

// End-to-end: register_all_linkers + index a 2-file JS project + link, with NO
// manual set_file_registry call — proves link_symbols injects the engine
// registry so a real cross-file dependency edge forms (the debug-path fix).
TEST(LinkerEngineTest, RegisterAllLinkersFormsDependencyEdge) {
    LinkerEngine engine("/project");
    register_all_linkers(engine, "/project");

    FileID utils_id = engine.get_or_create_file_id("/project/utils.js");
    FileID app_id = engine.get_or_create_file_id("/project/app.js");

    ASSERT_TRUE(engine.index_file(
        "/project/utils.js",
        "export function formatName(name) { return name.trim(); }\n"));
    ASSERT_TRUE(engine.index_file(
        "/project/app.js",
        "import { formatName } from './utils';\n"
        "console.log(formatName('x'));\n"));
    ASSERT_TRUE(engine.link_symbols());

    auto deps = engine.get_file_dependencies(app_id);
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0], utils_id);
}

}  // namespace
}  // namespace lci::symbollinker
