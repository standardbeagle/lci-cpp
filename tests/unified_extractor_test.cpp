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
// Object pool tests
// ---------------------------------------------------------------------------

TEST(ExtractorPoolTest, AcquireRelease) {
    auto& pool = thread_extractor_pool();

    auto* ue = pool.acquire("content", 1, ".go", "test.go");
    ASSERT_NE(ue, nullptr);

    ue->extract(nullptr);  // null tree is safe
    pool.release(ue);

    // Re-acquire should give back the same (or a recycled) extractor
    auto* ue2 = pool.acquire("content2", 2, ".py", "test.py");
    ASSERT_NE(ue2, nullptr);
    pool.release(ue2);
}

TEST(ExtractorPoolTest, RecyclesInstances) {
    auto& pool = thread_extractor_pool();

    auto* ue1 = pool.acquire("a", 1, ".go", "a.go");
    pool.release(ue1);

    auto* ue2 = pool.acquire("b", 2, ".py", "b.py");
    EXPECT_EQ(ue1, ue2) << "Pool should recycle the same extractor";
    pool.release(ue2);
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

}  // namespace
}  // namespace lci::parser
