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

namespace lci::parser {
namespace {

struct ExpectedSymbol {
    std::string_view name;
    SymbolType type;
};

struct ExpectedReference {
    std::string_view name;
    ReferenceType type;
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

const Symbol* find_symbol(const ExtractionResults& graph,
                          std::string_view name) {
    const auto it = std::find_if(
        graph.symbols.begin(), graph.symbols.end(),
        [name](const Symbol& symbol) { return symbol.name == name; });
    return it == graph.symbols.end() ? nullptr : &*it;
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

    for (const auto& expected : symbols) {
        int matches = 0;
        for (const auto& actual : graph.symbols) {
            if (actual.name == expected.name && actual.type == expected.type)
                ++matches;
        }
        EXPECT_GE(matches, 1)
            << expected.name << " (" << to_string(expected.type) << ")\n"
            << graph_summary;
    }
    for (const auto& expected : references) {
        bool found = false;
        for (const auto& actual : graph.references) {
            if (actual.referenced_name == expected.name &&
                actual.type == expected.type) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found)
            << expected.name << " (reference type "
            << static_cast<int>(expected.type) << ")\n"
            << graph_summary;
    }
    for (const auto expected : imports) {
        bool found = false;
        for (const auto& actual : graph.imports) {
            if (actual.path.find(expected) != std::string::npos) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "import " << expected << '\n' << graph_summary;
    }
    EXPECT_FALSE(graph.scopes.empty());
    EXPECT_FALSE(graph.blocks.empty());
}

struct ExtensionFixture {
    std::string_view extension;
    Language language;
    std::string_view source;
    std::string_view symbol;
};

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
        ExtensionFixture{".go", Language::Go, go, "marker"},
        ExtensionFixture{".py", Language::Python, python, "marker"},
        ExtensionFixture{".pyw", Language::Python, python, "marker"},
        ExtensionFixture{".pyi", Language::Python, python, "marker"},
        ExtensionFixture{".pyx", Language::Python, python, "marker"},
        ExtensionFixture{".pxd", Language::Python, python, "marker"},
        ExtensionFixture{".js", Language::JavaScript, javascript, "marker"},
        ExtensionFixture{".jsx", Language::JavaScript, javascript, "marker"},
        ExtensionFixture{".mjs", Language::JavaScript, javascript, "marker"},
        ExtensionFixture{".cjs", Language::JavaScript, javascript, "marker"},
        ExtensionFixture{".ts", Language::TypeScript, typescript, "marker"},
        ExtensionFixture{".tsx", Language::Tsx, typescript, "marker"},
        ExtensionFixture{".mts", Language::TypeScript, typescript, "marker"},
        ExtensionFixture{".cts", Language::TypeScript, typescript, "marker"},
        ExtensionFixture{".rs", Language::Rust, rust, "marker"},
        ExtensionFixture{".c", Language::C, c, "marker"},
        ExtensionFixture{".cpp", Language::Cpp, cpp, "marker"},
        ExtensionFixture{".cc", Language::Cpp, cpp, "marker"},
        ExtensionFixture{".cxx", Language::Cpp, cpp, "marker"},
        ExtensionFixture{".h", Language::Cpp, cpp, "marker"},
        ExtensionFixture{".hpp", Language::Cpp, cpp, "marker"},
        ExtensionFixture{".hh", Language::Cpp, cpp, "marker"},
        ExtensionFixture{".hxx", Language::Cpp, cpp, "marker"},
        ExtensionFixture{".h++", Language::Cpp, cpp, "marker"},
        ExtensionFixture{".java", Language::Java, java, "marker"},
        ExtensionFixture{".cs", Language::CSharp, csharp, "Marker"},
        ExtensionFixture{".php", Language::PHP, php, "marker"},
        ExtensionFixture{".phtml", Language::PHP, php, "marker"},
        ExtensionFixture{".kt", Language::Kotlin, kotlin, "marker"},
        ExtensionFixture{".kts", Language::Kotlin, kotlin, "marker"},
        ExtensionFixture{".zig", Language::Zig, zig, "marker"},
        ExtensionFixture{".rb", Language::Ruby, ruby, "marker"},
    };

