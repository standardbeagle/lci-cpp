#include <gtest/gtest.h>

#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>
#include <lci/symbollinker/go_linker.h>
#include <lci/symbollinker/js_linker.h>
#include <lci/symbollinker/linker_engine.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

namespace lci::symbollinker {
namespace {

// Parses source code using the appropriate tree-sitter grammar.
parser::UniqueTree parse(parser::Language lang, std::string_view src) {
    parser::UniqueParser p = parser::make_parser(lang);
    if (!p) return nullptr;
    TSTree* tree = ts_parser_parse_string(
        p.get(), nullptr, src.data(), static_cast<uint32_t>(src.size()));
    return parser::UniqueTree(tree);
}

// Returns true if a symbol name exists in the table.
bool has_symbol(const SymbolTable& t, std::string_view name) {
    return std::find(t.symbol_names.begin(), t.symbol_names.end(), name) !=
           t.symbol_names.end();
}

// Returns true if an import with the given path exists.
bool has_import(const SymbolTable& t, std::string_view path) {
    for (const auto& imp : t.imports) {
        if (imp.import_path == path) return true;
    }
    return false;
}

// Returns true if an export with the given name exists.
bool has_export(const SymbolTable& t, std::string_view name) {
    for (const auto& exp : t.exports) {
        if (exp.exported_name == name) return true;
    }
    return false;
}

// =========================================================================
// GoExtractor tests
// =========================================================================

TEST(GoExtractorTest, CanHandleGoFiles) {
    GoExtractor ext;
    EXPECT_TRUE(ext.can_handle("/project/main.go"));
    EXPECT_TRUE(ext.can_handle("util.go"));
    EXPECT_FALSE(ext.can_handle("/project/main.py"));
    EXPECT_FALSE(ext.can_handle("/project/main.js"));
}

TEST(GoExtractorTest, Language) {
    GoExtractor ext;
    EXPECT_EQ(ext.language(), parser::Language::Go);
}

TEST(GoExtractorTest, NullTreeReturnsEmptyTable) {
    GoExtractor ext;
    SymbolTable t = ext.extract_symbols(1, "package main", nullptr);
    EXPECT_EQ(t.file_id, 1u);
    EXPECT_TRUE(t.symbol_names.empty());
}

TEST(GoExtractorTest, ExtractFunction) {
    GoExtractor ext;
    auto tree = parse(parser::Language::Go,
                      "package main\n\nfunc Hello() {}\nfunc helper() {}");
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, "package main\n\nfunc Hello() {}\nfunc helper() {}", tree.get());

