#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <gtest/gtest.h>
#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lci::parser {
namespace {

struct ExpectedSymbol {
    std::string_view name;
    SymbolType type;
};

struct ExpectedReference {
    std::string_view name;
    ReferenceType type;
    int count{1};
};

bool tree_has_error(TSNode node) {
    if (ts_node_is_error(node) || ts_node_is_missing(node)) return true;
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        if (tree_has_error(ts_node_child(node, i))) return true;
    }
    return false;
}

ExtractionResults extract_spec_fixture(
    Language language, std::string_view extension, std::string_view path,
    std::string_view source) {
    auto parser = make_parser(language);
    EXPECT_TRUE(parser) << path;
    if (!parser) return {};

    UniqueTree tree(ts_parser_parse_string(
        parser.get(), nullptr, source.data(),
        static_cast<uint32_t>(source.size())));
    EXPECT_TRUE(tree) << path;
    if (!tree) return {};
    EXPECT_FALSE(tree_has_error(ts_tree_root_node(tree.get())))
        << path << " fixture no longer conforms to the pinned grammar";

    UnifiedExtractor extractor;
    extractor.init(source, 1, extension, path);
    extractor.extract(tree.get());
    return extractor.take_results();
}

void expect_graph(
    const ExtractionResults& graph,
    std::initializer_list<ExpectedSymbol> symbols,
    std::initializer_list<ExpectedReference> references,
    std::initializer_list<std::string_view> imports = {}) {
    std::ostringstream summary;
    summary << "symbols:";
    for (const auto& symbol : graph.symbols)
        summary << ' ' << symbol.name << ':' << to_string(symbol.type);
    summary << "\nrefs:";
    for (const auto& reference : graph.references)
        summary << ' ' << reference.referenced_name << ':'
                << static_cast<int>(reference.type);
    summary << "\nimports:";
    for (const auto& import : graph.imports) summary << ' ' << import.path;
    const std::string graph_summary = summary.str();

    std::vector<std::string> actual_symbols;
    actual_symbols.reserve(graph.symbols.size());
    for (const auto& actual : graph.symbols) {
        actual_symbols.push_back(actual.name + ":" +
                                 std::string(to_string(actual.type)));
    }
    std::vector<std::string> expected_symbols;
    expected_symbols.reserve(symbols.size());
    for (const auto& expected : symbols) {
        expected_symbols.push_back(std::string(expected.name) + ":" +
                                   std::string(to_string(expected.type)));
    }
    std::sort(actual_symbols.begin(), actual_symbols.end());
    std::sort(expected_symbols.begin(), expected_symbols.end());
    EXPECT_EQ(actual_symbols, expected_symbols) << graph_summary;

    for (const auto& expected : references) {
        int matches = 0;
        for (const auto& actual : graph.references) {
            if (actual.referenced_name == expected.name &&
                actual.type == expected.type) {
                ++matches;
            }
        }
        EXPECT_EQ(matches, expected.count)
            << expected.name << " (reference type "
            << static_cast<int>(expected.type) << ")\n"
            << graph_summary;
    }

    std::vector<std::string> actual_imports;
    actual_imports.reserve(graph.imports.size());
    for (const auto& actual : graph.imports) {
        actual_imports.push_back(actual.path);
    }
    std::vector<std::string> expected_imports;
    expected_imports.reserve(imports.size());
    for (const auto expected : imports) {
        expected_imports.emplace_back(expected);
    }
    std::sort(actual_imports.begin(), actual_imports.end());
    std::sort(expected_imports.begin(), expected_imports.end());
    EXPECT_EQ(actual_imports, expected_imports) << graph_summary;
}

struct ExtensionFixture {
    std::string_view extension;
    std::string_view source;
    std::string_view symbol;
    SymbolType symbol_type;
};

struct CommonCallableFixture {
    Language language;
    std::string_view extension;
    std::string_view source;
    std::string_view callable;
    SymbolType symbol_type;
};

