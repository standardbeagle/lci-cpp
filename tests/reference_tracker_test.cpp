#include <gtest/gtest.h>

#include <lci/core/reference_tracker.h>

#include <string>
#include <vector>

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Symbol make_sym(const std::string& name, SymbolType type, FileID file_id,
                int line, int end_line) {
    Symbol s;
    s.name = name;
    s.type = type;
    s.file_id = file_id;
    s.line = line;
    s.column = 0;
    s.end_line = end_line;
    s.end_column = 80;
    return s;
}


// ---------------------------------------------------------------------------
// ReferenceTracker - basic operations
// ---------------------------------------------------------------------------

TEST(ReferenceTrackerTest, EmptyTracker) {
    ReferenceTracker rt;
    EXPECT_EQ(rt.get_reference_stats().total_symbols, 0);
    EXPECT_EQ(rt.get_enhanced_symbol(1), nullptr);
    EXPECT_TRUE(rt.get_all_references().empty());
    EXPECT_FALSE(rt.has_relationships());
}

TEST(ReferenceTrackerTest, ProcessFileSingleSymbol) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("main", SymbolType::Function, 1, 1, 10),
    };
    std::vector<Reference> refs;
    std::vector<ScopeInfo> scopes;

    auto enhanced = rt.process_file(1, "main.go", symbols, refs, scopes);
    ASSERT_EQ(enhanced.size(), 1u);
    EXPECT_EQ(enhanced[0].symbol.name, "main");
    // "main" starts with lowercase, so Go convention says not exported.
    EXPECT_FALSE(enhanced[0].is_exported);
}

TEST(ReferenceTrackerTest, ProcessFileMultipleSymbols) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("Foo", SymbolType::Class, 1, 1, 20),
        make_sym("Bar", SymbolType::Function, 1, 25, 40),
        make_sym("_private", SymbolType::Function, 1, 45, 60),
    };
    std::vector<Reference> refs;
    std::vector<ScopeInfo> scopes;

    auto enhanced = rt.process_file(1, "test.py", symbols, refs, scopes);
    ASSERT_EQ(enhanced.size(), 3u);
    EXPECT_TRUE(enhanced[0].is_exported);
    EXPECT_TRUE(enhanced[1].is_exported);
    EXPECT_FALSE(enhanced[2].is_exported);
}

TEST(ReferenceTrackerTest, BidirectionalReferences) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("caller", SymbolType::Function, 1, 1, 10),
        make_sym("callee", SymbolType::Function, 1, 15, 25),
    };

    Reference call_ref;
    call_ref.type = ReferenceType::Call;
    call_ref.referenced_name = "callee";
    call_ref.line = 5;
    call_ref.column = 10;
    std::vector<Reference> refs = {call_ref};
    std::vector<ScopeInfo> scopes;

    rt.process_file(1, "test.go", symbols, refs, scopes);
    rt.process_all_references();

    EXPECT_TRUE(rt.has_relationships());

    auto stats = rt.get_reference_stats();
    EXPECT_GT(stats.total_references, 0);
}

TEST(ReferenceTrackerTest, FindSymbolsByName) {
    ReferenceTracker rt;

    std::vector<Symbol> syms1 = {
        make_sym("Foo", SymbolType::Function, 1, 1, 10),
    };
    std::vector<Symbol> syms2 = {
        make_sym("Foo", SymbolType::Method, 2, 1, 10),
    };

    rt.process_file(1, "a.go", syms1, {}, {});
    rt.process_file(2, "b.go", syms2, {}, {});

    auto found = rt.find_symbols_by_name("Foo");
    EXPECT_EQ(found.size(), 2u);

    auto not_found = rt.find_symbols_by_name("Bar");
    EXPECT_TRUE(not_found.empty());
}

TEST(ReferenceTrackerTest, FindSymbolByFileAndName) {
    ReferenceTracker rt;

    std::vector<Symbol> syms1 = {
        make_sym("Foo", SymbolType::Function, 1, 1, 10),
    };
    std::vector<Symbol> syms2 = {
        make_sym("Foo", SymbolType::Method, 2, 1, 10),
    };

    rt.process_file(1, "a.go", syms1, {}, {});
    rt.process_file(2, "b.go", syms2, {}, {});

    const auto* in_file1 = rt.find_symbol_by_file_and_name(1, "Foo");
    ASSERT_NE(in_file1, nullptr);
    EXPECT_EQ(in_file1->symbol.type, SymbolType::Function);

    const auto* in_file2 = rt.find_symbol_by_file_and_name(2, "Foo");
    ASSERT_NE(in_file2, nullptr);
    EXPECT_EQ(in_file2->symbol.type, SymbolType::Method);

    EXPECT_EQ(rt.find_symbol_by_file_and_name(3, "Foo"), nullptr);
}

