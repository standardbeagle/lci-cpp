#include <gtest/gtest.h>

#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>
#include <lci/symbollinker/csharp_linker.h>
#include <lci/symbollinker/linker_engine.h>
#include <lci/symbollinker/python_linker.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

namespace lci::symbollinker {
namespace {

parser::UniqueTree parse(parser::Language lang, std::string_view src) {
    parser::UniqueParser p = parser::make_parser(lang);
    if (!p) return nullptr;
    TSTree* tree = ts_parser_parse_string(
        p.get(), nullptr, src.data(), static_cast<uint32_t>(src.size()));
    return parser::UniqueTree(tree);
}

bool has_symbol(const SymbolTable& t, std::string_view name) {
    return std::find(t.symbol_names.begin(), t.symbol_names.end(), name) !=
           t.symbol_names.end();
}

bool has_import(const SymbolTable& t, std::string_view path) {
    for (const auto& imp : t.imports) {
        if (imp.import_path == path) return true;
    }
    return false;
}

bool has_export(const SymbolTable& t, std::string_view name) {
    for (const auto& exp : t.exports) {
        if (exp.exported_name == name) return true;
    }
    return false;
}

// =========================================================================
// PythonExtractor tests
// =========================================================================

TEST(PythonExtractorTest, CanHandlePythonFiles) {
    PythonExtractor ext;
    EXPECT_TRUE(ext.can_handle("/project/main.py"));
    EXPECT_TRUE(ext.can_handle("util.pyw"));
    EXPECT_TRUE(ext.can_handle("stubs.pyi"));
    EXPECT_FALSE(ext.can_handle("/project/main.go"));
    EXPECT_FALSE(ext.can_handle("/project/main.js"));
}

TEST(PythonExtractorTest, Language) {
    PythonExtractor ext;
    EXPECT_EQ(ext.language(), parser::Language::Python);
}

TEST(PythonExtractorTest, NullTreeReturnsEmptyTable) {
    PythonExtractor ext;
    SymbolTable t = ext.extract_symbols(1, "x = 1", nullptr);
    EXPECT_EQ(t.file_id, 1u);
    EXPECT_TRUE(t.symbol_names.empty());
}

TEST(PythonExtractorTest, ExtractFunction) {
    PythonExtractor ext;
    std::string_view src = "def hello():\n    pass\n\ndef _private():\n    pass\n";
    auto tree = parse(parser::Language::Python, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "hello"));
    EXPECT_TRUE(has_symbol(t, "_private"));
    EXPECT_TRUE(has_export(t, "hello"));
    EXPECT_FALSE(has_export(t, "_private"));
}

TEST(PythonExtractorTest, ExtractAsyncFunction) {
    PythonExtractor ext;
    std::string_view src = "async def fetch():\n    pass\n";
    auto tree = parse(parser::Language::Python, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "fetch"));
    EXPECT_TRUE(has_export(t, "fetch"));
}