// Functions are the common unit consumed by callers, complexity, context,
// and side-effect analysis. Keep this matrix deliberately uniform: each
// fixture uses the language's ordinary two-parameter callable syntax, one
// branch, and one call. Besides recognizing the declaration, extraction must
// populate every graph channel downstream features rely on.
TEST(LanguageConstructConformance, CommonCallablesPopulateEveryGraphChannel) {
    constexpr std::array fixtures{
        CommonCallableFixture{Language::Go, ".go",
            "package fixture\nfunc helper(v int) int { return v }\nfunc choose(a, b int) int { if a > b { return helper(a) }; return b }\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::Python, ".py",
            "def helper(v):\n    return v\n\ndef choose(a, b):\n    if a > b:\n        return helper(a)\n    return b\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::JavaScript, ".js",
            "function helper(v) { return v; }\nfunction choose(a, b) { if (a > b) return helper(a); return b; }\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::TypeScript, ".ts",
            "function helper(v: number): number { return v; }\nfunction choose(a: number, b: number): number { if (a > b) return helper(a); return b; }\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::Rust, ".rs",
            "fn helper(v: i32) -> i32 { v }\nfn choose(a: i32, b: i32) -> i32 { if a > b { helper(a) } else { b } }\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::C, ".c",
            "int helper(int v) { return v; }\nint choose(int a, int b) { if (a > b) return helper(a); return b; }\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::Cpp, ".cpp",
            "int helper(int v) { return v; }\nint choose(int a, int b) { if (a > b) return helper(a); return b; }\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::Java, ".java",
            "class Fixture { int helper(int v) { return v; } int choose(int a, int b) { if (a > b) return helper(a); return b; } }\n",
            "choose", SymbolType::Method},
        CommonCallableFixture{Language::CSharp, ".cs",
            "class Fixture { int Helper(int v) { return v; } int Choose(int a, int b) { if (a > b) return Helper(a); return b; } }\n",
            "Choose", SymbolType::Method},
        CommonCallableFixture{Language::PHP, ".php",
            "<?php function helper(int $v): int { return $v; } function choose(int $a, int $b): int { if ($a > $b) return helper($a); return $b; }\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::Kotlin, ".kt",
            "fun helper(v: Int): Int = v\nfun choose(a: Int, b: Int): Int { if (a > b) return helper(a); return b }\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::Zig, ".zig",
            "fn helper(v: i32) i32 { return v; }\nfn choose(a: i32, b: i32) i32 { if (a > b) return helper(a); return b; }\n",
            "choose", SymbolType::Function},
        CommonCallableFixture{Language::Ruby, ".rb",
            "def helper(v)\n  v\nend\ndef choose(a, b)\n  return helper(a) if a > b\n  b\nend\n",
            "choose", SymbolType::Method},
    };

    for (const auto& fixture : fixtures) {
        SCOPED_TRACE(fixture.extension);
        const std::string path = "common" + std::string(fixture.extension);
        auto graph = extract_spec_fixture(fixture.language, fixture.extension,
                                          path, fixture.source);

        const auto symbol = std::find_if(
            graph.symbols.begin(), graph.symbols.end(), [&](const Symbol& item) {
                return item.name == fixture.callable &&
                       item.type == fixture.symbol_type;
            });
        ASSERT_NE(symbol, graph.symbols.end());
        EXPECT_EQ(symbol->parameter_count, 2);

        EXPECT_TRUE(std::any_of(
            graph.blocks.begin(), graph.blocks.end(), [&](const BlockBoundary& block) {
                return block.name == fixture.callable;
            })) << "callable body is missing from the block graph";
        EXPECT_TRUE(std::any_of(
            graph.scopes.begin(), graph.scopes.end(), [&](const ScopeInfo& scope) {
                return scope.name == fixture.callable &&
                       (scope.type == ScopeType::Function ||
                        scope.type == ScopeType::Method);
            })) << "callable is missing from the scope graph";

        const PositionKey declaration_key{symbol->line - 1,
                                           symbol->column - 1};
        const auto declaration = graph.declarations.find(declaration_key);
        ASSERT_NE(declaration, graph.declarations.end());
        EXPECT_FALSE(declaration->second.signature.empty());

        EXPECT_TRUE(std::any_of(
            graph.complexity.begin(), graph.complexity.end(),
            [&](const auto& item) {
                return item.first.line == symbol->line &&
                       item.first.column == symbol->column && item.second >= 2;
            })) << "branching callable is missing complexity metadata";
        EXPECT_TRUE(std::any_of(
            graph.references.begin(), graph.references.end(),
            [](const Reference& reference) {
                return reference.type == ReferenceType::Call;
            })) << "nested call is missing from the reference graph";
    }
}