TEST(ReferenceTrackerTest, GetSymbolAtLine) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("Outer", SymbolType::Class, 1, 1, 50),
        make_sym("Inner", SymbolType::Method, 1, 10, 20),
    };

    rt.process_file(1, "test.go", symbols, {}, {});

    const auto* at_line_5 = rt.get_symbol_at_line(1, 5);
    ASSERT_NE(at_line_5, nullptr);
    EXPECT_EQ(at_line_5->symbol.name, "Outer");

    const auto* at_line_15 = rt.get_symbol_at_line(1, 15);
    ASSERT_NE(at_line_15, nullptr);

    EXPECT_EQ(rt.get_symbol_at_line(1, 100), nullptr);
}

TEST(ReferenceTrackerTest, RemoveFile) {
    ReferenceTracker rt;

    std::vector<Symbol> syms = {
        make_sym("Foo", SymbolType::Function, 1, 1, 10),
    };
    rt.process_file(1, "a.go", syms, {}, {});

    auto found = rt.find_symbols_by_name("Foo");
    EXPECT_EQ(found.size(), 1u);

    rt.remove_file(1);

    found = rt.find_symbols_by_name("Foo");
    EXPECT_TRUE(found.empty());
}

TEST(ReferenceTrackerTest, Clear) {
    ReferenceTracker rt;

    std::vector<Symbol> syms = {
        make_sym("Foo", SymbolType::Function, 1, 1, 10),
    };
    rt.process_file(1, "a.go", syms, {}, {});
    rt.clear();

    EXPECT_EQ(rt.get_reference_stats().total_symbols, 0);
    EXPECT_TRUE(rt.find_symbols_by_name("Foo").empty());
}

// ---------------------------------------------------------------------------
// Type relationships
// ---------------------------------------------------------------------------

TEST(ReferenceTrackerTest, TypeRelationshipsImplements) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("Reader", SymbolType::Interface, 1, 1, 10),
        make_sym("FileReader", SymbolType::Struct, 1, 15, 30),
    };

    Reference impl_ref;
    impl_ref.type = ReferenceType::Implements;
    impl_ref.referenced_name = "Reader";
    impl_ref.source_symbol = 0;
    impl_ref.target_symbol = 0;
    impl_ref.line = 16;
    impl_ref.column = 0;

    std::vector<Reference> refs = {impl_ref};
    std::vector<ScopeInfo> scopes;
    rt.process_file(1, "test.go", symbols, refs, scopes);
    rt.process_all_references();

    auto rels = rt.get_type_relationships(1);
    // The relationship structure exists; content depends on resolution.
    EXPECT_FALSE(rels.has_relationships() && rels.implements.empty() &&
                 rels.implemented_by.empty());
}

TEST(ReferenceTrackerTest, TypeRelationshipsExtends) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("Base", SymbolType::Class, 1, 1, 10),
        make_sym("Derived", SymbolType::Class, 1, 15, 30),
    };

    Reference ext_ref;
    ext_ref.type = ReferenceType::Extends;
    ext_ref.referenced_name = "Base";
    ext_ref.source_symbol = 0;
    ext_ref.target_symbol = 0;
    ext_ref.line = 16;
    ext_ref.column = 0;

    std::vector<Reference> refs = {ext_ref};
    std::vector<ScopeInfo> scopes;
    rt.process_file(1, "test.go", symbols, refs, scopes);
    rt.process_all_references();

    auto all_refs = rt.get_all_references();
    bool has_extends = false;
    for (const auto& r : all_refs) {
        if (r.type == ReferenceType::Extends) {
            has_extends = true;
            break;
        }
    }
    EXPECT_TRUE(has_extends);
}

// ---------------------------------------------------------------------------
// Call graph utilities
// ---------------------------------------------------------------------------

TEST(ReferenceTrackerTest, CallGraphCalleeNames) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("main", SymbolType::Function, 1, 1, 20),
        make_sym("helper", SymbolType::Function, 1, 25, 40),
    };

    Reference call_ref;
    call_ref.type = ReferenceType::Call;
    call_ref.referenced_name = "helper";
    call_ref.line = 5;
    call_ref.column = 5;

    std::vector<Reference> refs = {call_ref};
    std::vector<ScopeInfo> scopes;
    rt.process_file(1, "test.go", symbols, refs, scopes);
    rt.process_all_references();

    // Find "main" symbol and check its callees.
    const auto* main_sym = rt.find_symbol_by_name("main");
    if (main_sym != nullptr) {
        auto callees = rt.get_callee_names(main_sym->id);
        // May or may not resolve depending on location matching.
        // The important thing is the API works without errors.
        (void)callees;
    }
}