    EXPECT_TRUE(has_symbol(t, "Hello"));
    EXPECT_TRUE(has_symbol(t, "helper"));
    // Hello is exported (capitalized), helper is not.
    EXPECT_TRUE(has_export(t, "Hello"));
    EXPECT_FALSE(has_export(t, "helper"));
}

TEST(GoExtractorTest, ExtractMethod) {
    GoExtractor ext;
    std::string_view src =
        "package main\n\ntype Server struct{}\n\n"
        "func (s *Server) Start() {}\n"
        "func (s Server) stop() {}";
    auto tree = parse(parser::Language::Go, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Server.Start"));
    EXPECT_TRUE(has_symbol(t, "Server.stop"));
    EXPECT_TRUE(has_export(t, "Server.Start"));
    EXPECT_FALSE(has_export(t, "Server.stop"));
}

TEST(GoExtractorTest, ExtractStruct) {
    GoExtractor ext;
    std::string_view src =
        "package main\n\ntype Config struct {\n"
        "\tHost string\n\tport int\n}";
    auto tree = parse(parser::Language::Go, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Config"));
    EXPECT_TRUE(has_symbol(t, "Config.Host"));
    EXPECT_TRUE(has_symbol(t, "Config.port"));
    EXPECT_TRUE(has_export(t, "Config"));
    EXPECT_TRUE(has_export(t, "Config.Host"));
    EXPECT_FALSE(has_export(t, "Config.port"));
}

TEST(GoExtractorTest, ExtractInterface) {
    GoExtractor ext;
    std::string_view src =
        "package main\n\ntype Reader interface {\n"
        "\tRead(p []byte) (int, error)\n}";
    auto tree = parse(parser::Language::Go, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Reader"));
    EXPECT_TRUE(has_symbol(t, "Reader.Read"));
    EXPECT_TRUE(has_export(t, "Reader"));
}

TEST(GoExtractorTest, ExtractImports) {
    GoExtractor ext;
    std::string_view src =
        "package main\n\nimport (\n"
        "\t\"fmt\"\n"
        "\t\"os\"\n"
        "\tlog \"github.com/sirupsen/logrus\"\n"
        ")\n";
    auto tree = parse(parser::Language::Go, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_import(t, "fmt"));
    EXPECT_TRUE(has_import(t, "os"));
    EXPECT_TRUE(has_import(t, "github.com/sirupsen/logrus"));

    // Check alias for logrus.
    for (const auto& imp : t.imports) {
        if (imp.import_path == "github.com/sirupsen/logrus") {
            EXPECT_EQ(imp.alias, "log");
        }
    }
}

TEST(GoExtractorTest, ExtractVarAndConst) {
    GoExtractor ext;
    std::string_view src =
        "package main\n\nvar Version = \"1.0\"\nvar count int\n"
        "const MaxRetries = 3\nconst debug = false";
    auto tree = parse(parser::Language::Go, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Version"));
    EXPECT_TRUE(has_symbol(t, "count"));
    EXPECT_TRUE(has_symbol(t, "MaxRetries"));
    EXPECT_TRUE(has_symbol(t, "debug"));
    EXPECT_TRUE(has_export(t, "Version"));
    EXPECT_TRUE(has_export(t, "MaxRetries"));
    EXPECT_FALSE(has_export(t, "count"));
    EXPECT_FALSE(has_export(t, "debug"));
}

// =========================================================================
// GoResolver tests
// =========================================================================

TEST(GoResolverTest, StandardPackage) {
    GoResolver resolver("/project");

    auto res = resolver.resolve_import("fmt", 1);
    EXPECT_TRUE(res.is_builtin);
    EXPECT_FALSE(res.is_external);

    res = resolver.resolve_import("net/http", 1);
    EXPECT_TRUE(res.is_builtin);
}

TEST(GoResolverTest, ExternalPackage) {
    GoResolver resolver("/project");

    auto res = resolver.resolve_import("github.com/pkg/errors", 1);
    EXPECT_TRUE(res.is_external);
    EXPECT_FALSE(res.is_builtin);
}

TEST(GoResolverTest, ModuleImport) {
    GoResolver resolver("/project");
    resolver.set_module_name("github.com/myorg/myapp");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/internal/util/util.go"] = 10;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import(
        "github.com/myorg/myapp/internal/util", 1);
    EXPECT_FALSE(res.is_external);
    EXPECT_EQ(res.file_id, 10u);
}

TEST(GoResolverTest, ModuleImportNotFound) {
    GoResolver resolver("/project");
    resolver.set_module_name("github.com/myorg/myapp");
    resolver.set_file_registry({});

    auto res = resolver.resolve_import(
        "github.com/myorg/myapp/notexist", 1);
    EXPECT_TRUE(res.is_external);
}

// =========================================================================
// JSExtractor tests
// =========================================================================

TEST(JSExtractorTest, CanHandleJSFiles) {
    JSExtractor ext(false);
    EXPECT_TRUE(ext.can_handle("/project/app.js"));
    EXPECT_TRUE(ext.can_handle("component.jsx"));
    EXPECT_TRUE(ext.can_handle("util.mjs"));
    EXPECT_FALSE(ext.can_handle("app.ts"));
    EXPECT_FALSE(ext.can_handle("main.go"));
}

TEST(JSExtractorTest, CanHandleTSFiles) {
    JSExtractor ext(true);
    EXPECT_TRUE(ext.can_handle("app.ts"));
    EXPECT_TRUE(ext.can_handle("component.tsx"));
    EXPECT_TRUE(ext.can_handle("util.mts"));
    EXPECT_FALSE(ext.can_handle("app.js"));
    EXPECT_FALSE(ext.can_handle("main.go"));
}

TEST(JSExtractorTest, Language) {
    JSExtractor js(false);
    EXPECT_EQ(js.language(), parser::Language::JavaScript);

    JSExtractor ts(true);
    EXPECT_EQ(ts.language(), parser::Language::TypeScript);
}

TEST(JSExtractorTest, NullTreeReturnsEmptyTable) {
    JSExtractor ext(false);
    SymbolTable t = ext.extract_symbols(1, "const x = 1;", nullptr);
    EXPECT_TRUE(t.symbol_names.empty());
}

TEST(JSExtractorTest, ExtractFunction) {
    JSExtractor ext(false);
    std::string_view src = "function greet(name) { return name; }\n"
                           "function helper() {}";
    auto tree = parse(parser::Language::JavaScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "greet"));
    EXPECT_TRUE(has_symbol(t, "helper"));
}

TEST(JSExtractorTest, ExtractExportedFunction) {
    JSExtractor ext(false);
    std::string_view src = "export function greet() {}\nfunction internal() {}";
    auto tree = parse(parser::Language::JavaScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "greet"));
    EXPECT_TRUE(has_export(t, "greet"));
    EXPECT_TRUE(has_symbol(t, "internal"));
    EXPECT_FALSE(has_export(t, "internal"));
}

TEST(JSExtractorTest, ExtractClass) {
    JSExtractor ext(false);
    std::string_view src =
        "class MyClass {\n"
        "  constructor() {}\n"
        "  doWork() {}\n"
        "}";
    auto tree = parse(parser::Language::JavaScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "MyClass"));
    EXPECT_TRUE(has_symbol(t, "MyClass.constructor"));
    EXPECT_TRUE(has_symbol(t, "MyClass.doWork"));
}

TEST(JSExtractorTest, ExtractNamedImports) {
    JSExtractor ext(false);
    std::string_view src =
        "import { useState, useEffect } from 'react';\n"
        "import defaultExport from './local';";
    auto tree = parse(parser::Language::JavaScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_import(t, "react"));
    EXPECT_TRUE(has_import(t, "./local"));

    // Check named imports from react.
    for (const auto& imp : t.imports) {
        if (imp.import_path == "react" && !imp.imported_names.empty()) {
            EXPECT_EQ(imp.imported_names.size(), 2u);
        }
    }

    // Check default import.
    for (const auto& imp : t.imports) {
        if (imp.import_path == "./local") {
            EXPECT_TRUE(imp.is_default);
            EXPECT_EQ(imp.alias, "defaultExport");
        }
    }
}

TEST(JSExtractorTest, ExtractNamespaceImport) {
    JSExtractor ext(false);
    std::string_view src = "import * as React from 'react';";
    auto tree = parse(parser::Language::JavaScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_import(t, "react"));
    for (const auto& imp : t.imports) {
        if (imp.import_path == "react" && imp.is_namespace) {
            EXPECT_EQ(imp.alias, "React");
        }
    }
}

TEST(JSExtractorTest, ExtractNamedExports) {
    JSExtractor ext(false);
    std::string_view src = "const x = 1;\nconst y = 2;\nexport { x, y };";
    auto tree = parse(parser::Language::JavaScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_export(t, "x"));
    EXPECT_TRUE(has_export(t, "y"));
}

TEST(JSExtractorTest, ExtractDefaultExport) {
    JSExtractor ext(false);
    std::string_view src = "function App() {}\nexport default App;";
    auto tree = parse(parser::Language::JavaScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_export(t, "default"));
}

TEST(JSExtractorTest, ExtractReExport) {
    JSExtractor ext(false);
    std::string_view src = "export { foo, bar } from './utils';";
    auto tree = parse(parser::Language::JavaScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    bool found_re_export = false;
    for (const auto& exp : t.exports) {
        if (exp.is_re_export && exp.source_path == "./utils") {
            found_re_export = true;
        }
    }
    EXPECT_TRUE(found_re_export);
}

TEST(JSExtractorTest, ExtractVariables) {
    JSExtractor ext(false);
    std::string_view src = "const API_URL = 'http://example.com';\n"
                           "let counter = 0;\nvar name = 'test';";
    auto tree = parse(parser::Language::JavaScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "API_URL"));
    EXPECT_TRUE(has_symbol(t, "counter"));
    EXPECT_TRUE(has_symbol(t, "name"));
}

// =========================================================================
// TypeScript-specific tests
// =========================================================================

TEST(TSExtractorTest, ExtractInterface) {
    JSExtractor ext(true);
    std::string_view src =
        "export interface User {\n"
        "  name: string;\n"
        "  age: number;\n"
        "}";
    auto tree = parse(parser::Language::TypeScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "User"));
    EXPECT_TRUE(has_export(t, "User"));
}