// Every extension backed by a linked grammar must reach extraction, not just
// agree with language_map.h. This catches routing regressions where a file is
// classified as code but silently produces no graph records.
TEST(LanguageConstructConformance, EveryLinkedExtensionExtractsGraphRecords) {
    constexpr std::string_view go = "package fixture\nfunc marker() {}\n";
    constexpr std::string_view python = "def marker():\n    pass\n";
    constexpr std::string_view javascript = "function marker() {}\n";
    constexpr std::string_view typescript =
        "function marker(value: number): number { return value; }\n";
    constexpr std::string_view rust = "fn marker() {}\n";
    constexpr std::string_view c = "void marker(void) {}\n";
    constexpr std::string_view cpp = "namespace fixture { void marker() {} }\n";
    constexpr std::string_view java =
        "class Fixture { void marker() {} }\n";
    constexpr std::string_view csharp =
        "class Fixture { void Marker() {} }\n";
    constexpr std::string_view php = "<?php function marker() {}\n";
    constexpr std::string_view kotlin = "fun marker() = Unit\n";
    constexpr std::string_view zig = "fn marker() void {}\n";
    constexpr std::string_view ruby = "def marker\nend\n";

    constexpr std::array fixtures{
        ExtensionFixture{".go", go, "marker", SymbolType::Function},
        ExtensionFixture{".py", python, "marker", SymbolType::Function},
        ExtensionFixture{".pyw", python, "marker", SymbolType::Function},
        ExtensionFixture{".pyi", python, "marker", SymbolType::Function},
        ExtensionFixture{".pyx", python, "marker", SymbolType::Function},
        ExtensionFixture{".pxd", python, "marker", SymbolType::Function},
        ExtensionFixture{".js", javascript, "marker", SymbolType::Function},
        ExtensionFixture{".jsx", javascript, "marker", SymbolType::Function},
        ExtensionFixture{".mjs", javascript, "marker", SymbolType::Function},
        ExtensionFixture{".cjs", javascript, "marker", SymbolType::Function},
        ExtensionFixture{".ts", typescript, "marker", SymbolType::Function},
        ExtensionFixture{".tsx", typescript, "marker", SymbolType::Function},
        ExtensionFixture{".mts", typescript, "marker", SymbolType::Function},
        ExtensionFixture{".cts", typescript, "marker", SymbolType::Function},
        ExtensionFixture{".rs", rust, "marker", SymbolType::Function},
        ExtensionFixture{".c", c, "marker", SymbolType::Function},
        ExtensionFixture{".cpp", cpp, "marker", SymbolType::Function},
        ExtensionFixture{".cc", cpp, "marker", SymbolType::Function},
        ExtensionFixture{".cxx", cpp, "marker", SymbolType::Function},
        ExtensionFixture{".h", cpp, "marker", SymbolType::Function},
        ExtensionFixture{".hpp", cpp, "marker", SymbolType::Function},
        ExtensionFixture{".hh", cpp, "marker", SymbolType::Function},
        ExtensionFixture{".hxx", cpp, "marker", SymbolType::Function},
        ExtensionFixture{".h++", cpp, "marker", SymbolType::Function},
        ExtensionFixture{".java", java, "marker", SymbolType::Method},
        ExtensionFixture{".cs", csharp, "Marker", SymbolType::Method},
        ExtensionFixture{".php", php, "marker", SymbolType::Function},
        ExtensionFixture{".phtml", php, "marker", SymbolType::Function},
        ExtensionFixture{".kt", kotlin, "marker", SymbolType::Function},
        ExtensionFixture{".kts", kotlin, "marker", SymbolType::Function},
        ExtensionFixture{".zig", zig, "marker", SymbolType::Function},
        ExtensionFixture{".rb", ruby, "marker", SymbolType::Method},
    };

    for (const auto& fixture : fixtures) {
        SCOPED_TRACE(fixture.extension);
        Language routed{};
        ASSERT_TRUE(language_from_extension(fixture.extension, routed));
        const std::string path = "fixture" + std::string(fixture.extension);
        auto graph = extract_spec_fixture(routed, fixture.extension, path,
                                          fixture.source);
        const int named = static_cast<int>(std::count_if(
            graph.symbols.begin(), graph.symbols.end(),
            [&](const Symbol& symbol) { return symbol.name == fixture.symbol; }));
        const int typed = static_cast<int>(std::count_if(
            graph.symbols.begin(), graph.symbols.end(), [&](const Symbol& symbol) {
                return symbol.name == fixture.symbol &&
                       symbol.type == fixture.symbol_type;
            }));
        EXPECT_EQ(named, 1) << fixture.extension;
        EXPECT_EQ(typed, 1) << fixture.extension;
    }
}