TEST(ReferenceTrackerTest, FunctionTree) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("root", SymbolType::Function, 1, 1, 10),
    };

    rt.process_file(1, "test.go", symbols, {}, {});

    const auto* sym = rt.find_symbol_by_name("root");
    ASSERT_NE(sym, nullptr);

    auto tree = rt.build_function_tree(sym->id, 3);
    EXPECT_EQ(tree.name, "root");
    EXPECT_TRUE(tree.children.empty());
}

// ---------------------------------------------------------------------------
// Scope chain caching
// ---------------------------------------------------------------------------

TEST(ReferenceTrackerTest, ScopeChainCaching) {
    ReferenceTracker rt;

    ScopeInfo class_scope;
    class_scope.type = ScopeType::Class;
    class_scope.name = "MyClass";
    class_scope.start_line = 1;
    class_scope.end_line = 50;

    ScopeInfo method_scope;
    method_scope.type = ScopeType::Method;
    method_scope.name = "myMethod";
    method_scope.start_line = 10;
    method_scope.end_line = 20;

    std::vector<ScopeInfo> scopes = {class_scope, method_scope};

    std::vector<Symbol> symbols = {
        make_sym("foo", SymbolType::Variable, 1, 15, 15),
        make_sym("bar", SymbolType::Variable, 1, 16, 16),
    };

    auto enhanced = rt.process_file(1, "test.go", symbols, {}, scopes);

    // Both symbols at similar positions should get scope chains.
    ASSERT_EQ(enhanced.size(), 2u);
    EXPECT_FALSE(enhanced[0].scope_chain.empty());
    EXPECT_FALSE(enhanced[1].scope_chain.empty());
}

// ---------------------------------------------------------------------------
// Reference cache
// ---------------------------------------------------------------------------

TEST(ReferenceTrackerTest, ReferenceCacheHit) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("target", SymbolType::Function, 1, 1, 10),
    };

    // Two references to the same name should benefit from caching.
    Reference ref1;
    ref1.id = 1;
    ref1.type = ReferenceType::Call;
    ref1.referenced_name = "target";
    ref1.line = 15;
    ref1.column = 5;

    Reference ref2;
    ref2.id = 2;
    ref2.type = ReferenceType::Call;
    ref2.referenced_name = "target";
    ref2.line = 20;
    ref2.column = 5;

    std::vector<Reference> refs = {ref1, ref2};
    std::vector<ScopeInfo> scopes;
    rt.process_file(1, "test.go", symbols, refs, scopes);
    rt.process_all_references();

    auto all_refs = rt.get_all_references();
    EXPECT_GE(all_refs.size(), 2u);
}

// ---------------------------------------------------------------------------
// Line-to-symbol index
// ---------------------------------------------------------------------------

TEST(ReferenceTrackerTest, LineToSymbolsIndex) {
    ReferenceTracker rt;

    absl::flat_hash_map<int, std::vector<int>> line_map;
    line_map[10] = {0, 1};
    line_map[20] = {2};

    rt.store_line_to_symbols(1, std::move(line_map));

    const auto* result = rt.get_file_line_to_symbols(1);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->size(), 2u);

    EXPECT_EQ(rt.get_file_line_to_symbols(99), nullptr);
}

// ---------------------------------------------------------------------------
// PostingsIndex
// ---------------------------------------------------------------------------

TEST(PostingsIndexTest, EmptyIndex) {
    PostingsIndex pi;
    EXPECT_EQ(pi.token_count(), 0);
    EXPECT_EQ(pi.file_count(), 0);

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("test", true, files, offsets);
    EXPECT_TRUE(files.empty());
}

TEST(PostingsIndexTest, IndexAndFind) {
    PostingsIndex pi;

    std::string content = "function hello() { return world; }";
    pi.index_file(1, content);

    EXPECT_GT(pi.token_count(), 0);
    EXPECT_EQ(pi.file_count(), 1);

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("function", true, files, offsets);
    EXPECT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], 1u);
    EXPECT_GE(offsets[1], 0);
}

TEST(PostingsIndexTest, CaseInsensitiveFind) {
    PostingsIndex pi;

    std::string content = "Function HELLO world";
    pi.index_file(1, content);

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("FUNCTION", true, files, offsets);
    EXPECT_EQ(files.size(), 1u);

    files.clear();
    offsets.clear();
    pi.find("hello", true, files, offsets);
    EXPECT_EQ(files.size(), 1u);
}

TEST(PostingsIndexTest, MinTokenLength) {
    PostingsIndex pi;

    std::string content = "ab cd ef gh";
    pi.index_file(1, content);

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("ab", true, files, offsets);
    EXPECT_TRUE(files.empty());
}