TEST(TSExtractorTest, ExtractTypeAlias) {
    JSExtractor ext(true);
    std::string_view src = "export type ID = string | number;";
    auto tree = parse(parser::Language::TypeScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "ID"));
    EXPECT_TRUE(has_export(t, "ID"));
}

TEST(TSExtractorTest, ExtractEnum) {
    JSExtractor ext(true);
    std::string_view src =
        "enum Direction {\n"
        "  Up = 'UP',\n"
        "  Down = 'DOWN',\n"
        "}";
    auto tree = parse(parser::Language::TypeScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Direction"));
}

TEST(TSExtractorTest, TypeOnlyImport) {
    JSExtractor ext(true);
    std::string_view src = "import type { User } from './types';";
    auto tree = parse(parser::Language::TypeScript, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_import(t, "./types"));
    for (const auto& imp : t.imports) {
        if (imp.import_path == "./types") {
            EXPECT_TRUE(imp.is_type_only);
        }
    }
}

// =========================================================================
// JSResolver tests
// =========================================================================

TEST(JSResolverTest, BuiltinModule) {
    JSResolver resolver("/project");

    auto res = resolver.resolve_import("fs", 1);
    EXPECT_TRUE(res.is_builtin);

    res = resolver.resolve_import("node:path", 1);
    EXPECT_TRUE(res.is_builtin);
}

TEST(JSResolverTest, RelativeImport) {
    JSResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/src/utils.js"] = 10;
    registry["/project/src/app.js"] = 20;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("./utils", 20);
    EXPECT_EQ(res.file_id, 10u);
    EXPECT_FALSE(res.is_external);
}

TEST(JSResolverTest, RelativeImportWithExtension) {
    JSResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/src/utils.ts"] = 10;
    registry["/project/src/app.ts"] = 20;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("./utils", 20);
    EXPECT_EQ(res.file_id, 10u);
}

TEST(JSResolverTest, RelativeImportParentDir) {
    JSResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/shared/types.ts"] = 10;
    registry["/project/src/app.ts"] = 20;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("../shared/types", 20);
    EXPECT_EQ(res.file_id, 10u);
}

TEST(JSResolverTest, IndexFileResolution) {
    JSResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/src/utils/index.js"] = 10;
    registry["/project/src/app.js"] = 20;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("./utils", 20);
    EXPECT_EQ(res.file_id, 10u);
}

TEST(JSResolverTest, ExternalPackage) {
    JSResolver resolver("/project");
    resolver.set_file_registry({});

    auto res = resolver.resolve_import("lodash", 1);
    EXPECT_TRUE(res.is_external);
}

// =========================================================================
// TSResolver tests
// =========================================================================

TEST(TSResolverTest, Language) {
    TSResolver resolver("/project");
    EXPECT_EQ(resolver.language(), parser::Language::TypeScript);
}

TEST(TSResolverTest, DelegatesResolution) {
    TSResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/src/types.ts"] = 10;
    registry["/project/src/app.ts"] = 20;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("./types", 20);
    EXPECT_EQ(res.file_id, 10u);
}

// =========================================================================
// Integration tests with LinkerEngine
// =========================================================================

TEST(GoLinkerIntegrationTest, CrossFileSymbolResolution) {
    LinkerEngine engine("/project");

    auto ext = std::make_unique<GoExtractor>();
    auto resolver = std::make_unique<GoResolver>("/project");
    resolver->set_module_name("github.com/myorg/myapp");

    absl::flat_hash_map<std::string, FileID> file_reg;

    // Register files first to get stable IDs.
    FileID util_id = engine.get_or_create_file_id("/project/util.go");
    FileID main_id = engine.get_or_create_file_id("/project/main.go");
    file_reg["/project/util.go"] = util_id;
    file_reg["/project/main.go"] = main_id;
    resolver->set_file_registry(file_reg);

    engine.register_extractor(std::move(ext));
    engine.register_resolver(std::move(resolver));

    std::string_view util_src =
        "package util\n\nfunc Helper() string { return \"help\" }\n";
    std::string_view main_src =
        "package main\n\nimport \"github.com/myorg/myapp/util\"\n\n"
        "func main() { util.Helper() }\n";

    EXPECT_TRUE(engine.index_file("/project/util.go", util_src));
    EXPECT_TRUE(engine.index_file("/project/main.go", main_src));
    EXPECT_TRUE(engine.link_symbols());

    // Verify util.go has exported symbol.
    const SymbolTable* util_table = engine.get_symbol_table(util_id);
    ASSERT_NE(util_table, nullptr);
    EXPECT_TRUE(has_symbol(*util_table, "Helper"));
    EXPECT_TRUE(has_export(*util_table, "Helper"));

    // Verify main.go has imports.
    const SymbolTable* main_table = engine.get_symbol_table(main_id);
    ASSERT_NE(main_table, nullptr);
    EXPECT_TRUE(has_import(*main_table, "github.com/myorg/myapp/util"));
}

TEST(JSLinkerIntegrationTest, CrossFileSymbolResolution) {
    LinkerEngine engine("/project");

    auto js_ext = std::make_unique<JSExtractor>(false);
    auto js_resolver = std::make_unique<JSResolver>("/project");

    absl::flat_hash_map<std::string, FileID> file_reg;

    FileID utils_id = engine.get_or_create_file_id("/project/utils.js");
    FileID app_id = engine.get_or_create_file_id("/project/app.js");
    file_reg["/project/utils.js"] = utils_id;
    file_reg["/project/app.js"] = app_id;
    js_resolver->set_file_registry(file_reg);

    engine.register_extractor(std::move(js_ext));
    engine.register_resolver(std::move(js_resolver));

    std::string_view utils_src =
        "export function formatName(name) { return name.trim(); }\n"
        "export const VERSION = '1.0';\n";
    std::string_view app_src =
        "import { formatName, VERSION } from './utils';\n"
        "console.log(formatName('test'), VERSION);\n";

    EXPECT_TRUE(engine.index_file("/project/utils.js", utils_src));
    EXPECT_TRUE(engine.index_file("/project/app.js", app_src));
    EXPECT_TRUE(engine.link_symbols());

    const SymbolTable* utils_table = engine.get_symbol_table(utils_id);
    ASSERT_NE(utils_table, nullptr);
    EXPECT_TRUE(has_symbol(*utils_table, "formatName"));
    EXPECT_TRUE(has_export(*utils_table, "formatName"));

    const SymbolTable* app_table = engine.get_symbol_table(app_id);
    ASSERT_NE(app_table, nullptr);
    EXPECT_TRUE(has_import(*app_table, "./utils"));

    // Verify dependency tracking.
    auto deps = engine.get_file_dependencies(app_id);
    EXPECT_EQ(deps.size(), 1u);
    if (!deps.empty()) {
        EXPECT_EQ(deps[0], utils_id);
    }
}

TEST(TSLinkerIntegrationTest, TypeScriptCrossFile) {
    LinkerEngine engine("/project");

    auto ts_ext = std::make_unique<JSExtractor>(true);
    auto ts_resolver = std::make_unique<TSResolver>("/project");

    absl::flat_hash_map<std::string, FileID> file_reg;

    FileID types_id = engine.get_or_create_file_id("/project/types.ts");
    FileID app_id = engine.get_or_create_file_id("/project/app.ts");
    file_reg["/project/types.ts"] = types_id;
    file_reg["/project/app.ts"] = app_id;
    ts_resolver->set_file_registry(file_reg);

    engine.register_extractor(std::move(ts_ext));
    engine.register_resolver(std::move(ts_resolver));

    std::string_view types_src =
        "export interface User { name: string; }\n"
        "export type ID = string;\n";
    std::string_view app_src =
        "import type { User, ID } from './types';\n"
        "const user: User = { name: 'Alice' };\n";

    EXPECT_TRUE(engine.index_file("/project/types.ts", types_src));
    EXPECT_TRUE(engine.index_file("/project/app.ts", app_src));
    EXPECT_TRUE(engine.link_symbols());

    const SymbolTable* types_table = engine.get_symbol_table(types_id);
    ASSERT_NE(types_table, nullptr);
    EXPECT_TRUE(has_symbol(*types_table, "User"));
    EXPECT_TRUE(has_symbol(*types_table, "ID"));

    const SymbolTable* app_table = engine.get_symbol_table(app_id);
    ASSERT_NE(app_table, nullptr);
    EXPECT_TRUE(has_import(*app_table, "./types"));

    // Verify type-only import.
    for (const auto& imp : app_table->imports) {
        if (imp.import_path == "./types") {
            EXPECT_TRUE(imp.is_type_only);
        }
    }
}

}  // namespace
}  // namespace lci::symbollinker