TEST(LanguageConstructConformance, JsxSyntaxReachesTheDataGraph) {
    constexpr std::string_view jsx = R"(
function Card(props) { return <article>{props.title}</article>; }
const view = <Card title="ready" />;
)";
    auto jsx_graph =
        extract_spec_fixture(Language::JavaScript, ".jsx", "fixture.jsx", jsx);
    expect_graph(jsx_graph,
                 {{"Card", SymbolType::Function},
                  {"view", SymbolType::Variable}},
                 {});
}

TEST(LanguageConstructConformance, Go) {
    constexpr std::string_view source = R"(package fixture
import "fmt"
type Reader interface { Read() string }
type Base struct{}
type Service struct { Base }
type Label = string
const Limit = 4
var Enabled = true
func Build() *Service { return &Service{} }
func (s *Service) Read() string { return fmt.Sprint(Limit) }
func Run() string { return Build().Read() }
)";
    auto graph = extract_spec_fixture(Language::Go, ".go", "fixture.go", source);
    expect_graph(graph,
                 {{"Reader", SymbolType::Interface},
                  {"Read", SymbolType::Method},
                  {"Base", SymbolType::Struct},
                  {"Service", SymbolType::Struct},
                  {"Label", SymbolType::Type},
                  {"Limit", SymbolType::Constant},
                  {"Enabled", SymbolType::Variable},
                  {"Build", SymbolType::Function},
                  {"Read", SymbolType::Method},
                  {"Run", SymbolType::Function}},
                 {{"Base", ReferenceType::Extends},
                  {"Build", ReferenceType::Call}},
                 {"fmt"});
}

TEST(LanguageConstructConformance, Python) {
    constexpr std::string_view source = R"(import pathlib
from collections import abc
class Base:
    def ping(self):
        return "base"
class Service(Base):
    @classmethod
    def make(cls):
        return cls()
    async def run(self):
        return self.ping()
def helper():
    return Service.make()
)";
    auto graph = extract_spec_fixture(Language::Python, ".py", "fixture.py", source);
    expect_graph(graph,
                 {{"Base", SymbolType::Class},
                  {"Service", SymbolType::Class},
                  {"ping", SymbolType::Method},
                  {"make", SymbolType::Method},
                  {"run", SymbolType::Method},
                  {"helper", SymbolType::Function}},
                 {{"Base", ReferenceType::Extends},
                  {"Service.make", ReferenceType::Call}},
                 {"import pathlib", "from collections import abc"});
}

TEST(LanguageConstructConformance, JavaScript) {
    constexpr std::string_view source = R"(import value from "./dep.js";
export class Base { ping() { return value; } }
export class Service extends Base {
  status = "ready";
  constructor() { super(); }
  run() { return this.ping(); }
}
export function build() { return new Service(); }
export function* ids() { yield 1; }
export const arrow = (x) => build(x);
let enabled = true;
)";
    auto graph = extract_spec_fixture(Language::JavaScript, ".js", "fixture.js", source);
    expect_graph(graph,
                 {{"Base", SymbolType::Class},
                  {"Service", SymbolType::Class},
                  {"status", SymbolType::Property},
                  {"constructor", SymbolType::Method},
                  {"ping", SymbolType::Method},
                  {"run", SymbolType::Method},
                  {"build", SymbolType::Function},
                  {"ids", SymbolType::Function},
                  {"arrow", SymbolType::Function},
                  {"arrow", SymbolType::Variable},
                  {"enabled", SymbolType::Variable}},
                 {{"Base", ReferenceType::Extends},
                  {"Service", ReferenceType::Usage, 2},
                  {"Service.constructor", ReferenceType::Call},
                  {"build", ReferenceType::Call}},
                 {"./dep.js"});
}