TEST(PythonExtractorTest, ExtractClass) {
    PythonExtractor ext;
    std::string_view src =
        "class Server:\n"
        "    def start(self):\n"
        "        pass\n"
        "    def _stop(self):\n"
        "        pass\n";
    auto tree = parse(parser::Language::Python, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Server"));
    EXPECT_TRUE(has_symbol(t, "Server.start"));
    EXPECT_TRUE(has_symbol(t, "Server._stop"));
    EXPECT_TRUE(has_export(t, "Server"));
    EXPECT_TRUE(has_export(t, "Server.start"));
    EXPECT_FALSE(has_export(t, "Server._stop"));
}

TEST(PythonExtractorTest, ExtractImportStatement) {
    PythonExtractor ext;
    std::string_view src = "import os\nimport json\nimport os.path\n";
    auto tree = parse(parser::Language::Python, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_import(t, "os"));
    EXPECT_TRUE(has_import(t, "json"));
    EXPECT_TRUE(has_import(t, "os.path"));
}

TEST(PythonExtractorTest, ExtractFromImport) {
    PythonExtractor ext;
    std::string_view src =
        "from os import path\n"
        "from collections import OrderedDict, defaultdict\n";
    auto tree = parse(parser::Language::Python, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_import(t, "os"));
    EXPECT_TRUE(has_import(t, "collections"));

    // Check imported names for collections.
    for (const auto& imp : t.imports) {
        if (imp.import_path == "collections") {
            EXPECT_GE(imp.imported_names.size(), 2u);
        }
    }
}

TEST(PythonExtractorTest, ExtractRelativeImport) {
    PythonExtractor ext;
    std::string_view src = "from . import utils\nfrom ..models import User\n";
    auto tree = parse(parser::Language::Python, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_FALSE(t.imports.empty());
}

TEST(PythonExtractorTest, ExtractAssignment) {
    PythonExtractor ext;
    std::string_view src = "VERSION = \"1.0\"\n_debug = False\n";
    auto tree = parse(parser::Language::Python, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "VERSION"));
    EXPECT_TRUE(has_symbol(t, "_debug"));
    EXPECT_TRUE(has_export(t, "VERSION"));
    EXPECT_FALSE(has_export(t, "_debug"));
}

// =========================================================================
// PythonResolver tests
// =========================================================================

TEST(PythonResolverTest, BuiltinModule) {
    PythonResolver resolver("/project");

    auto res = resolver.resolve_import("sys", 1);
    EXPECT_TRUE(res.is_builtin);

    res = resolver.resolve_import("os", 1);
    EXPECT_TRUE(res.is_builtin);
}

TEST(PythonResolverTest, StdlibModule) {
    PythonResolver resolver("/project");

    auto res = resolver.resolve_import("json", 1);
    EXPECT_TRUE(res.is_builtin);

    res = resolver.resolve_import("collections", 1);
    EXPECT_TRUE(res.is_builtin);

    res = resolver.resolve_import("pathlib", 1);
    EXPECT_TRUE(res.is_builtin);
}

TEST(PythonResolverTest, AbsoluteImportResolution) {
    PythonResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/mypackage/utils.py"] = 10;
    registry["/project/mypackage/__init__.py"] = 11;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("mypackage.utils", 1);
    EXPECT_EQ(res.file_id, 10u);
    EXPECT_EQ(res.resolved_path, "/project/mypackage/utils.py");
}

TEST(PythonResolverTest, PackageInitResolution) {
    PythonResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/mypackage/__init__.py"] = 11;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("mypackage", 1);
    EXPECT_EQ(res.file_id, 11u);
}

TEST(PythonResolverTest, RelativeImportResolution) {
    PythonResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/pkg/main.py"] = 1;
    registry["/project/pkg/utils.py"] = 2;
    registry["/project/pkg/__init__.py"] = 3;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import(".utils", 1);
    EXPECT_EQ(res.file_id, 2u);
    EXPECT_EQ(res.resolved_path, "/project/pkg/utils.py");
}

TEST(PythonResolverTest, ExternalPackage) {
    PythonResolver resolver("/project");

    auto res = resolver.resolve_import("numpy", 1);
    EXPECT_TRUE(res.is_external);
}

// =========================================================================
// PythonExtractor + Resolver integration
// =========================================================================

TEST(PythonIntegrationTest, MultiFileSymbolResolution) {
    PythonExtractor ext;
    PythonResolver resolver("/project");

    // Set up file registry.
    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/app/main.py"] = 1;
    registry["/project/app/utils.py"] = 2;
    resolver.set_file_registry(registry);

    // Parse utils.py
    std::string_view utils_src =
        "def helper():\n    pass\n\nclass Config:\n    pass\n";
    auto utils_tree = parse(parser::Language::Python, utils_src);
    ASSERT_NE(utils_tree, nullptr);
    SymbolTable utils_table =
        ext.extract_symbols(2, utils_src, utils_tree.get());
    EXPECT_TRUE(has_symbol(utils_table, "helper"));
    EXPECT_TRUE(has_symbol(utils_table, "Config"));

    // Parse main.py
    std::string_view main_src =
        "from app.utils import helper\n\ndef run():\n    helper()\n";
    auto main_tree = parse(parser::Language::Python, main_src);
    ASSERT_NE(main_tree, nullptr);
    SymbolTable main_table =
        ext.extract_symbols(1, main_src, main_tree.get());
    EXPECT_TRUE(has_import(main_table, "app.utils"));

    // Resolve the import.
    auto res = resolver.resolve_import("app.utils", 1);
    EXPECT_EQ(res.file_id, 2u);
}

// =========================================================================
// CSharpExtractor tests
// =========================================================================

TEST(CSharpExtractorTest, CanHandleCSharpFiles) {
    CSharpExtractor ext;
    EXPECT_TRUE(ext.can_handle("/project/Program.cs"));
    EXPECT_TRUE(ext.can_handle("script.csx"));
    EXPECT_FALSE(ext.can_handle("/project/main.py"));
    EXPECT_FALSE(ext.can_handle("/project/main.go"));
}

TEST(CSharpExtractorTest, Language) {
    CSharpExtractor ext;
    EXPECT_EQ(ext.language(), parser::Language::CSharp);
}

TEST(CSharpExtractorTest, NullTreeReturnsEmptyTable) {
    CSharpExtractor ext;
    SymbolTable t = ext.extract_symbols(1, "class A {}", nullptr);
    EXPECT_EQ(t.file_id, 1u);
    EXPECT_TRUE(t.symbol_names.empty());
}

TEST(CSharpExtractorTest, ExtractClass) {
    CSharpExtractor ext;
    std::string_view src =
        "namespace MyApp {\n"
        "    public class Server {\n"
        "        public void Start() {}\n"
        "        private void Stop() {}\n"
        "    }\n"
        "}\n";
    auto tree = parse(parser::Language::CSharp, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "MyApp"));
    EXPECT_TRUE(has_symbol(t, "Server"));
    EXPECT_TRUE(has_symbol(t, "Server.Start"));
    EXPECT_TRUE(has_symbol(t, "Server.Stop"));
    EXPECT_TRUE(has_export(t, "Server"));
    EXPECT_TRUE(has_export(t, "Server.Start"));
    EXPECT_FALSE(has_export(t, "Server.Stop"));
}

