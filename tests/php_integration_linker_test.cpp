#include <gtest/gtest.h>

#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>
#include <lci/symbollinker/csharp_linker.h>
#include <lci/symbollinker/go_linker.h>
#include <lci/symbollinker/js_linker.h>
#include <lci/symbollinker/linker_engine.h>
#include <lci/symbollinker/php_linker.h>
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
// PhpExtractor tests
// =========================================================================

TEST(PhpExtractorTest, CanHandlePhpFiles) {
    PhpExtractor ext;
    EXPECT_TRUE(ext.can_handle("/project/index.php"));
    EXPECT_TRUE(ext.can_handle("template.phtml"));
    EXPECT_TRUE(ext.can_handle("archive.phar"));
    EXPECT_FALSE(ext.can_handle("/project/main.py"));
    EXPECT_FALSE(ext.can_handle("/project/main.js"));
}

TEST(PhpExtractorTest, Language) {
    PhpExtractor ext;
    EXPECT_EQ(ext.language(), parser::Language::PHP);
}

TEST(PhpExtractorTest, NullTreeReturnsEmptyTable) {
    PhpExtractor ext;
    SymbolTable t = ext.extract_symbols(1, "<?php echo 1;", nullptr);
    EXPECT_EQ(t.file_id, 1u);
    EXPECT_TRUE(t.symbol_names.empty());
}

TEST(PhpExtractorTest, ExtractFunction) {
    PhpExtractor ext;
    std::string_view src =
        "<?php\n"
        "function greet($name) { return $name; }\n"
        "function helper() {}\n";
    auto tree = parse(parser::Language::PHP, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "greet"));
    EXPECT_TRUE(has_symbol(t, "helper"));
    EXPECT_TRUE(has_export(t, "greet"));
    EXPECT_TRUE(has_export(t, "helper"));
}

TEST(PhpExtractorTest, ExtractClass) {
    PhpExtractor ext;
    std::string_view src =
        "<?php\n"
        "class UserService {\n"
        "    public $name;\n"
        "    private $email;\n"
        "    public function getName() { return $this->name; }\n"
        "    private function validate() {}\n"
        "}\n";
    auto tree = parse(parser::Language::PHP, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "UserService"));
    EXPECT_TRUE(has_export(t, "UserService"));
    EXPECT_TRUE(has_symbol(t, "UserService.getName"));
    EXPECT_TRUE(has_export(t, "UserService.getName"));
    EXPECT_TRUE(has_symbol(t, "UserService.validate"));
    EXPECT_FALSE(has_export(t, "UserService.validate"));
    EXPECT_TRUE(has_symbol(t, "UserService.name"));
    EXPECT_TRUE(has_export(t, "UserService.name"));
    EXPECT_TRUE(has_symbol(t, "UserService.email"));
    EXPECT_FALSE(has_export(t, "UserService.email"));
}