TEST(LanguageConstructConformance, TypeScript) {
    constexpr std::string_view source = R"(import { dep } from "./dep";
interface Runnable { run(): number; }
type Identifier = string | number;
enum State { Ready, Done }
class Base { ping(): number { return dep(); } }
class Service extends Base implements Runnable {
  private count: number = 0;
  constructor(public id: Identifier) { super(); }
  run(): number { return this.ping(); }
}
function build(id: Identifier): Service { return new Service(id); }
const arrow = (id: Identifier): Service => build(id);
)";
    auto graph = extract_spec_fixture(Language::TypeScript, ".ts", "fixture.ts", source);
    expect_graph(graph,
                 {{"Runnable", SymbolType::Interface},
                  {"Identifier", SymbolType::Type},
                  {"State", SymbolType::Enum},
                  {"Base", SymbolType::Class},
                  {"Service", SymbolType::Class},
                  {"count", SymbolType::Property},
                  {"constructor", SymbolType::Method},
                  {"ping", SymbolType::Method},
                  {"run", SymbolType::Method},
                  {"build", SymbolType::Function},
                  {"arrow", SymbolType::Function},
                  {"arrow", SymbolType::Variable}},
                 {{"Base", ReferenceType::Extends},
                  {"Runnable", ReferenceType::Implements},
                  {"Service", ReferenceType::Usage},
                  {"Service.constructor", ReferenceType::Call},
                  {"build", ReferenceType::Call}},
                 {"./dep"});
}

TEST(LanguageConstructConformance, Rust) {
    constexpr std::string_view source = R"(use std::fmt;
mod nested { pub const VALUE: i32 = 1; }
struct Point { x: i32 }
enum State { Ready, Done }
trait Runnable { fn run(&self); }
fn helper() {}
impl Runnable for Point { fn run(&self) { helper(); } }
fn build() -> Point { Point { x: 1 } }
)";
    auto graph = extract_spec_fixture(Language::Rust, ".rs", "fixture.rs", source);
    expect_graph(graph,
                 {{"nested", SymbolType::Module},
                  {"Point", SymbolType::Struct},
                  {"x", SymbolType::Field},
                  {"State", SymbolType::Enum},
                  {"Runnable", SymbolType::Trait},
                  {"Runnable", SymbolType::Impl},
                  {"helper", SymbolType::Function},
                  {"run", SymbolType::Method},
                 {"build", SymbolType::Function}},
                 {{"helper", ReferenceType::Call}},
                 {"use std::fmt;"});
}

TEST(LanguageConstructConformance, C) {
    constexpr std::string_view source = R"(#include <stddef.h>
typedef struct Point { int x; } Point;
enum State { READY, DONE };
int add(int left, int right) { return left + right; }
int run(void) { Point point = {1}; return add(point.x, 4); }
)";
    auto graph = extract_spec_fixture(Language::C, ".c", "fixture.c", source);
    expect_graph(graph,
                 {{"Point", SymbolType::Struct},
                  {"x", SymbolType::Field},
                  {"State", SymbolType::Enum},
                  {"add", SymbolType::Function},
                  {"left", SymbolType::Variable},
                  {"right", SymbolType::Variable},
                  {"point", SymbolType::Variable},
                  {"run", SymbolType::Function}},
                 {{"add", ReferenceType::Call}}, {"stddef.h"});
}