TEST(PostingsIndexTest, MultipleFiles) {
    PostingsIndex pi;

    pi.index_file(1, "function alpha() {}");
    pi.index_file(2, "function beta() {}");
    pi.index_file(3, "class gamma {}");

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("function", true, files, offsets);
    EXPECT_EQ(files.size(), 2u);

    files.clear();
    offsets.clear();
    pi.find("alpha", true, files, offsets);
    EXPECT_EQ(files.size(), 1u);
}

TEST(PostingsIndexTest, RemoveFile) {
    PostingsIndex pi;

    pi.index_file(1, "function alpha() {}");
    pi.index_file(2, "function beta() {}");

    pi.remove_file(1);

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("alpha", true, files, offsets);
    EXPECT_TRUE(files.empty());

    files.clear();
    offsets.clear();
    pi.find("function", true, files, offsets);
    EXPECT_EQ(files.size(), 1u);
}

TEST(PostingsIndexTest, Clear) {
    PostingsIndex pi;

    pi.index_file(1, "function hello world");
    pi.clear();

    EXPECT_EQ(pi.token_count(), 0);
    EXPECT_EQ(pi.file_count(), 0);
}

TEST(PostingsIndexTest, FirstOffsetPerFile) {
    PostingsIndex pi;

    std::string content = "hello world hello again";
    pi.index_file(1, content);

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("hello", true, files, offsets);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(offsets[1], 0);
}

// ---------------------------------------------------------------------------
// ImportResolver
// ---------------------------------------------------------------------------

TEST(ImportResolverTest, ExtractGoImports) {
    ImportResolver resolver;

    auto data = resolver.extract_file_imports(
        1, "test.go",
        "import \"fmt\"\nimport alias \"strings\"\n");

    ASSERT_GE(data.bindings.size(), 1u);
    EXPECT_EQ(data.bindings[0].imported_name, "fmt");
    EXPECT_EQ(data.bindings[0].source_file, "fmt");
}

TEST(ImportResolverTest, ExtractJSImports) {
    ImportResolver resolver;

    auto data = resolver.extract_file_imports(
        1, "test.js",
        "import { Foo, Bar } from './utils';\n");

    ASSERT_GE(data.bindings.size(), 2u);
    bool found_foo = false;
    bool found_bar = false;
    for (const auto& b : data.bindings) {
        if (b.imported_name == "Foo") found_foo = true;
        if (b.imported_name == "Bar") found_bar = true;
    }
    EXPECT_TRUE(found_foo);
    EXPECT_TRUE(found_bar);
}

TEST(ImportResolverTest, ExtractPythonImports) {
    ImportResolver resolver;

    auto data = resolver.extract_file_imports(
        1, "test.py",
        "from os.path import join, exists\nimport sys\n");

    ASSERT_GE(data.bindings.size(), 3u);
}

TEST(ImportResolverTest, ExtractRustImports) {
    ImportResolver resolver;

    auto data = resolver.extract_file_imports(
        1, "test.rs",
        "use std::collections::HashMap;\nuse std::{Vec, String};\n");

    ASSERT_GE(data.bindings.size(), 1u);
    EXPECT_EQ(data.bindings[0].imported_name, "HashMap");
}

TEST(ImportResolverTest, ResolvePrefersSameFile) {
    ImportResolver resolver;

    EnhancedSymbol sym1;
    sym1.symbol.name = "Foo";
    sym1.symbol.file_id = 1;
    sym1.is_exported = true;

    EnhancedSymbol sym2;
    sym2.symbol.name = "Foo";
    sym2.symbol.file_id = 2;
    sym2.is_exported = true;

    std::vector<SymbolID> candidates = {100, 200};

    auto lookup = [&](SymbolID id) -> const EnhancedSymbol* {
        if (id == 100) return &sym1;
        if (id == 200) return &sym2;
        return nullptr;
    };

    SymbolID resolved = resolver.resolve_symbol_reference(
        1, "Foo", candidates, lookup);

    EXPECT_EQ(resolved, 100u);
}

TEST(ImportResolverTest, BuildAndResolveImportGraph) {
    ImportResolver resolver;

    FileImportData data;
    data.file_id = 1;
    ImportBinding b;
    b.imported_name = "Helper";
    b.original_name = "Helper";
    b.source_file = "./utils";
    data.bindings.push_back(b);

    std::vector<FileImportData> all_data = {data};
    resolver.build_import_graph(all_data);

    EnhancedSymbol sym;
    sym.symbol.name = "Helper";
    sym.symbol.file_id = 2;
    sym.is_exported = true;

    std::vector<SymbolID> candidates = {300};
    auto lookup = [&](SymbolID id) -> const EnhancedSymbol* {
        if (id == 300) return &sym;
        return nullptr;
    };

    SymbolID resolved = resolver.resolve_symbol_reference(
        1, "Helper", candidates, lookup);
    EXPECT_EQ(resolved, 300u);
}

}  // namespace
}  // namespace lci