TEST(PhpExtractorTest, ExtractInterface) {
    PhpExtractor ext;
    std::string_view src =
        "<?php\n"
        "interface Renderable {\n"
        "    public function render(): string;\n"
        "}\n";
    auto tree = parse(parser::Language::PHP, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Renderable"));
    EXPECT_TRUE(has_export(t, "Renderable"));
    EXPECT_TRUE(has_symbol(t, "Renderable.render"));
}

TEST(PhpExtractorTest, ExtractTrait) {
    PhpExtractor ext;
    std::string_view src =
        "<?php\n"
        "trait Timestamps {\n"
        "    public function getCreatedAt() { return $this->created_at; }\n"
        "}\n";
    auto tree = parse(parser::Language::PHP, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Timestamps"));
    EXPECT_TRUE(has_export(t, "Timestamps"));
    EXPECT_TRUE(has_symbol(t, "Timestamps.getCreatedAt"));
}

TEST(PhpExtractorTest, ExtractUseStatements) {
    PhpExtractor ext;
    std::string_view src =
        "<?php\n"
        "use App\\Models\\User;\n"
        "use App\\Services\\Auth as AuthService;\n";
    auto tree = parse(parser::Language::PHP, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_import(t, "App\\Models\\User"));
    EXPECT_TRUE(has_import(t, "App\\Services\\Auth"));

    for (const auto& imp : t.imports) {
        if (imp.import_path == "App\\Services\\Auth") {
            EXPECT_EQ(imp.alias, "AuthService");
        }
        if (imp.import_path == "App\\Models\\User") {
            EXPECT_EQ(imp.alias, "User");
        }
    }
}

TEST(PhpExtractorTest, ExtractNamespace) {
    PhpExtractor ext;
    std::string_view src =
        "<?php\n"
        "namespace App\\Controllers;\n"
        "class HomeController {}\n";
    auto tree = parse(parser::Language::PHP, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "HomeController"));
    EXPECT_TRUE(has_export(t, "HomeController"));
}

TEST(PhpExtractorTest, ExtractConstants) {
    PhpExtractor ext;
    std::string_view src =
        "<?php\n"
        "class Config {\n"
        "    const VERSION = '1.0';\n"
        "    public const APP_NAME = 'MyApp';\n"
        "}\n";
    auto tree = parse(parser::Language::PHP, src);
    ASSERT_NE(tree, nullptr);

    SymbolTable t = ext.extract_symbols(1, src, tree.get());

    EXPECT_TRUE(has_symbol(t, "Config"));
    EXPECT_TRUE(has_symbol(t, "Config.VERSION"));
    EXPECT_TRUE(has_symbol(t, "Config.APP_NAME"));
}

// =========================================================================
// PhpResolver tests
// =========================================================================

TEST(PhpResolverTest, BuiltinClass) {
    PhpResolver resolver("/project");

    auto res = resolver.resolve_import("Exception", 1);
    EXPECT_TRUE(res.is_builtin);
    EXPECT_FALSE(res.is_external);

    res = resolver.resolve_import("stdClass", 1);
    EXPECT_TRUE(res.is_builtin);
}

TEST(PhpResolverTest, PSR4Resolution) {
    PhpResolver resolver("/project");
    resolver.add_psr4_mapping("App\\", "/project/src/");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/src/Models/User.php"] = 10;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("App\\Models\\User", 1);
    EXPECT_EQ(res.file_id, 10u);
    EXPECT_FALSE(res.is_external);
}

TEST(PhpResolverTest, FileIncludeResolution) {
    PhpResolver resolver("/project");

    absl::flat_hash_map<std::string, FileID> registry;
    registry["/project/src/config.php"] = 10;
    registry["/project/src/app.php"] = 20;
    resolver.set_file_registry(registry);

    auto res = resolver.resolve_import("./config.php", 20);
    EXPECT_EQ(res.file_id, 10u);
}

TEST(PhpResolverTest, UnknownNamespaceIsExternal) {
    PhpResolver resolver("/project");
    resolver.set_file_registry({});

    auto res = resolver.resolve_import("Vendor\\Package\\Service", 1);
    EXPECT_TRUE(res.is_external);
}

TEST(PhpResolverTest, Language) {
    PhpResolver resolver("/project");
    EXPECT_EQ(resolver.language(), parser::Language::PHP);
}

// =========================================================================
// PHP LinkerEngine integration
// =========================================================================

TEST(PhpLinkerIntegrationTest, CrossFileSymbolResolution) {
    LinkerEngine engine("/project");

    auto ext = std::make_unique<PhpExtractor>();
    auto resolver = std::make_unique<PhpResolver>("/project");
    resolver->add_psr4_mapping("App\\", "/project/src/");

    absl::flat_hash_map<std::string, FileID> file_reg;

    FileID model_id =
        engine.get_or_create_file_id("/project/src/Models/User.php");
    FileID ctrl_id =
        engine.get_or_create_file_id("/project/src/Controllers/UserController.php");
    file_reg["/project/src/Models/User.php"] = model_id;
    file_reg["/project/src/Controllers/UserController.php"] = ctrl_id;
    resolver->set_file_registry(file_reg);

    engine.register_extractor(std::move(ext));
    engine.register_resolver(std::move(resolver));

    std::string_view model_src =
        "<?php\n"
        "namespace App\\Models;\n"
        "class User {\n"
        "    public function getName(): string { return $this->name; }\n"
        "}\n";
    std::string_view ctrl_src =
        "<?php\n"
        "namespace App\\Controllers;\n"
        "use App\\Models\\User;\n"
        "class UserController {\n"
        "    public function show() { $user = new User(); }\n"
        "}\n";

    EXPECT_TRUE(engine.index_file("/project/src/Models/User.php", model_src));
    EXPECT_TRUE(engine.index_file(
        "/project/src/Controllers/UserController.php", ctrl_src));
    EXPECT_TRUE(engine.link_symbols());

    const SymbolTable* model_table = engine.get_symbol_table(model_id);
    ASSERT_NE(model_table, nullptr);
    EXPECT_TRUE(has_symbol(*model_table, "User"));
    EXPECT_TRUE(has_export(*model_table, "User"));

    const SymbolTable* ctrl_table = engine.get_symbol_table(ctrl_id);
    ASSERT_NE(ctrl_table, nullptr);
    EXPECT_TRUE(has_import(*ctrl_table, "App\\Models\\User"));

    auto deps = engine.get_file_dependencies(ctrl_id);
    EXPECT_EQ(deps.size(), 1u);
    if (!deps.empty()) {
        EXPECT_EQ(deps[0], model_id);
    }
}

// =========================================================================
// All 5 language linkers integration
// =========================================================================

TEST(AllLinkerIntegrationTest, AllFiveLanguagesRegistered) {
    LinkerEngine engine("/project");

    engine.register_extractor(std::make_unique<GoExtractor>());
    engine.register_extractor(std::make_unique<JSExtractor>(false));
    engine.register_extractor(std::make_unique<JSExtractor>(true));
    engine.register_extractor(std::make_unique<PythonExtractor>());
    engine.register_extractor(std::make_unique<CSharpExtractor>());
    engine.register_extractor(std::make_unique<PhpExtractor>());

    engine.register_resolver(std::make_unique<GoResolver>("/project"));
    engine.register_resolver(std::make_unique<JSResolver>("/project"));
    engine.register_resolver(std::make_unique<TSResolver>("/project"));
    engine.register_resolver(std::make_unique<PythonResolver>("/project"));
    engine.register_resolver(std::make_unique<CSharpResolver>("/project"));
    engine.register_resolver(std::make_unique<PhpResolver>("/project"));

    auto stats = engine.stats();
    EXPECT_GE(stats.extractors, 6);
    EXPECT_GE(stats.resolvers, 6);
}

TEST(AllLinkerIntegrationTest, GoMultiFileProject) {
    LinkerEngine engine("/project");

    auto ext = std::make_unique<GoExtractor>();
    auto resolver = std::make_unique<GoResolver>("/project");
    resolver->set_module_name("github.com/myorg/app");

    absl::flat_hash_map<std::string, FileID> file_reg;
    FileID svc_id = engine.get_or_create_file_id("/project/svc.go");
    FileID main_id = engine.get_or_create_file_id("/project/main.go");
    file_reg["/project/svc.go"] = svc_id;
    file_reg["/project/main.go"] = main_id;
    resolver->set_file_registry(file_reg);

    engine.register_extractor(std::move(ext));
    engine.register_resolver(std::move(resolver));

    EXPECT_TRUE(engine.index_file(
        "/project/svc.go",
        "package svc\n\nfunc Serve() {}\n"));
    EXPECT_TRUE(engine.index_file(
        "/project/main.go",
        "package main\n\nimport \"github.com/myorg/app/svc\"\n\n"
        "func main() { svc.Serve() }\n"));
    EXPECT_TRUE(engine.link_symbols());

    const SymbolTable* svc = engine.get_symbol_table(svc_id);
    ASSERT_NE(svc, nullptr);
    EXPECT_TRUE(has_export(*svc, "Serve"));
}

TEST(AllLinkerIntegrationTest, JSMultiFileProject) {
    LinkerEngine engine("/project");

    auto ext = std::make_unique<JSExtractor>(false);
    auto resolver = std::make_unique<JSResolver>("/project");

    absl::flat_hash_map<std::string, FileID> file_reg;
    FileID lib_id = engine.get_or_create_file_id("/project/lib.js");
    FileID app_id = engine.get_or_create_file_id("/project/app.js");
    file_reg["/project/lib.js"] = lib_id;
    file_reg["/project/app.js"] = app_id;
    resolver->set_file_registry(file_reg);

    engine.register_extractor(std::move(ext));
    engine.register_resolver(std::move(resolver));

    EXPECT_TRUE(engine.index_file(
        "/project/lib.js",
        "export function compute(x) { return x * 2; }\n"));
    EXPECT_TRUE(engine.index_file(
        "/project/app.js",
        "import { compute } from './lib';\nconsole.log(compute(5));\n"));
    EXPECT_TRUE(engine.link_symbols());

    const SymbolTable* lib = engine.get_symbol_table(lib_id);
    ASSERT_NE(lib, nullptr);
    EXPECT_TRUE(has_export(*lib, "compute"));

    auto deps = engine.get_file_dependencies(app_id);
    EXPECT_EQ(deps.size(), 1u);
}

TEST(AllLinkerIntegrationTest, PythonMultiFileProject) {
    LinkerEngine engine("/project");

    auto ext = std::make_unique<PythonExtractor>();
    auto resolver = std::make_unique<PythonResolver>("/project");

    absl::flat_hash_map<std::string, FileID> file_reg;
    FileID util_id = engine.get_or_create_file_id("/project/utils.py");
    FileID main_id = engine.get_or_create_file_id("/project/main.py");
    file_reg["/project/utils.py"] = util_id;
    file_reg["/project/main.py"] = main_id;
    resolver->set_file_registry(file_reg);

    engine.register_extractor(std::move(ext));
    engine.register_resolver(std::move(resolver));

    EXPECT_TRUE(engine.index_file(
        "/project/utils.py",
        "def calculate(x):\n    return x + 1\n"));
    EXPECT_TRUE(engine.index_file(
        "/project/main.py",
        "from utils import calculate\n\nresult = calculate(5)\n"));
    EXPECT_TRUE(engine.link_symbols());

    const SymbolTable* utils = engine.get_symbol_table(util_id);
    ASSERT_NE(utils, nullptr);
    EXPECT_TRUE(has_export(*utils, "calculate"));
}

TEST(AllLinkerIntegrationTest, CSharpMultiFileProject) {
    LinkerEngine engine("/project");

    auto ext = std::make_unique<CSharpExtractor>();
    auto resolver = std::make_unique<CSharpResolver>("/project");

    absl::flat_hash_map<std::string, FileID> file_reg;
    FileID model_id = engine.get_or_create_file_id("/project/Models/User.cs");
    FileID ctrl_id =
        engine.get_or_create_file_id("/project/Controllers/UserCtrl.cs");
    file_reg["/project/Models/User.cs"] = model_id;
    file_reg["/project/Controllers/UserCtrl.cs"] = ctrl_id;
    resolver->set_file_registry(file_reg);

    engine.register_extractor(std::move(ext));
    engine.register_resolver(std::move(resolver));

    EXPECT_TRUE(engine.index_file(
        "/project/Models/User.cs",
        "namespace Models {\n"
        "    public class User {\n"
        "        public string Name { get; set; }\n"
        "    }\n"
        "}\n"));
    EXPECT_TRUE(engine.index_file(
        "/project/Controllers/UserCtrl.cs",
        "using Models;\n"
        "namespace Controllers {\n"
        "    public class UserCtrl {\n"
        "        public void Get() {}\n"
        "    }\n"
        "}\n"));
    EXPECT_TRUE(engine.link_symbols());

    const SymbolTable* model = engine.get_symbol_table(model_id);
    ASSERT_NE(model, nullptr);
    EXPECT_TRUE(has_symbol(*model, "User"));
}

TEST(AllLinkerIntegrationTest, PhpMultiFileProject) {
    LinkerEngine engine("/project");

    auto ext = std::make_unique<PhpExtractor>();
    auto resolver = std::make_unique<PhpResolver>("/project");
    resolver->add_psr4_mapping("App\\", "/project/src/");

    absl::flat_hash_map<std::string, FileID> file_reg;
    FileID repo_id =
        engine.get_or_create_file_id("/project/src/Repos/UserRepo.php");
    FileID svc_id =
        engine.get_or_create_file_id("/project/src/Services/UserService.php");
    file_reg["/project/src/Repos/UserRepo.php"] = repo_id;
    file_reg["/project/src/Services/UserService.php"] = svc_id;
    resolver->set_file_registry(file_reg);

    engine.register_extractor(std::move(ext));
    engine.register_resolver(std::move(resolver));

    EXPECT_TRUE(engine.index_file(
        "/project/src/Repos/UserRepo.php",
        "<?php\n"
        "namespace App\\Repos;\n"
        "class UserRepo {\n"
        "    public function findById($id) { return null; }\n"
        "}\n"));
    EXPECT_TRUE(engine.index_file(
        "/project/src/Services/UserService.php",
        "<?php\n"
        "namespace App\\Services;\n"
        "use App\\Repos\\UserRepo;\n"
        "class UserService {\n"
        "    public function getUser($id) {}\n"
        "}\n"));
    EXPECT_TRUE(engine.link_symbols());

    const SymbolTable* repo = engine.get_symbol_table(repo_id);
    ASSERT_NE(repo, nullptr);
    EXPECT_TRUE(has_symbol(*repo, "UserRepo"));
    EXPECT_TRUE(has_export(*repo, "UserRepo"));

    const SymbolTable* svc = engine.get_symbol_table(svc_id);
    ASSERT_NE(svc, nullptr);
    EXPECT_TRUE(has_import(*svc, "App\\Repos\\UserRepo"));

    auto deps = engine.get_file_dependencies(svc_id);
    EXPECT_EQ(deps.size(), 1u);
    if (!deps.empty()) {
        EXPECT_EQ(deps[0], repo_id);
    }
}

TEST(AllLinkerIntegrationTest, MixedLanguageEngine) {
    LinkerEngine engine("/project");

    // Register all extractors.
    engine.register_extractor(std::make_unique<GoExtractor>());
    engine.register_extractor(std::make_unique<JSExtractor>(false));
    engine.register_extractor(std::make_unique<PythonExtractor>());
    engine.register_extractor(std::make_unique<CSharpExtractor>());
    engine.register_extractor(std::make_unique<PhpExtractor>());

    // Register all resolvers.
    engine.register_resolver(std::make_unique<GoResolver>("/project"));
    engine.register_resolver(std::make_unique<JSResolver>("/project"));
    engine.register_resolver(std::make_unique<PythonResolver>("/project"));
    engine.register_resolver(std::make_unique<CSharpResolver>("/project"));
    engine.register_resolver(std::make_unique<PhpResolver>("/project"));

    // Index one file per language.
    EXPECT_TRUE(engine.index_file(
        "/project/main.go",
        "package main\n\nfunc Run() {}\n"));
    EXPECT_TRUE(engine.index_file(
        "/project/app.js",
        "export function start() {}\n"));
    EXPECT_TRUE(engine.index_file(
        "/project/util.py",
        "def process():\n    return True\n"));
    EXPECT_TRUE(engine.index_file(
        "/project/Program.cs",
        "namespace App {\n    public class Program {\n"
        "        public static void Main() {}\n    }\n}\n"));
    EXPECT_TRUE(engine.index_file(
        "/project/index.php",
        "<?php\nfunction bootstrap() {}\n"));

    EXPECT_TRUE(engine.link_symbols());

    auto stats = engine.stats();
    EXPECT_EQ(stats.files, 5);
    EXPECT_GE(stats.symbol_links, 5);
}

}  // namespace
}  // namespace lci::symbollinker