TEST(LanguageConstructConformance, Cpp) {
    constexpr std::string_view source = R"(#include <string>
namespace fixture {
struct Point { int x; };
enum class State { Ready, Done };
class Base { public: virtual int run() = 0; };
class Service : public Base {
 public:
  Service() {}
  int run() override { return helper(); }
  static int helper() { return 1; }
};
using Name = std::string;
int build() { Service service; auto ptr = new Service(); return service.run(); }
}
)";
    auto graph = extract_spec_fixture(Language::Cpp, ".cpp", "fixture.cpp", source);
    expect_graph(graph,
                 {{"fixture", SymbolType::Namespace},
                  {"Point", SymbolType::Struct},
                  {"x", SymbolType::Field},
                  {"State", SymbolType::Enum},
                  {"Base", SymbolType::Class},
                  {"Service", SymbolType::Class},
                  {"Service", SymbolType::Constructor},
                  {"run", SymbolType::Method},
                  {"helper", SymbolType::Method},
                  {"Name", SymbolType::Type},
                  {"build", SymbolType::Function},
                  {"service", SymbolType::Variable},
                  {"ptr", SymbolType::Variable}},
                 {{"Service.Service", ReferenceType::Call},
                  {"helper", ReferenceType::Call},
                  {"Service.run", ReferenceType::Call}},
                 {"string"});
}

TEST(LanguageConstructConformance, Java) {
    constexpr std::string_view source = R"(package fixture;
import java.util.List;
@interface Marker {}
interface Runnable { int run(); }
enum State { READY, DONE }
record Point(int x) {}
class Base { int ping() { return 1; } }
class Service extends Base implements Runnable {
  private int count;
  Service() {}
  public int run() { return ping(); }
}
class Factory { Service build() { return new Service(); } }
)";
    auto graph = extract_spec_fixture(Language::Java, ".java", "Fixture.java", source);
    expect_graph(graph,
                 {{"Marker", SymbolType::Annotation},
                  {"Runnable", SymbolType::Interface},
                  {"State", SymbolType::Enum},
                  {"Point", SymbolType::Record},
                  {"Base", SymbolType::Class},
                  {"ping", SymbolType::Method},
                  {"Service", SymbolType::Class},
                  {"count", SymbolType::Field},
                  {"Service", SymbolType::Constructor},
                  {"run", SymbolType::Method},
                  {"run", SymbolType::Method},
                  {"Factory", SymbolType::Class},
                  {"build", SymbolType::Method}},
                 {{"Service.ping", ReferenceType::Call},
                  {"Service.Service", ReferenceType::Call}},
                 {"package fixture;", "import java.util.List;"});
}

TEST(LanguageConstructConformance, CSharp) {
    constexpr std::string_view source = R"(using System;
namespace Fixture {
public delegate void Changed();
public interface IRunnable { int Run(); }
public enum State { Ready, Done }
public struct Point { public int X; }
public record Result(int Value);
public class Service : IRunnable {
  public event Changed Updated;
  public int Value { get; set; }
  private int count;
  public Service() {}
  public int Run() { return Helper(); }
  private int Helper() { return count; }
}
public class Factory { public Service Build() { return new Service(); } }
}
)";
    auto graph = extract_spec_fixture(Language::CSharp, ".cs", "Fixture.cs", source);
    expect_graph(graph,
                 {{"Fixture", SymbolType::Namespace},
                  {"Changed", SymbolType::Delegate},
                  {"IRunnable", SymbolType::Interface},
                  {"State", SymbolType::Enum},
                  {"Point", SymbolType::Struct},
                  {"X", SymbolType::Field},
                  {"Result", SymbolType::Record},
                  {"Service", SymbolType::Class},
                  {"Updated", SymbolType::Event},
                  {"Value", SymbolType::Property},
                  {"count", SymbolType::Field},
                  {"Service", SymbolType::Constructor},
                  {"Run", SymbolType::Method},
                  {"Run", SymbolType::Method},
                  {"Helper", SymbolType::Method},
                  {"Factory", SymbolType::Class},
                  {"Build", SymbolType::Method}},
                 {{"Service.Helper", ReferenceType::Call},
                  {"Service.Service", ReferenceType::Call}},
                 {"System"});
}

