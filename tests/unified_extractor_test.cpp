#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <gtest/gtest.h>
#include <tree_sitter/api.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace lci::parser {
namespace {

// Helper: parse source code and return the tree.
UniqueTree parse(Language lang, std::string_view src) {
    UniqueParser parser = make_parser(lang);
    if (!parser) return nullptr;
    TSTree* raw = ts_parser_parse_string(
        parser.get(), nullptr, src.data(),
        static_cast<uint32_t>(src.size()));
    return UniqueTree(raw);
}

// Helper: find symbol by name in results.
const Symbol* find_symbol(const ExtractionResults& r,
                          std::string_view name) {
    for (const auto& s : r.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// Helper: find block by name in results.
const BlockBoundary* find_block(const ExtractionResults& r,
                                std::string_view name) {
    for (const auto& b : r.blocks) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

// Helper: count symbols of a given type.
int count_symbols(const ExtractionResults& r, SymbolType type) {
    int n = 0;
    for (const auto& s : r.symbols) {
        if (s.type == type) ++n;
    }
    return n;
}

// Helper: count references of a given type.
int count_refs(const ExtractionResults& r, ReferenceType type) {
    int n = 0;
    for (const auto& ref : r.references) {
        if (ref.type == type) ++n;
    }
    return n;
}

// Helper: find complexity for a position key.
int find_complexity(const ExtractionResults& r, int line, int column) {
    for (const auto& [key, cx] : r.complexity) {
        if (key.line == line && key.column == column) return cx;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Go extraction tests
// ---------------------------------------------------------------------------

constexpr std::string_view kGoSource = R"(package main

import "fmt"

type Greeter interface {
	Greet(name string) string
}

type MyStruct struct {
	Name string
	Age  int
}

func (m *MyStruct) Greet(name string) string {
	if name == "" {
		return "Hello, World!"
	}
	return fmt.Sprintf("Hello, %s!", name)
}

func add(a, b int) int {
	return a + b
}

var globalVar = 42

const Pi = 3.14159
)";

TEST(UnifiedExtractorTest, GoFunctions) {
    auto tree = parse(Language::Go, kGoSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kGoSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // Should find the add function
    const Symbol* add_sym = find_symbol(r, "add");
    ASSERT_NE(add_sym, nullptr);
    EXPECT_EQ(add_sym->type, SymbolType::Function);

    // Should find the Greet method (Go method_declaration)
    const Symbol* greet_sym = find_symbol(r, "Greet");
    ASSERT_NE(greet_sym, nullptr);
    EXPECT_EQ(greet_sym->type, SymbolType::Method);

    // Parameter counts: `func add(a, b int)` declares TWO names in one
    // parameter_declaration; `Greet(name string)` declares one (the
    // receiver is a separate field and is not a parameter). This field
    // previously had no writer anywhere, so the /list-symbols params
    // sort ranked an all-zero column.
    EXPECT_EQ(add_sym->parameter_count, 2);
    EXPECT_EQ(greet_sym->parameter_count, 1);
}

TEST(UnifiedExtractorTest, ParameterCountsAcrossLanguages) {
    {
        constexpr std::string_view src =
            "def solo():\n    pass\n\n"
            "def trio(a, b, c=1):\n    pass\n";
        auto tree = parse(Language::Python, src);
        ASSERT_NE(tree.get(), nullptr);
        UnifiedExtractor ue;
        ue.init(src, 1, ".py", "m.py");
        ue.extract(tree.get());
        auto r = ue.get_results();
        ASSERT_NE(find_symbol(r, "solo"), nullptr);
        EXPECT_EQ(find_symbol(r, "solo")->parameter_count, 0);
        ASSERT_NE(find_symbol(r, "trio"), nullptr);
        EXPECT_EQ(find_symbol(r, "trio")->parameter_count, 3);
    }
    {
        constexpr std::string_view src =
            "int pair(int a, char b) { return a; }\n"
            "void none() {}\n";
        auto tree = parse(Language::Cpp, src);
        ASSERT_NE(tree.get(), nullptr);
        UnifiedExtractor ue;
        ue.init(src, 1, ".cpp", "m.cpp");
        ue.extract(tree.get());
        auto r = ue.get_results();
        ASSERT_NE(find_symbol(r, "pair"), nullptr);
        EXPECT_EQ(find_symbol(r, "pair")->parameter_count, 2);
        ASSERT_NE(find_symbol(r, "none"), nullptr);
        EXPECT_EQ(find_symbol(r, "none")->parameter_count, 0);
    }
    {
        constexpr std::string_view src =
            "function two(x, y) { return x; }\n"
            "const one = (z) => z;\n";
        auto tree = parse(Language::JavaScript, src);
        ASSERT_NE(tree.get(), nullptr);
        UnifiedExtractor ue;
        ue.init(src, 1, ".js", "m.js");
        ue.extract(tree.get());
        auto r = ue.get_results();
        ASSERT_NE(find_symbol(r, "two"), nullptr);
        EXPECT_EQ(find_symbol(r, "two")->parameter_count, 2);
        ASSERT_NE(find_symbol(r, "one"), nullptr);
        EXPECT_EQ(find_symbol(r, "one")->parameter_count, 1);
    }
}

TEST(UnifiedExtractorTest, GoTypes) {
    auto tree = parse(Language::Go, kGoSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kGoSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // Should find MyStruct as struct
    const Symbol* my_struct = find_symbol(r, "MyStruct");
    ASSERT_NE(my_struct, nullptr);
    EXPECT_EQ(my_struct->type, SymbolType::Struct);

    // Should find Greeter as interface
    const Symbol* greeter = find_symbol(r, "Greeter");
    ASSERT_NE(greeter, nullptr);
    EXPECT_EQ(greeter->type, SymbolType::Interface);
}

TEST(UnifiedExtractorTest, GoVariablesAndConstants) {
    auto tree = parse(Language::Go, kGoSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kGoSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Symbol* gv = find_symbol(r, "globalVar");
    ASSERT_NE(gv, nullptr);
    EXPECT_EQ(gv->type, SymbolType::Variable);

    const Symbol* pi = find_symbol(r, "Pi");
    ASSERT_NE(pi, nullptr);
    EXPECT_EQ(pi->type, SymbolType::Constant);
}

// Go groups declarations in `var ( ... )` / `const ( ... )` blocks, which
// nest their specs one level deeper than a single-line declaration. The
// extractor only inspected DIRECT children of the declaration, so every
// grouped name was dropped: `lci browse binding/form_mapping.go` on gin
// listed its first symbol at line 32 and skipped the three package errors
// above it, while `lci refs ErrConvertMapStringSlice` still resolved both
// the declaration and its use. The index knew the references and not the
// declaration.
TEST(UnifiedExtractorTest, GoGroupedVarAndConstBlocks) {
    constexpr std::string_view kSource = R"(package main

import "errors"

var (
	errUnknownType = errors.New("unknown type")

	// ErrConvertMapStringSlice can not convert to map[string][]string
	ErrConvertMapStringSlice = errors.New("can not convert")
)

const (
	StatusOK   = 200
	StatusFail = 500
)
)";
    auto tree = parse(Language::Go, kSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Symbol* first = find_symbol(r, "errUnknownType");
    ASSERT_NE(first, nullptr) << "grouped var members must be indexed";
    EXPECT_EQ(first->type, SymbolType::Variable);

    const Symbol* exported = find_symbol(r, "ErrConvertMapStringSlice");
    ASSERT_NE(exported, nullptr);
    EXPECT_EQ(exported->type, SymbolType::Variable);

    const Symbol* ok = find_symbol(r, "StatusOK");
    ASSERT_NE(ok, nullptr) << "grouped const members must be indexed";
    EXPECT_EQ(ok->type, SymbolType::Constant);
    const Symbol* fail = find_symbol(r, "StatusFail");
    ASSERT_NE(fail, nullptr);
    EXPECT_EQ(fail->type, SymbolType::Constant);
}

// Each name reports its OWN line. The declaration's span covers the whole
// block, so stamping every member with it would point a reader at `var (`
// for a symbol thirty lines down.
TEST(UnifiedExtractorTest, GoGroupedMembersCarryTheirOwnLines) {
    constexpr std::string_view kSource = R"(package main

var (
	alpha = 1

	beta = 2
)
)";
    auto tree = parse(Language::Go, kSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Symbol* alpha = find_symbol(r, "alpha");
    const Symbol* beta = find_symbol(r, "beta");
    ASSERT_NE(alpha, nullptr);
    ASSERT_NE(beta, nullptr);
    EXPECT_EQ(alpha->line, 4);
    EXPECT_EQ(beta->line, 6);
}

// Grouped `type ( ... )` blocks lost every member after the first: the scan
// stopped at the first type_spec, and the one it kept was stamped with the
// declaration's line rather than its own.
TEST(UnifiedExtractorTest, GoGroupedTypeBlock) {
    constexpr std::string_view kSource = R"(package main

type (
	Alpha struct{ N int }

	Beta interface{ Do() }
)
)";
    auto tree = parse(Language::Go, kSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Symbol* alpha = find_symbol(r, "Alpha");
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->type, SymbolType::Struct);
    EXPECT_EQ(alpha->line, 4) << "its own line, not the `type (` line";

    const Symbol* beta = find_symbol(r, "Beta");
    ASSERT_NE(beta, nullptr) << "members after the first must survive";
    EXPECT_EQ(beta->type, SymbolType::Interface);
    EXPECT_EQ(beta->line, 6);
}

TEST(UnifiedExtractorTest, GoImports) {
    auto tree = parse(Language::Go, kGoSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kGoSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    ASSERT_FALSE(r.imports.empty());
    bool found_fmt = false;
    for (const auto& imp : r.imports) {
        if (imp.path == "fmt") found_fmt = true;
    }
    EXPECT_TRUE(found_fmt);
}

TEST(UnifiedExtractorTest, GoScopes) {
    auto tree = parse(Language::Go, kGoSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kGoSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // Should have at least a file-level scope
    bool has_file_scope = false;
    for (const auto& s : r.scopes) {
        if (s.type == ScopeType::File) has_file_scope = true;
    }
    EXPECT_TRUE(has_file_scope);

    // Should have function scopes
    int func_scopes = 0;
    for (const auto& s : r.scopes) {
        if (s.type == ScopeType::Function) func_scopes++;
    }
    EXPECT_GE(func_scopes, 1);
}

TEST(UnifiedExtractorTest, GoComplexity) {
    auto tree = parse(Language::Go, kGoSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kGoSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // The Greet method has an if statement, so complexity should be >= 2
    const Symbol* greet = find_symbol(r, "Greet");
    ASSERT_NE(greet, nullptr);
    int cx = find_complexity(r, greet->line, greet->column);
    EXPECT_GE(cx, 2) << "Greet method should have complexity >= 2 (base + if)";

    // add function has no branching, complexity should be 1
    const Symbol* add_sym = find_symbol(r, "add");
    ASSERT_NE(add_sym, nullptr);
    int add_cx = find_complexity(r, add_sym->line, add_sym->column);
    EXPECT_EQ(add_cx, 1) << "add function should have complexity 1 (base only)";
}

TEST(UnifiedExtractorTest, GoReferences) {
    auto tree = parse(Language::Go, kGoSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kGoSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // Should have call references (fmt.Sprintf)
    int calls = count_refs(r, ReferenceType::Call);
    EXPECT_GE(calls, 1);
}

TEST(UnifiedExtractorTest, GoFields) {
    auto tree = parse(Language::Go, kGoSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kGoSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Symbol* name_field = find_symbol(r, "Name");
    ASSERT_NE(name_field, nullptr);
    EXPECT_EQ(name_field->type, SymbolType::Field);

    const Symbol* age_field = find_symbol(r, "Age");
    ASSERT_NE(age_field, nullptr);
    EXPECT_EQ(age_field->type, SymbolType::Field);
}

TEST(UnifiedExtractorTest, GoBlocks) {
    auto tree = parse(Language::Go, kGoSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kGoSource, 1, ".go", "main.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const BlockBoundary* add_block = find_block(r, "add");
    ASSERT_NE(add_block, nullptr);
    EXPECT_EQ(add_block->type, BlockType::Function);
}

// ---------------------------------------------------------------------------
// Python extraction tests
// ---------------------------------------------------------------------------

constexpr std::string_view kPythonSource = R"(import os
from pathlib import Path

class Animal:
    def __init__(self, name):
        self.name = name

    def speak(self):
        if self.name:
            return f"I am {self.name}"
        return "..."

class Dog(Animal):
    def speak(self):
        return "Woof!"

def greet(name):
    return f"Hello, {name}"

count = 0
)";

TEST(UnifiedExtractorTest, PythonFunctions) {
    auto tree = parse(Language::Python, kPythonSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kPythonSource, 2, ".py", "animals.py");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Symbol* greet = find_symbol(r, "greet");
    ASSERT_NE(greet, nullptr);
    EXPECT_EQ(greet->type, SymbolType::Function);
}

TEST(UnifiedExtractorTest, PythonClasses) {
    auto tree = parse(Language::Python, kPythonSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kPythonSource, 2, ".py", "animals.py");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Symbol* animal = find_symbol(r, "Animal");
    ASSERT_NE(animal, nullptr);
    EXPECT_EQ(animal->type, SymbolType::Class);

    const Symbol* dog = find_symbol(r, "Dog");
    ASSERT_NE(dog, nullptr);
    EXPECT_EQ(dog->type, SymbolType::Class);
}

TEST(UnifiedExtractorTest, PythonMethods) {
    auto tree = parse(Language::Python, kPythonSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kPythonSource, 2, ".py", "animals.py");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // Methods inside classes should be SymbolType::Method
    int methods = count_symbols(r, SymbolType::Method);
    EXPECT_GE(methods, 3)
        << "Should find __init__, speak (Animal), speak (Dog)";
}

TEST(UnifiedExtractorTest, PythonImports) {
    auto tree = parse(Language::Python, kPythonSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kPythonSource, 2, ".py", "animals.py");
    ue.extract(tree.get());
    auto r = ue.get_results();

    EXPECT_GE(r.imports.size(), 2u);
}

TEST(UnifiedExtractorTest, PythonInheritance) {
    auto tree = parse(Language::Python, kPythonSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kPythonSource, 2, ".py", "animals.py");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // Dog(Animal) should create an extends reference
    bool found_extends = false;
    for (const auto& ref : r.references) {
        if (ref.type == ReferenceType::Extends &&
            ref.referenced_name == "Animal") {
            found_extends = true;
        }
    }
    EXPECT_TRUE(found_extends) << "Should find extends reference to Animal";
}

TEST(UnifiedExtractorTest, PythonComplexity) {
    auto tree = parse(Language::Python, kPythonSource);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(kPythonSource, 2, ".py", "animals.py");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // Animal.speak has an if statement -> complexity >= 2
    // Find it by checking all complexity entries
    bool found_complex = false;
    for (const auto& [key, cx] : r.complexity) {
        if (cx >= 2) found_complex = true;
    }
    EXPECT_TRUE(found_complex) << "Should find at least one function with complexity >= 2";
}

// ---------------------------------------------------------------------------
// Node type caching test
// ---------------------------------------------------------------------------

TEST(UnifiedExtractorTest, NodeTypeCaching) {
    // Parse a small Go file and verify extraction works
    // (node type caching is internal but verified by correct results)
    constexpr std::string_view src = "package main\nfunc hello() {}\n";
    auto tree = parse(Language::Go, src);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(src, 1, ".go", "hello.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Symbol* hello = find_symbol(r, "hello");
    ASSERT_NE(hello, nullptr);
    EXPECT_EQ(hello->type, SymbolType::Function);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(UnifiedExtractorTest, NullTree) {
    UnifiedExtractor ue;
    ue.init("", 0, ".go", "empty.go");
    ue.extract(nullptr);
    auto r = ue.get_results();
    EXPECT_TRUE(r.symbols.empty());
    EXPECT_TRUE(r.blocks.empty());
}

TEST(UnifiedExtractorTest, EmptyFile) {
    auto tree = parse(Language::Go, "package main\n");
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init("package main\n", 1, ".go", "empty.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // Should have file scope at minimum
    EXPECT_FALSE(r.scopes.empty());
}

TEST(UnifiedExtractorTest, ResetClearsState) {
    constexpr std::string_view src = "package main\nfunc hello() {}\n";
    auto tree = parse(Language::Go, src);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(src, 1, ".go", "hello.go");
    ue.extract(tree.get());

    auto r1 = ue.get_results();
    EXPECT_FALSE(r1.symbols.empty());

    ue.reset();
    auto r2 = ue.get_results();
    EXPECT_TRUE(r2.symbols.empty());
    EXPECT_TRUE(r2.scopes.empty());
}

// ---------------------------------------------------------------------------
// TSX extraction tests
// ---------------------------------------------------------------------------

TEST(UnifiedExtractorTest, TsxRoutesToTsxGrammar) {
    Language lang{};
    ASSERT_TRUE(language_from_extension(".tsx", lang));
    EXPECT_EQ(lang, Language::Tsx);
    // .ts stays on the plain TypeScript grammar.
    ASSERT_TRUE(language_from_extension(".ts", lang));
    EXPECT_EQ(lang, Language::TypeScript);
}

TEST(UnifiedExtractorTest, TsxJsxSymbolsExtracted) {
    // JSX is invalid under the plain TypeScript grammar (the `<div>` reads
    // as a type assertion and the parse degrades to ERROR nodes); the TSX
    // grammar parses it cleanly and symbol extraction sees every function.
    constexpr std::string_view src = R"(
function App(): JSX.Element {
    return <div className="app"><span>hello</span></div>;
}

const Button = (props: { label: string }) => <button>{props.label}</button>;

function helper(x: number): number {
    return x * 2;
}
)";
    Language lang{};
    ASSERT_TRUE(language_from_extension(".tsx", lang));
    auto tree = parse(lang, src);
    ASSERT_NE(tree.get(), nullptr);
    EXPECT_FALSE(ts_node_has_error(ts_tree_root_node(tree.get())));

    UnifiedExtractor ue;
    ue.init(src, 1, ".tsx", "app.tsx");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Symbol* app = find_symbol(r, "App");
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->type, SymbolType::Function);
    EXPECT_NE(find_symbol(r, "Button"), nullptr);
    EXPECT_NE(find_symbol(r, "helper"), nullptr);
}

// -- foreign_receiver marking -------------------------------------------------
// A method call through an object that is not self/this must carry
// foreign_receiver so the resolver never links it back to the calling symbol
// (the `size -> size` false-cycle class: DeletedFileTracker::size() calling
// flat_hash_set::size() resolved to itself by same-file name match).

const Reference* find_call_ref(const ExtractionResults& r,
                               std::string_view name_contains) {
    for (const auto& ref : r.references) {
        if (ref.type == ReferenceType::Call &&
            ref.referenced_name.find(name_contains) != std::string::npos)
            return &ref;
    }
    return nullptr;
}

TEST(UnifiedExtractorTest, CppMemberCallThroughFieldIsForeignReceiver) {
    constexpr std::string_view src = R"(
struct Tracker {
    Store files;
    int size() const { return files.size(); }
    int total() const { return size(); }
};
)";
    Language lang{};
    ASSERT_TRUE(language_from_extension(".cpp", lang));
    auto tree = parse(lang, src);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(src, 1, ".cpp", "tracker.cpp");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // files.size(): explicit non-self receiver -> foreign.
    const Reference* member_call = find_call_ref(r, "size");
    ASSERT_NE(member_call, nullptr);
    EXPECT_TRUE(member_call->foreign_receiver);

    // size() bare call: no receiver -> not foreign (stays resolvable to the
    // sibling method / genuine recursion).
    bool found_bare = false;
    for (const auto& ref : r.references) {
        if (ref.type == ReferenceType::Call &&
            ref.referenced_name == "size" && !ref.foreign_receiver)
            found_bare = true;
    }
    EXPECT_TRUE(found_bare);
}

TEST(UnifiedExtractorTest, GoDirectRecursionIsNotForeignReceiver) {
    constexpr std::string_view src = R"(
package main

func fact(n int) int {
    if n <= 1 { return 1 }
    return fact(n - 1)
}
)";
    auto tree = parse(Language::Go, src);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(src, 1, ".go", "fact.go");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Reference* rec = find_call_ref(r, "fact");
    ASSERT_NE(rec, nullptr);
    EXPECT_FALSE(rec->foreign_receiver);
}

TEST(UnifiedExtractorTest, PhpScopedCallsEmitReferences) {
    // self::/static:: qualify to the enclosing class (recursion stays
    // linkable); parent:: is a foreign receiver; Foo:: qualifies to Foo.
    // scoped_call_expression previously emitted NO reference at all.
    constexpr std::string_view src = R"(<?php
class Norm {
  public static function flatten($v) {
    return self::flatten($v);
  }
  public function run() {
    parent::run();
    Other::helper();
  }
}
)";
    Language lang{};
    ASSERT_TRUE(language_from_extension(".php", lang));
    auto tree = parse(lang, src);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(src, 1, ".php", "norm.php");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Reference* self_call = find_call_ref(r, "flatten");
    ASSERT_NE(self_call, nullptr);
    EXPECT_FALSE(self_call->foreign_receiver);
    EXPECT_EQ(self_call->referenced_name, "Norm.flatten");

    const Reference* parent_call = find_call_ref(r, "run");
    ASSERT_NE(parent_call, nullptr);
    EXPECT_TRUE(parent_call->foreign_receiver);

    const Reference* scoped = find_call_ref(r, "helper");
    ASSERT_NE(scoped, nullptr);
    EXPECT_EQ(scoped->referenced_name, "Other.helper");
    // A class other than the enclosing one is a foreign receiver: without
    // this, Psr7\Utils::modifyRequest inside RedirectMiddleware's own
    // modifyRequest fell back to a bare-name self-match (fake recursion).
    EXPECT_TRUE(scoped->foreign_receiver);
}

TEST(UnifiedExtractorTest, PhpScopedCallToOtherClassSameNameIsForeign) {
    constexpr std::string_view src = R"(<?php
class RedirectMiddleware {
  public function modifyRequest($request) {
    return Utils::modifyRequest($request);
  }
}
)";
    Language lang{};
    ASSERT_TRUE(language_from_extension(".php", lang));
    auto tree = parse(lang, src);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(src, 1, ".php", "rm.php");
    ue.extract(tree.get());
    auto r = ue.get_results();

    const Reference* call = find_call_ref(r, "modifyRequest");
    ASSERT_NE(call, nullptr);
    EXPECT_TRUE(call->foreign_receiver);
    EXPECT_EQ(call->referenced_name, "Utils.modifyRequest");
}

TEST(UnifiedExtractorTest, CsharpThisCallIsNotForeignReceiver) {
    constexpr std::string_view src = R"(
class Svc {
    void Run() { this.Step(); other.Step(); }
    void Step() {}
    Svc other;
}
)";
    Language lang{};
    ASSERT_TRUE(language_from_extension(".cs", lang));
    auto tree = parse(lang, src);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(src, 1, ".cs", "svc.cs");
    ue.extract(tree.get());
    auto r = ue.get_results();

    bool this_call_not_foreign = false, other_call_foreign = false;
    for (const auto& ref : r.references) {
        if (ref.type != ReferenceType::Call) continue;
        if (ref.referenced_name.find("Step") == std::string::npos) continue;
        if (ref.foreign_receiver)
            other_call_foreign = true;
        else
            this_call_not_foreign = true;
    }
    EXPECT_TRUE(this_call_not_foreign);
    EXPECT_TRUE(other_call_foreign);
}

}  // namespace
}  // namespace lci::parser