    for (const auto& fixture : fixtures) {
        SCOPED_TRACE(fixture.extension);
        Language routed{};
        ASSERT_TRUE(language_from_extension(fixture.extension, routed));
        EXPECT_EQ(routed, fixture.language);
        const std::string path = "fixture" + std::string(fixture.extension);
        auto graph = extract_spec_fixture(routed, fixture.extension, path,
                                          fixture.source);
        EXPECT_NE(find_symbol(graph, fixture.symbol), nullptr);
        EXPECT_FALSE(graph.scopes.empty());
        EXPECT_FALSE(graph.blocks.empty());
    }
}

TEST(LanguageConstructConformance, JsxAndTsxSyntaxReachTheirDataGraphs) {
    constexpr std::string_view jsx = R"(
function Card(props) { return <article>{props.title}</article>; }
const view = <Card title="ready" />;
)";
    auto jsx_graph =
        extract_spec_fixture(Language::JavaScript, ".jsx", "fixture.jsx", jsx);
    EXPECT_NE(find_symbol(jsx_graph, "Card"), nullptr);
    EXPECT_NE(find_symbol(jsx_graph, "view"), nullptr);

    constexpr std::string_view tsx = R"(
interface Props { title: string }
function Card(props: Props) { return <article>{props.title}</article>; }
const view = <Card title="ready" />;
)";
    auto tsx_graph =
        extract_spec_fixture(Language::Tsx, ".tsx", "fixture.tsx", tsx);
    EXPECT_NE(find_symbol(tsx_graph, "Props"), nullptr);
    EXPECT_NE(find_symbol(tsx_graph, "Card"), nullptr);
    EXPECT_NE(find_symbol(tsx_graph, "view"), nullptr);
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
                  {"ping", SymbolType::Method},
                  {"run", SymbolType::Method},
                  {"build", SymbolType::Function},
                  {"ids", SymbolType::Function},
                  {"arrow", SymbolType::Function},
                  {"enabled", SymbolType::Variable}},
                 {{"Base", ReferenceType::Extends},
                  {"Service", ReferenceType::Usage},
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
                  {"run", SymbolType::Method},
                  {"build", SymbolType::Function},
                  {"arrow", SymbolType::Function}},
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
                  {"State", SymbolType::Enum},
                  {"Runnable", SymbolType::Trait},
                  {"helper", SymbolType::Function},
                  {"run", SymbolType::Method},
                  {"build", SymbolType::Function}},
                 {{"helper", ReferenceType::Call}},
                 {"std::fmt"});
    EXPECT_GE(std::count_if(graph.symbols.begin(), graph.symbols.end(),
                            [](const Symbol& symbol) {
                                return symbol.type == SymbolType::Impl;
                            }),
              1);
}

TEST(LanguageConstructConformance, C) {
    constexpr std::string_view source = R"(#include <stddef.h>
typedef struct Point { int x; } Point;
enum State { READY, DONE };
static const int limit = 4;
int add(int left, int right) { return left + right; }
int run(void) { Point point = {1}; return add(point.x, limit); }
)";
    auto graph = extract_spec_fixture(Language::C, ".c", "fixture.c", source);
    expect_graph(graph,
                 {{"Point", SymbolType::Struct},
                  {"State", SymbolType::Enum},
                  {"add", SymbolType::Function},
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
                  {"State", SymbolType::Enum},
                  {"Base", SymbolType::Class},
                  {"Service", SymbolType::Class},
                  {"Service", SymbolType::Constructor},
                  {"run", SymbolType::Method},
                  {"helper", SymbolType::Method},
                  {"build", SymbolType::Function}},
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
                  {"Service", SymbolType::Class},
                  {"Service", SymbolType::Constructor},
                  {"run", SymbolType::Method}},
                 {{"Service.ping", ReferenceType::Call},
                  {"Service.Service", ReferenceType::Call}},
                 {"package fixture", "import java.util.List"});
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
                  {"Result", SymbolType::Record},
                  {"Service", SymbolType::Class},
                  {"Updated", SymbolType::Event},
                  {"Value", SymbolType::Property},
                  {"count", SymbolType::Field},
                  {"Service", SymbolType::Constructor},
                  {"Run", SymbolType::Method},
                  {"Helper", SymbolType::Method}},
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
    public function __construct() {}
    public function run(): int { return $this->help(); }
}
function build(): Service { return new Service(); }
)";
    auto graph = extract_spec_fixture(Language::PHP, ".php", "fixture.php", source);
    expect_graph(graph,
                 {{"Fixture", SymbolType::Namespace},
                  {"Runnable", SymbolType::Interface},
                  {"Helper", SymbolType::Trait},
                  {"State", SymbolType::Enum},
                  {"Base", SymbolType::Class},
                  {"Service", SymbolType::Class},
                  {"LIMIT", SymbolType::Constant},
                  {"run", SymbolType::Method},
                  {"build", SymbolType::Function}},
                 {{"Base", ReferenceType::Extends},
                  {"Runnable", ReferenceType::Implements},
                  {"Helper", ReferenceType::Extends},
                  {"Service.help", ReferenceType::Call},
                  {"Service.__construct", ReferenceType::Call}},
                 {"Vendor\\Dependency"});
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
                 {"kotlin.math.abs"});
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