TEST(LanguageConstructConformance, PHP) {
    constexpr std::string_view source = R"(<?php
namespace Fixture;
use Vendor\Dependency;
interface Runnable { public function run(): int; }
trait Helper { public function help(): int { return 1; } }
enum State { case Ready; case Done; }
class Base {}
class Service extends Base implements Runnable {
    use Helper;
    public const LIMIT = 4;
    private int $count = 0;
    public function __construct() {}
    public function run(): int { return $this->help(); }
}
function build(): Service { return new Service(); }
)";
    auto graph = extract_spec_fixture(Language::PHP, ".php", "fixture.php", source);
    expect_graph(graph,
                 {{"Fixture", SymbolType::Namespace},
                  {"Runnable", SymbolType::Interface},
                  {"run", SymbolType::Method},
                  {"Helper", SymbolType::Trait},
                  {"help", SymbolType::Method},
                  {"State", SymbolType::Enum},
                  {"Base", SymbolType::Class},
                  {"Service", SymbolType::Class},
                  {"LIMIT", SymbolType::Constant},
                  {"count", SymbolType::Property},
                  {"__construct", SymbolType::Method},
                  {"run", SymbolType::Method},
                  {"build", SymbolType::Function}},
                 {{"Base", ReferenceType::Extends},
                  {"Runnable", ReferenceType::Implements},
                  {"Helper", ReferenceType::Extends},
                  {"Service.help", ReferenceType::Call},
                  {"Service.__construct", ReferenceType::Call}},
                 {"use Vendor\\Dependency;", "use Helper;"});
}

TEST(LanguageConstructConformance, Kotlin) {
    constexpr std::string_view source = R"(package fixture
import kotlin.math.abs
interface Runnable { fun run(): Int }
enum class State { Ready, Done }
data class Point(val x: Int)
object Registry { fun value(): Int = 1 }
class Service : Runnable {
    override fun run(): Int = helper()
    private fun helper(): Int = Registry.value()
}
fun build(): Service = Service()
)";
    auto graph = extract_spec_fixture(Language::Kotlin, ".kt", "Fixture.kt", source);
    expect_graph(graph,
                 {{"Runnable", SymbolType::Interface},
                  {"run", SymbolType::Method},
                  {"State", SymbolType::Enum},
                  {"Point", SymbolType::Class},
                  {"Registry", SymbolType::Object},
                  {"value", SymbolType::Method},
                  {"Service", SymbolType::Class},
                  {"run", SymbolType::Method},
                  {"helper", SymbolType::Method},
                  {"build", SymbolType::Function}},
                 {{"Service.helper", ReferenceType::Call},
                  {"Registry.value", ReferenceType::Call},
                  {"Service", ReferenceType::Call}},
                 {"import kotlin.math.abs"});
}

TEST(LanguageConstructConformance, Zig) {
    constexpr std::string_view source = R"(const std = @import("std");
const helper = @import("helpers/math.zig");
const sibling = @import("analysis.zig");
const Point = struct {
    x: i32,
    fn value(self: Point) i32 { return self.x; }
};
const State = enum { ready, done };
const Payload = union(enum) { value: i32, empty };
fn build() Point { return Point{ .x = 1 }; }
pub fn run() i32 { const point = Point{ .x = 1 }; return point.value(); }
)";
    auto graph = extract_spec_fixture(Language::Zig, ".zig", "fixture.zig", source);
    expect_graph(graph,
                 {{"Point", SymbolType::Struct},
                  {"value", SymbolType::Method},
                  {"State", SymbolType::Enum},
                  {"Payload", SymbolType::Struct},
                  {"build", SymbolType::Function},
                  {"run", SymbolType::Function}},
                 {{"Point.value", ReferenceType::Call}},
                 {"std", "helpers/math.zig", "analysis.zig"});
}

TEST(LanguageConstructConformance, Ruby) {
    constexpr std::string_view source = R"(require "json"
module Fixture
  class Base
    def ping
      1
    end
  end
  class Service < Base
    def self.build
      Service.new
    end
    def run
      self.ping
    end
  end
end
def helper
  Fixture::Service.build
end
)";
    auto graph = extract_spec_fixture(Language::Ruby, ".rb", "fixture.rb", source);
    expect_graph(graph,
                 {{"Fixture", SymbolType::Module},
                  {"Base", SymbolType::Class},
                  {"Service", SymbolType::Class},
                  {"ping", SymbolType::Method},
                  {"build", SymbolType::Method},
                  {"run", SymbolType::Method},
                  {"helper", SymbolType::Method}},
                 {{"Service.ping", ReferenceType::Call},
                  {"Service.build", ReferenceType::Call},
                  {"Service.initialize", ReferenceType::Call}});
}

}  // namespace
}  // namespace lci::parser