TEST(CSharpExtractorTest, ExtractInterface) {
    CSharpExtractor ext;
    std::string_view src =
        "public interface IRepository {\n"
        "    void Save();\n"
        "}\n";
    auto tree = parse(parser::Language::CSharp, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "IRepository"));
    EXPECT_TRUE(has_export(t, "IRepository"));
}

TEST(CSharpExtractorTest, ExtractEnum) {
    CSharpExtractor ext;
    std::string_view src =
        "public enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue\n"
        "}\n";
    auto tree = parse(parser::Language::CSharp, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Color"));
    EXPECT_TRUE(has_symbol(t, "Color.Red"));
    EXPECT_TRUE(has_symbol(t, "Color.Green"));
    EXPECT_TRUE(has_symbol(t, "Color.Blue"));
    EXPECT_TRUE(has_export(t, "Color"));
}

TEST(CSharpExtractorTest, ExtractUsingDirectives) {
    CSharpExtractor ext;
    std::string_view src =
        "using System;\n"
        "using System.Collections.Generic;\n"
        "using static System.Math;\n"
        "namespace MyApp { class App {} }\n";
    auto tree = parse(parser::Language::CSharp, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_import(t, "System"));
    EXPECT_TRUE(has_import(t, "System.Collections.Generic"));
    EXPECT_TRUE(has_import(t, "System.Math"));

    // Check that static using has is_type_only set.
    for (const auto& imp : t.imports) {
        if (imp.import_path == "System.Math") {
            EXPECT_TRUE(imp.is_type_only);
        }
    }
}

TEST(CSharpExtractorTest, ExtractProperty) {
    CSharpExtractor ext;
    std::string_view src =
        "public class Config {\n"
        "    public string Host { get; set; }\n"
        "    private int _port;\n"
        "}\n";
    auto tree = parse(parser::Language::CSharp, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Config"));
    EXPECT_TRUE(has_symbol(t, "Config.Host"));
}

TEST(CSharpExtractorTest, ExtractConstructor) {
    CSharpExtractor ext;
    std::string_view src =
        "public class Service {\n"
        "    public Service(int id) {}\n"
        "}\n";
    auto tree = parse(parser::Language::CSharp, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Service"));
    EXPECT_TRUE(has_symbol(t, "Service.Service"));
}

// =========================================================================
// CSharpResolver tests
// =========================================================================

TEST(CSharpResolverTest, BuiltinNamespace) {
    CSharpResolver resolver("/project");

    auto res = resolver.resolve_import("System", 1);
    EXPECT_TRUE(res.is_builtin);

    res = resolver.resolve_import("System.Collections.Generic", 1);
    EXPECT_TRUE(res.is_builtin);

    res = resolver.resolve_import("Microsoft.Extensions.DependencyInjection", 1);
    EXPECT_TRUE(res.is_builtin);
}

TEST(CSharpResolverTest, ProjectNamespaceResolution) {
    CSharpResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/MyApp/Models/User.cs"] = 10;
    registry["/project/MyApp/Services/AuthService.cs"] = 11;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("MyApp.Models", 1);
    EXPECT_EQ(res.file_id, 10u);
}

TEST(CSharpResolverTest, KnownExternalNamespace) {
    CSharpResolver resolver("/project");

    auto res = resolver.resolve_import("Newtonsoft.Json", 1);
    EXPECT_TRUE(res.is_external);

    res = resolver.resolve_import("NUnit.Framework", 1);
    EXPECT_TRUE(res.is_external);
}

TEST(CSharpResolverTest, UnknownNamespace) {
    CSharpResolver resolver("/project");

    auto res = resolver.resolve_import("SomeUnknown.Namespace", 1);
    EXPECT_TRUE(res.is_external);
}

// =========================================================================
// CSharpExtractor + Resolver integration
// =========================================================================

TEST(CSharpIntegrationTest, MultiFileNamespaceResolution) {
    CSharpExtractor ext;
    CSharpResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/MyApp/Models/User.cs"] = 1;
    registry["/project/MyApp/Services/UserService.cs"] = 2;
    resolver.set_file_registry(registry);

    // Parse User.cs
    std::string_view user_src =
        "namespace MyApp.Models {\n"
        "    public class User {\n"
        "        public string Name { get; set; }\n"
        "    }\n"
        "}\n";
    auto user_tree = parse(parser::Language::CSharp, user_src);
    ASSERT_NE(user_tree, nullptr);
    SymbolTable user_table =
        ext.extract_symbols(1, user_src, user_tree.get());
    EXPECT_TRUE(has_symbol(user_table, "User"));
    EXPECT_TRUE(has_symbol(user_table, "MyApp.Models"));

    // Parse UserService.cs
    std::string_view svc_src =
        "using MyApp.Models;\n"
        "namespace MyApp.Services {\n"
        "    public class UserService {\n"
        "        public User GetUser() { return null; }\n"
        "    }\n"
        "}\n";
    auto svc_tree = parse(parser::Language::CSharp, svc_src);
    ASSERT_NE(svc_tree, nullptr);
    SymbolTable svc_table =
        ext.extract_symbols(2, svc_src, svc_tree.get());
    EXPECT_TRUE(has_import(svc_table, "MyApp.Models"));
    EXPECT_TRUE(has_symbol(svc_table, "UserService"));

    // Resolve the using directive.
    auto res = resolver.resolve_import("MyApp.Models", 2);
    EXPECT_EQ(res.file_id, 1u);
}

// =========================================================================
// LinkerEngine integration with Python and C# extractors
// =========================================================================

TEST(LinkerEngineTest, PythonExtractorRegistration) {
    LinkerEngine engine("/project");
    engine.register_extractor(std::make_unique<PythonExtractor>());
    engine.register_resolver(std::make_unique<PythonResolver>("/project"));

    EXPECT_GE(engine.stats().extractors, 1);
    EXPECT_GE(engine.stats().resolvers, 1);
}

TEST(LinkerEngineTest, CSharpExtractorRegistration) {
    LinkerEngine engine("/project");
    engine.register_extractor(std::make_unique<CSharpExtractor>());
    engine.register_resolver(std::make_unique<CSharpResolver>("/project"));

    EXPECT_GE(engine.stats().extractors, 1);
    EXPECT_GE(engine.stats().resolvers, 1);
}

}  // namespace
}  // namespace lci::symbollinker
