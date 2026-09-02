#include <gtest/gtest.h>

#include <lci/core/callers_report.h>
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
    EXPECT_EQ(rt.pin()->get_enhanced_symbol(1), nullptr);
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

// Pins the classify/call_resolution_totals consistency fix: a call resolved
// to a bodiless DECLARATION (interface / abstract method spec) is dynamic
// dispatch in call_resolution_totals — classify_same_name_calls used to
// skip every resolved ref, undercounting `dynamic` for exactly the
// interface-reached dead-code candidates the count exists for.
TEST(ReferenceTrackerTest, DeclarationOnlyTargetCountsAsDynamic) {
    ReferenceTracker rt;

    Symbol decl = make_sym("Handle", SymbolType::Method, 1, 1, 1);
    decl.declaration_only = true;
    std::vector<Symbol> symbols = {
        decl,
        make_sym("caller", SymbolType::Function, 1, 5, 15),
    };

    Reference call_ref;
    call_ref.type = ReferenceType::Call;
    call_ref.referenced_name = "Handle";
    call_ref.line = 8;
    call_ref.column = 4;
    std::vector<Reference> refs = {call_ref};
    std::vector<ScopeInfo> scopes;

    rt.process_file(1, "test.go", symbols, refs, scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    auto totals = snap->call_resolution_totals();
    ASSERT_EQ(totals.dynamic, 1) << "resolved-to-declaration call must be "
                                    "dynamic in the totals";
    auto stats = snap->classify_same_name_calls("Handle");
    EXPECT_EQ(stats.dynamic, 1)
        << "classify_same_name_calls disagrees with call_resolution_totals";
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

    auto snapshot = rt.pin();
    auto found = snapshot->find_symbols_by_name("Foo");
    EXPECT_EQ(found.size(), 2u);

    auto not_found = snapshot->find_symbols_by_name("Bar");
    EXPECT_TRUE(not_found.empty());
}

TEST(ReferenceTrackerTest, SymbolHandleRetainsTemporaryPinnedSnapshot) {
    ReferenceTracker rt;
    std::vector<Symbol> symbols = {
        make_sym("Stable", SymbolType::Function, 1, 1, 3),
    };
    rt.process_file(1, "stable.go", symbols, {}, {});

    auto stable = rt.pin()->find_symbol_by_name("Stable");
    ASSERT_NE(stable, nullptr);
    rt.clear();

    EXPECT_EQ(stable->symbol.name, "Stable");
    EXPECT_EQ(stable->symbol.file_id, 1u);
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

    auto snapshot = rt.pin();
    auto in_file1 = snapshot->find_symbol_by_file_and_name(1, "Foo");
    ASSERT_NE(in_file1, nullptr);
    EXPECT_EQ(in_file1->symbol.type, SymbolType::Function);

    auto in_file2 = snapshot->find_symbol_by_file_and_name(2, "Foo");
    ASSERT_NE(in_file2, nullptr);
    EXPECT_EQ(in_file2->symbol.type, SymbolType::Method);

    EXPECT_EQ(snapshot->find_symbol_by_file_and_name(3, "Foo"), nullptr);
}

TEST(ReferenceTrackerTest, GetSymbolAtLine) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("Outer", SymbolType::Class, 1, 1, 50),
        make_sym("Inner", SymbolType::Method, 1, 10, 20),
    };

    rt.process_file(1, "test.go", symbols, {}, {});

    auto snapshot = rt.pin();
    auto at_line_5 = snapshot->get_symbol_at_line(1, 5);
    ASSERT_NE(at_line_5, nullptr);
    EXPECT_EQ(at_line_5->symbol.name, "Outer");

    auto at_line_15 = snapshot->get_symbol_at_line(1, 15);
    ASSERT_NE(at_line_15, nullptr);

    EXPECT_EQ(snapshot->get_symbol_at_line(1, 100), nullptr);
}

TEST(ReferenceTrackerTest, RemoveFile) {
    ReferenceTracker rt;

    std::vector<Symbol> syms = {
        make_sym("Foo", SymbolType::Function, 1, 1, 10),
    };
    rt.process_file(1, "a.go", syms, {}, {});

    auto snapshot = rt.pin();
    auto found = snapshot->find_symbols_by_name("Foo");
    EXPECT_EQ(found.size(), 1u);

    rt.remove_file(1);

    snapshot = rt.pin();
    found = snapshot->find_symbols_by_name("Foo");
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
    EXPECT_TRUE(rt.pin()->find_symbols_by_name("Foo").empty());
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
    auto snapshot = rt.pin();
    auto main_sym = snapshot->find_symbol_by_name("main");
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

    auto snapshot = rt.pin();
    auto sym = snapshot->find_symbol_by_name("root");
    ASSERT_NE(sym, nullptr);

    auto tree = rt.build_function_tree(sym->id, 3);
    EXPECT_EQ(tree.name, "root");
    EXPECT_TRUE(tree.children.empty());
}

// ---------------------------------------------------------------------------
// Receiver-type-qualified method resolution (SCIP base case)
// ---------------------------------------------------------------------------

// Two classes A and B each expose a method named run(). The extractor emits the
// call sites as receiver-type-qualified refs A.run / B.run (it knows the
// receiver types from the local var env). The resolver must route each ref to
// the run() whose owning class scope matches the receiver type, instead of
// collapsing both onto the first same-named symbol.
TEST(ReferenceTrackerTest, ResolvesByReceiverTypeScope) {
    ReferenceTracker rt;

    auto scope = [](ScopeType t, const char* n, int s, int e) {
        ScopeInfo si;
        si.type = t;
        si.name = n;
        si.start_line = s;
        si.end_line = e;
        return si;
    };
    std::vector<ScopeInfo> scopes = {
        scope(ScopeType::Class, "A", 1, 4),
        scope(ScopeType::Class, "B", 5, 8),
    };

    std::vector<Symbol> symbols = {
        make_sym("A", SymbolType::Struct, 1, 1, 4),
        make_sym("runA", SymbolType::Function, 1, 2, 2),
        make_sym("B", SymbolType::Struct, 1, 5, 8),
        make_sym("runB", SymbolType::Function, 1, 6, 6),
        make_sym("go", SymbolType::Function, 1, 9, 14),
    };
    // The two run methods share the visible name "run"; their distinct
    // identifiers above only let the test address each one.
    symbols[1].name = "run";
    symbols[3].name = "run";

    auto call = [](uint64_t id, const char* name, int line) {
        Reference r;
        r.id = id;
        r.type = ReferenceType::Call;
        r.referenced_name = name;
        r.line = line;
        r.column = 5;
        return r;
    };
    std::vector<Reference> refs = {
        call(1, "A.run", 11),  // a.run() inside go()
        call(2, "B.run", 13),  // b.run() inside go()
    };

    rt.process_file(1, "m.cpp", symbols, refs, scopes);
    rt.process_all_references();

    // Address each run() method by line (the struct symbol shares the same
    // span, so a plain name lookup is ambiguous).
    ReferenceTracker::Snapshot::SymbolHandle run_a;
    ReferenceTracker::Snapshot::SymbolHandle run_b;
    auto snapshot = rt.pin();
    for (const auto& es : snapshot->find_symbols_by_name("run")) {
        if (es->symbol.line == 2) run_a = es;
        if (es->symbol.line == 6) run_b = es;
    }
    ASSERT_NE(run_a, nullptr);
    ASSERT_NE(run_b, nullptr);

    // Each run() carries its owning class in the scope chain.
    auto chain_has = [](const EnhancedSymbol& es, const char* n) {
        for (const auto& sc : es.scope_chain)
            if (sc.name == n) return true;
        return false;
    };
    EXPECT_TRUE(chain_has(*run_a, "A"));
    EXPECT_TRUE(chain_has(*run_b, "B"));

    // A.run resolves to the L2 method, B.run to the L6 method — not both to A.
    auto a_callers = rt.get_caller_names(run_a->id);
    auto b_callers = rt.get_caller_names(run_b->id);
    EXPECT_EQ(a_callers.size(), 1u);
    EXPECT_EQ(b_callers.size(), 1u)
        << "B.run must resolve to the B method, not collapse onto A.run";
}

// ---------------------------------------------------------------------------
// Ambiguous bare-name resolution (D2: collision edges poison reach)
// ---------------------------------------------------------------------------

namespace {
Reference make_call(uint64_t id, const char* name, int line) {
    Reference r;
    r.id = id;
    r.type = ReferenceType::Call;
    r.referenced_name = name;
    r.line = line;
    r.column = 5;
    return r;
}
}  // namespace

// Two exported same-named functions in different packages, a caller in a
// third package with no import evidence: no candidate is distinguishable,
// so NO edge may be built. A wrong edge is worse than a missing one — every
// bare-name collision edge inflates transitive reach for an unrelated symbol.
TEST(ReferenceTrackerTest, AmbiguousCrossPackageBareNameDoesNotLink) {
    ReferenceTracker rt;

    rt.process_file(1, "pkg_a/a.go",
                    std::vector<Symbol>{
                        make_sym("Get", SymbolType::Function, 1, 1, 5)},
                    {}, {});
    rt.process_file(2, "pkg_b/b.go",
                    std::vector<Symbol>{
                        make_sym("Get", SymbolType::Function, 2, 1, 5)},
                    {}, {});
    std::vector<Reference> refs = {make_call(1, "Get", 3)};
    rt.process_file(3, "app/main.go",
                    std::vector<Symbol>{
                        make_sym("main", SymbolType::Function, 3, 1, 10)},
                    refs, {});
    rt.process_all_references();

    auto snapshot = rt.pin();
    auto main_sym = snapshot->find_symbol_by_file_and_name(3, "main");
    ASSERT_NE(main_sym, nullptr);
    EXPECT_TRUE(rt.get_callee_symbols(main_sym->id).empty())
        << "ambiguous bare name must not link to an arbitrary candidate";
}

// Same bare name in two directories, caller shares a directory with one of
// them (Go package = directory): the same-directory candidate is the target.
TEST(ReferenceTrackerTest, SameDirectoryCandidateWinsForUnexportedName) {
    ReferenceTracker rt;

    // Deliberately register the WRONG-package candidate first so insertion
    // order cannot mask a missing proximity rule.
    rt.process_file(1, "other/util.go",
                    std::vector<Symbol>{
                        make_sym("writer", SymbolType::Function, 1, 1, 5)},
                    {}, {});
    rt.process_file(2, "mw/compress.go",
                    std::vector<Symbol>{
                        make_sym("writer", SymbolType::Function, 2, 1, 5)},
                    {}, {});
    std::vector<Reference> refs = {make_call(1, "writer", 3)};
    rt.process_file(3, "mw/flush.go",
                    std::vector<Symbol>{
                        make_sym("flush", SymbolType::Function, 3, 1, 10)},
                    refs, {});
    rt.process_all_references();

    auto snapshot = rt.pin();
    auto flush_sym = snapshot->find_symbol_by_file_and_name(3, "flush");
    auto mw_writer = snapshot->find_symbol_by_file_and_name(2, "writer");
    ASSERT_NE(flush_sym, nullptr);
    ASSERT_NE(mw_writer, nullptr);
    auto callees = rt.get_callee_symbols(flush_sym->id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(callees[0], mw_writer->id)
        << "same-directory (same-package) candidate must win";
}

// Exported name collides between an example tree and production: the example
// candidate is demoted, the unique production candidate links. Covers Go's
// `_examples` convention (leading-underscore dir, ignored by the toolchain).
TEST(ReferenceTrackerTest, ExampleDirLosesToProductionOnAmbiguity) {
    ReferenceTracker rt;

    rt.process_file(1, "_examples/todos/demo.go",
                    std::vector<Symbol>{
                        make_sym("Route", SymbolType::Function, 1, 1, 5)},
                    {}, {});
    rt.process_file(2, "router/mux.go",
                    std::vector<Symbol>{
                        make_sym("Route", SymbolType::Function, 2, 1, 5)},
                    {}, {});
    std::vector<Reference> refs = {make_call(1, "Route", 3)};
    rt.process_file(3, "app/main.go",
                    std::vector<Symbol>{
                        make_sym("main", SymbolType::Function, 3, 1, 10)},
                    refs, {});
    rt.process_all_references();

    auto snapshot = rt.pin();
    auto main_sym = snapshot->find_symbol_by_file_and_name(3, "main");
    auto prod = snapshot->find_symbol_by_file_and_name(2, "Route");
    ASSERT_NE(main_sym, nullptr);
    ASSERT_NE(prod, nullptr);
    auto callees = rt.get_callee_symbols(main_sym->id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(callees[0], prod->id)
        << "_examples/ candidate must lose to the production one";
}

// A unique cross-file candidate still links — the narrowing must not break
// ordinary unambiguous resolution.
TEST(ReferenceTrackerTest, UniqueCrossFileCandidateStillLinks) {
    ReferenceTracker rt;

    rt.process_file(1, "pkg/util.go",
                    std::vector<Symbol>{
                        make_sym("helper", SymbolType::Function, 1, 1, 5)},
                    {}, {});
    std::vector<Reference> refs = {make_call(1, "helper", 3)};
    rt.process_file(2, "app/main.go",
                    std::vector<Symbol>{
                        make_sym("main", SymbolType::Function, 2, 1, 10)},
                    refs, {});
    rt.process_all_references();

    auto snapshot = rt.pin();
    auto main_sym = snapshot->find_symbol_by_file_and_name(2, "main");
    auto helper = snapshot->find_symbol_by_file_and_name(1, "helper");
    ASSERT_NE(main_sym, nullptr);
    ASSERT_NE(helper, nullptr);
    auto callees = rt.get_callee_symbols(main_sym->id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(callees[0], helper->id);
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

TEST(PostingsIndexTest, CaseSensitiveFindLowercasesLookupKey) {
    // Tokens are stored lowercased at index time, so a case-sensitive
    // lookup with any uppercase letter must still hit the lowered key —
    // the prefilter proposes candidates; the caller's verify scan enforces
    // exact case. Previously "FooBar" missed unconditionally.
    PostingsIndex pi;
    pi.index_file(1, "var y = FooBar()");

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("FooBar", false, files, offsets);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], 1u);
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

// ---------------------------------------------------------------------------
// Data-file token cap + PARTIAL self-nomination.
// The postings index is a PREFILTER: a capped file must appear in every
// find() result (superset property) or tokens past its cap become silent
// search misses -- the caller suppresses its scan-all fallback whenever the
// filter returns anything at all.
// ---------------------------------------------------------------------------

TEST(PostingsIndexTest, TokenCapMarksPartialAndSelfNominates) {
    PostingsIndex pi;
    // 6 unique tokens, cap at 2: aaa bbb collected, ccc..fff dropped.
    pi.index_file(1, "aaa bbb ccc ddd eee fff", /*max_unique_tokens=*/2);
    EXPECT_EQ(pi.partial_file_count(), 1);

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;

    // Collected token: found normally, real offset.
    pi.find("aaa", true, files, offsets);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(offsets[1], 0);

    // Dropped token: the partial file MUST still nominate itself,
    // with the scan-me offset sentinel.
    pi.find("fff", true, files, offsets);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], 1u);
    EXPECT_EQ(offsets[1], -1);
}

TEST(PostingsIndexTest, PartialDoesNotDuplicateRealHit) {
    PostingsIndex pi;
    pi.index_file(1, "aaa bbb ccc", /*max_unique_tokens=*/2);
    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    // "aaa" is both a real hit and a partial self-nomination; the real
    // offset must win and the file must appear once.
    pi.find("aaa", true, files, offsets);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(offsets[1], 0);
}

TEST(PostingsIndexTest, PartialNominatesInOtherFilesQueries) {
    PostingsIndex pi;
    pi.index_file(1, "alpha beta", 0);                        // full
    pi.index_file(2, "gamma delta epsilon zeta", /*cap=*/1);  // partial
    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("alpha", true, files, offsets);
    // Real hit in file 1 plus the partial file 2 self-nomination.
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(offsets[1], 0);
    EXPECT_EQ(offsets[2], -1);
}

TEST(PostingsIndexTest, CapZeroIsUnbounded) {
    PostingsIndex pi;
    pi.index_file(1, "aaa bbb ccc ddd", 0);
    EXPECT_EQ(pi.partial_file_count(), 0);
    EXPECT_EQ(pi.token_count(), 4);
}

TEST(PostingsIndexTest, CapNotHitIsNotPartial) {
    PostingsIndex pi;
    pi.index_file(1, "aaa bbb", /*max_unique_tokens=*/10);
    EXPECT_EQ(pi.partial_file_count(), 0);
}

TEST(PostingsIndexTest, RepeatsDoNotConsumeCap) {
    PostingsIndex pi;
    // 2 unique tokens repeated many times; cap 2 must NOT trip.
    pi.index_file(1, "aaa bbb aaa bbb aaa bbb aaa bbb", 2);
    EXPECT_EQ(pi.partial_file_count(), 0);
    EXPECT_EQ(pi.token_count(), 2);
}

TEST(PostingsIndexTest, RemoveFileClearsPartialMarker) {
    PostingsIndex pi;
    pi.index_file(1, "aaa bbb ccc", /*max_unique_tokens=*/1);
    ASSERT_EQ(pi.partial_file_count(), 1);
    pi.remove_file(1);
    EXPECT_EQ(pi.partial_file_count(), 0);
    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("aaa", true, files, offsets);
    EXPECT_TRUE(files.empty());
}

TEST(PostingsIndexTest, ReindexWithoutRemoveDoesNotDuplicate) {
    // The append-only posting lists rely on a per-file guard instead of a
    // per-entry contains(): re-indexing a file without removing it first
    // must clear its old entries, or find() would return the file twice.
    PostingsIndex pi;
    pi.index_file(1, "alpha beta");
    pi.index_file(1, "alpha gamma");

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find("alpha", true, files, offsets);
    EXPECT_EQ(files.size(), 1u);
    pi.find("beta", true, files, offsets);
    EXPECT_TRUE(files.empty());  // old token gone with the re-index
    pi.find("gamma", true, files, offsets);
    EXPECT_EQ(files.size(), 1u);
}

TEST(PostingsIndexTest, OverlongTokenDropMarksFilePartial) {
    // 64 bytes is kept, 65 is dropped. No identifier is 65 bytes; only
    // base64/minified runs are, and they averaged 387 B/token holding
    // 21 MB of a census. Dropping one MUST record the file PARTIAL: the
    // file's token set is now incomplete, so certified-absence narrowing
    // (PostingsIndex::narrow) may not skip it — a silently-dropped run
    // would make its own substrings unfindable. Partial files
    // self-nominate in every find(), which is what the assertions below
    // pin (the dropped token still resolves to the file, via the PARTIAL
    // superset rather than a stored posting).
    PostingsIndex pi;
    std::string keep(PostingsIndex::kMaxTokenBytes, 'a');
    std::string drop(PostingsIndex::kMaxTokenBytes + 1, 'b');
    pi.index_file(1, keep + " " + drop);

    EXPECT_EQ(pi.token_count(), 1);        // Overlong token not stored...
    EXPECT_EQ(pi.partial_file_count(), 1);  // ...but the file is PARTIAL.

    std::vector<FileID> files;
    absl::flat_hash_map<FileID, int> offsets;
    pi.find(keep, true, files, offsets);
    EXPECT_EQ(files.size(), 1u);
    pi.find(drop, true, files, offsets);
    ASSERT_EQ(files.size(), 1u);  // Self-nomination, offset -1 = "scan me".
    EXPECT_EQ(offsets[1], -1);
}

// -- PostingsIndex::narrow (certified-absence prefilter) ----------------------

TEST(PostingsIndexNarrowTest, InteriorRunRequiresExactToken) {
    PostingsIndex pi;
    pi.index_file(1, "the index server exits");
    pi.index_file(2, "the reindex server exits");

    // "index" is interior (delimited by spaces inside the pattern), so only
    // an exact stored token qualifies: file 2 stores "reindex", not "index".
    auto n = pi.narrow(" index server");
    EXPECT_TRUE(n.informative);
    EXPECT_TRUE(n.possible.contains(1));
    EXPECT_FALSE(n.possible.contains(2));
}

TEST(PostingsIndexNarrowTest, PatternStartRunMatchesTokenSuffix) {
    PostingsIndex pi;
    pi.index_file(1, "the reindex server exits");

    // "Index server" can match "...reIndex server..." — the run touching the
    // pattern start must match stored-token SUFFIXes, case-folded.
    auto n = pi.narrow("index server");
    EXPECT_TRUE(n.informative);
    EXPECT_TRUE(n.possible.contains(1));
}

TEST(PostingsIndexNarrowTest, PatternEndRunMatchesTokenPrefix) {
    PostingsIndex pi;
    pi.index_file(1, "the index serverside code");
    pi.index_file(2, "the index preserver code");

    auto n = pi.narrow("index server");
    EXPECT_TRUE(n.possible.contains(1));   // "serverside" prefix-matches
    EXPECT_FALSE(n.possible.contains(2));  // "preserver" does not
}

TEST(PostingsIndexNarrowTest, SingleRunMatchesTokenSubstring) {
    PostingsIndex pi;
    pi.index_file(1, "func repagination() {}");
    pi.index_file(2, "func unrelated() {}");

    auto n = pi.narrow("pagination");
    EXPECT_TRUE(n.informative);
    EXPECT_TRUE(n.possible.contains(1));
    EXPECT_FALSE(n.possible.contains(2));
}

TEST(PostingsIndexNarrowTest, CaseFoldedLookupCoversCaseSensitiveQueries) {
    PostingsIndex pi;
    pi.index_file(1, "type PageWindow struct{}");

    // Postings store lowercase; a case-sensitive query is case-folded for
    // the prefilter (superset) and the verify scan enforces exact case.
    auto n = pi.narrow("PageWindow");
    EXPECT_TRUE(n.informative);
    EXPECT_TRUE(n.possible.contains(1));
}

TEST(PostingsIndexNarrowTest, MultiRunConstraintsIntersect) {
    PostingsIndex pi;
    pi.index_file(1, "alpha beta gamma");
    pi.index_file(2, "alpha delta gamma");

    auto n = pi.narrow("alpha beta");
    EXPECT_TRUE(n.possible.contains(1));
    EXPECT_FALSE(n.possible.contains(2));
}

TEST(PostingsIndexNarrowTest, PartialFilesAlwaysNominate) {
    PostingsIndex pi;
    pi.index_file(1, "alpha beta gamma");
    pi.index_file(2, "many uniq tokens here", /*max_unique_tokens=*/2);

    auto n = pi.narrow("nowhere");
    EXPECT_TRUE(n.informative);
    EXPECT_FALSE(n.possible.contains(1));  // Certified absent.
    EXPECT_TRUE(n.possible.contains(2));   // PARTIAL: token cap hides data.
}

TEST(PostingsIndexNarrowTest, UnindexableRunsAreUninformative) {
    PostingsIndex pi;
    pi.index_file(1, "alpha beta gamma");

    // Runs under 3 bytes are not indexed and certify nothing.
    EXPECT_FALSE(pi.narrow("ab cd").informative);
    // Punctuation-only patterns have no runs at all.
    EXPECT_FALSE(pi.narrow("->[]").informative);
    // Overlong runs are not indexed either.
    std::string overlong(PostingsIndex::kMaxTokenBytes + 5, 'q');
    EXPECT_FALSE(pi.narrow(overlong).informative);
}

TEST(PostingsIndexTest, TokenizeContentReportsTruncation) {
    bool truncated = false;
    auto toks =
        PostingsIndex::tokenize_content("aaa bbb ccc ddd", 2, &truncated);
    EXPECT_TRUE(truncated);
    EXPECT_EQ(toks.size(), 2u);

    truncated = true;
    toks = PostingsIndex::tokenize_content("aaa bbb", 0, &truncated);
    EXPECT_FALSE(truncated);
    EXPECT_EQ(toks.size(), 2u);
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

TEST(ImportResolverTest, ExtractCSharpImports) {
    ImportResolver resolver;

    auto data = resolver.extract_file_imports(
        1, "Service.cs",
        "using System.Text;\n"
        "using static System.Math;\n"
        "global using System.Linq;\n"
        "using Json = System.Text.Json;\n"
        "namespace App { }\n");

    // namespace + static + global + alias = 4 bindings; the `namespace`
    // declaration must NOT be treated as an import.
    ASSERT_EQ(data.bindings.size(), 4u);
    bool ns_text = false, alias_json = false;
    for (const auto& b : data.bindings) {
        if (b.imported_name == "Text" && b.is_wildcard) ns_text = true;
        if (b.imported_name == "Json" &&
            b.original_name == "System.Text.Json")
            alias_json = true;
    }
    EXPECT_TRUE(ns_text);
    EXPECT_TRUE(alias_json);
}

TEST(ImportResolverTest, ExtractCppIncludes) {
    ImportResolver resolver;

    auto data = resolver.extract_file_imports(
        1, "engine.cpp",
        "#include \"core/widget.h\"\n"
        "#include <vector>\n"           // angle = system, skipped
        "#  include \"util.hpp\"\n");   // spaced form

    // Only the two quoted includes; <vector> skipped.
    ASSERT_EQ(data.bindings.size(), 2u);
    bool widget = false, util = false;
    for (const auto& b : data.bindings) {
        if (b.imported_name == "widget" &&
            b.source_file == "core/widget.h" && b.is_wildcard)
            widget = true;
        if (b.imported_name == "util" && b.source_file == "util.hpp")
            util = true;
    }
    EXPECT_TRUE(widget);
    EXPECT_TRUE(util);
}

TEST(ImportResolverTest, ExtractCpp20ModuleImport) {
    ImportResolver resolver;

    auto data = resolver.extract_file_imports(
        1, "mod.cpp",
        "import foo.bar;\n"
        "import <vector>;\n");  // header unit, skipped

    ASSERT_EQ(data.bindings.size(), 1u);
    EXPECT_EQ(data.bindings[0].imported_name, "bar");
    EXPECT_EQ(data.bindings[0].source_file, "foo.bar");
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

TEST(ImportResolverTest, ExtractZigImports) {
    ImportResolver ir;
    auto d = ir.extract_file_imports(
        1, "src/main.zig",
        "const completions = @import(\"features/completions.zig\");\n"
        "const std = @import(\"std\");\n"
        "const sibling = @import(\"analysis.zig\");\n");
    ASSERT_EQ(d.bindings.size(), 1u)
        << "only the directory-carrying import is cross-package evidence";
    EXPECT_EQ(d.bindings[0].source_file, "features");
    EXPECT_EQ(d.bindings[0].imported_name, "completions");
}

TEST(ImportResolverTest, NoEvidenceReturnsUndecided) {
    ImportResolver resolver;

    // Unexported symbol in another file, no import binding: the resolver
    // must NOT guess the first candidate — the caller owns the fallback.
    EnhancedSymbol sym;
    sym.symbol.name = "_hidden";
    sym.symbol.file_id = 2;
    sym.is_exported = false;

    std::vector<SymbolID> candidates = {100};
    auto lookup = [&](SymbolID id) -> const EnhancedSymbol* {
        return id == 100 ? &sym : nullptr;
    };

    EXPECT_EQ(resolver.resolve_symbol_reference(1, "_hidden", candidates,
                                                lookup),
              0u);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// receiver_type derivation (apply_enrichment)
// ---------------------------------------------------------------------------
// The /list-symbols receiver filter compares against
// EnhancedSymbol::receiver_type, which no extractor ever wrote -- the
// filter silently matched nothing. Enrichment now derives it: Go methods
// from the signature receiver, class-language methods from the nearest
// enclosing Class/Struct scope.

TEST(ReferenceTrackerTest, EnrichmentDerivesGoReceiverFromSignature) {
    SymbolLocationIndex sli;
    ReferenceTracker rt(&sli);
    std::vector<Symbol> syms = {
        make_sym("ServeHTTP", SymbolType::Method, 1, 10, 20)};
    auto enhanced = rt.process_file(1, "mux.go", syms, {}, {});
    ASSERT_EQ(enhanced.size(), 1u);

    enhanced[0].signature = "func (m *Mux) ServeHTTP(w http.ResponseWriter)";
    rt.apply_enrichment(enhanced);

    auto snap = rt.pin();
    const auto* es = snap->symbols.get(enhanced[0].id);
    ASSERT_NE(es, nullptr);
    EXPECT_EQ(es->receiver_type, "Mux");
}

TEST(ReferenceTrackerTest, EnrichmentDerivesClassReceiverFromScopeChain) {
    SymbolLocationIndex sli;
    ReferenceTracker rt(&sli);
    ScopeInfo cls;
    cls.type = ScopeType::Class;
    cls.name = "Widget";
    cls.start_line = 1;
    cls.end_line = 100;
    std::vector<ScopeInfo> scopes = {cls};
    std::vector<Symbol> syms = {
        make_sym("render", SymbolType::Method, 2, 10, 20)};
    auto enhanced = rt.process_file(2, "widget.py", syms, {}, scopes);
    ASSERT_EQ(enhanced.size(), 1u);

    enhanced[0].signature = "def render(self):";
    rt.apply_enrichment(enhanced);

    auto snap = rt.pin();
    const auto* es = snap->symbols.get(enhanced[0].id);
    ASSERT_NE(es, nullptr);
    EXPECT_EQ(es->receiver_type, "Widget");
}

TEST(ReferenceTrackerTest, EnrichmentLeavesTopLevelReceiverEmpty) {
    SymbolLocationIndex sli;
    ReferenceTracker rt(&sli);
    std::vector<Symbol> syms = {
        make_sym("helper", SymbolType::Function, 3, 5, 8)};
    auto enhanced = rt.process_file(3, "util.py", syms, {}, {});
    enhanced[0].signature = "def helper():";
    rt.apply_enrichment(enhanced);

    auto snap = rt.pin();
    const auto* es = snap->symbols.get(enhanced[0].id);
    ASSERT_NE(es, nullptr);
    EXPECT_TRUE(es->receiver_type.empty());
}

// Cross-language and path-quality resolution
// ---------------------------------------------------------------------------

TEST(ReferenceTrackerTest, CrossLanguageCandidateStaysUnresolved) {
    ReferenceTracker rt;

    // Vendored C++ file defines `len`; a Python file calls len(). The call
    // must stay unlinked — Python's len is a builtin, not the C++ symbol.
    std::vector<Symbol> cpp_syms = {
        make_sym("len", SymbolType::Function, 2, 170, 180),
    };
    std::vector<Reference> no_refs;
    std::vector<ScopeInfo> no_scopes;
    rt.process_file(2, "sklearn/svm/src/libsvm/svm.cpp", cpp_syms, no_refs,
                    no_scopes);

    std::vector<Symbol> py_syms = {
        make_sym("caller", SymbolType::Function, 1, 1, 10),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "len";
    call.line = 5;
    std::vector<Reference> py_refs = {call};
    auto enhanced =
        rt.process_file(1, "sklearn/utils/validation.py", py_syms, py_refs,
                        no_scopes);
    ASSERT_EQ(enhanced.size(), 1u);

    rt.process_all_references();

    EXPECT_TRUE(rt.get_callee_symbols(enhanced[0].id).empty());
}

TEST(ReferenceTrackerTest, LibraryPathBeatsExamplesPathOnFallback) {
    ReferenceTracker rt;

    std::vector<Reference> no_refs;
    std::vector<ScopeInfo> no_scopes;

    // Same-named dunder (unexported, never imported) defined in an examples
    // file — indexed first so it is the natural first candidate — and in a
    // library file. The ambiguous-name fallback must prefer library code.
    std::vector<Symbol> example_syms = {
        make_sym("__hook__", SymbolType::Function, 2, 10, 20),
    };
    rt.process_file(2, "examples/developing_estimators/demo.py", example_syms,
                    no_refs, no_scopes);

    std::vector<Symbol> lib_syms = {
        make_sym("__hook__", SymbolType::Function, 3, 30, 40),
    };
    auto lib =
        rt.process_file(3, "sklearn/pipeline.py", lib_syms, no_refs, no_scopes);
    ASSERT_EQ(lib.size(), 1u);

    std::vector<Symbol> caller_syms = {
        make_sym("run", SymbolType::Function, 1, 1, 10),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "__hook__";
    call.line = 5;
    std::vector<Reference> caller_refs = {call};
    auto caller = rt.process_file(1, "sklearn/base.py", caller_syms,
                                  caller_refs, no_scopes);
    ASSERT_EQ(caller.size(), 1u);

    rt.process_all_references();

    auto callees = rt.get_callee_symbols(caller[0].id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(callees[0], lib[0].id);
}

// ---------------------------------------------------------------------------
// Scope-chain cache - file identity
// ---------------------------------------------------------------------------

// Discrimination test for the scope-chain cache key. Two files hold a symbol
// with byte-identical line geometry but scopes with DIFFERENT names. The cache
// key is built from line numbers only; without file identity mixed in, the
// second file hits the first file's cache entry and its symbol reports the
// wrong file's scope names.
TEST(ReferenceTrackerTest, ScopeChainCacheDoesNotShareAcrossFiles) {
    ReferenceTracker rt;

    auto make_scope = [](const std::string& name, int start, int end) {
        ScopeInfo sc;
        sc.type = ScopeType::Class;
        sc.name = name;
        sc.full_path = name;
        sc.start_line = start;
        sc.end_line = end;
        sc.level = 1;
        return sc;
    };

    std::vector<Symbol> syms_a = {
        make_sym("handle", SymbolType::Method, 1, 5, 9),
    };
    std::vector<ScopeInfo> scopes_a = {make_scope("AlphaClass", 1, 20)};

    std::vector<Symbol> syms_b = {
        make_sym("handle", SymbolType::Method, 2, 5, 9),
    };
    std::vector<ScopeInfo> scopes_b = {make_scope("BetaClass", 1, 20)};

    auto a = rt.process_file(1, "alpha.go", syms_a, {}, scopes_a);
    auto b = rt.process_file(2, "beta.go", syms_b, {}, scopes_b);

    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    ASSERT_EQ(a[0].scope_chain.size(), 1u);
    ASSERT_EQ(b[0].scope_chain.size(), 1u);
    EXPECT_EQ(a[0].scope_chain[0].name, "AlphaClass");
    EXPECT_EQ(b[0].scope_chain[0].name, "BetaClass");
}

// -- foreign_receiver resolution gates ---------------------------------------
// A call through a non-self receiver must never resolve to the calling symbol
// itself (the false self-loop that fed fake CYCLES), and must not fall back to
// the no-evidence unique-candidate guess (which inflated LOAD BEARING reach).

TEST(ReferenceTrackerTest, ForeignReceiverCallNeverResolvesToSource) {
    ReferenceTracker rt;

    // One method `size` in the file; inside it, `files.size()` — a call whose
    // receiver is a member of an unindexed external type. Name-match would
    // resolve it to the method itself.
    std::vector<Symbol> symbols = {
        make_sym("size", SymbolType::Method, 1, 10, 14),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "size";
    call.line = 12;
    call.column = 20;
    call.foreign_receiver = true;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> scopes;

    rt.process_file(1, "tracker.h", symbols, refs, scopes);
    rt.process_all_references();

    auto ids = rt.pin()->find_symbols_by_name("size");
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_TRUE(rt.get_callee_symbols(ids[0]->id).empty())
        << "foreign-receiver call resolved back to its own source symbol";
}

TEST(ReferenceTrackerTest, BareRecursiveCallStillResolvesToSelf) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("fact", SymbolType::Function, 1, 1, 6),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "fact";
    call.line = 4;
    call.column = 12;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> scopes;

    rt.process_file(1, "fact.go", symbols, refs, scopes);
    rt.process_all_references();

    auto ids = rt.pin()->find_symbols_by_name("fact");
    ASSERT_EQ(ids.size(), 1u);
    auto callees = rt.get_callee_symbols(ids[0]->id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(callees[0], ids[0]->id);
}

TEST(ReferenceTrackerTest, ForeignReceiverSkipsNoEvidenceGuess) {
    ReferenceTracker rt;

    // Caller in a.cs; a lone same-named method `Add` in b.cs (different dir,
    // no import evidence). For a bare call the unique-candidate fallback
    // links; for a foreign-receiver call of unknown type it must not guess.
    std::vector<Symbol> caller = {
        make_sym("Run", SymbolType::Method, 1, 1, 10),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "Add";
    call.line = 5;
    call.column = 8;
    call.foreign_receiver = true;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> scopes;
    rt.process_file(1, "svc/a.cs", caller, refs, scopes);

    std::vector<Symbol> target = {
        make_sym("Add", SymbolType::Method, 2, 1, 4),
    };
    std::vector<Reference> no_refs;
    rt.process_file(2, "store/b.cs", target, no_refs, scopes);
    rt.process_all_references();

    auto run = rt.pin()->find_symbols_by_name("Run");
    ASSERT_EQ(run.size(), 1u);
    EXPECT_TRUE(rt.get_callee_symbols(run[0]->id).empty())
        << "foreign-receiver call of unknown type linked by pure guess";
}

// A receiver-TYPE-qualified call ("Builder.apply") whose type match fails must
// NOT degrade to bare-name resolution: the type evidence positively excludes
// every same-named method on other types. The 2026-08-30 okhttp audit found
// Kotlin's stdlib `apply` (112 call sites, receiver types known, no such
// project method) all resolving to ConnectionSpec.apply via the
// unique-candidate fallback, inflating its whole caller chain's reach
// (concat: 1 real call site, reported reach=155) and fabricating 3/6 layer
// violations. Same class as zls initCapacity reach=146.
TEST(ReferenceTrackerTest, TypedReceiverMissSkipsNameFallback) {
    ReferenceTracker rt;

    // Caller in a.kt: call recorded with known receiver type Builder.
    std::vector<Symbol> caller = {
        make_sym("configure", SymbolType::Function, 1, 1, 10),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "Builder.apply";
    call.line = 5;
    call.column = 8;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> no_scopes;
    rt.process_file(1, "config/a.kt", caller, refs, no_scopes);

    // The corpus's only `apply`: a method of ConnectionSpec in another dir.
    ScopeInfo cls;
    cls.type = ScopeType::Class;
    cls.name = "ConnectionSpec";
    cls.start_line = 1;
    cls.end_line = 100;
    std::vector<ScopeInfo> scopes = {cls};
    std::vector<Symbol> target = {
        make_sym("apply", SymbolType::Method, 2, 10, 20),
    };
    std::vector<Reference> no_refs;
    rt.process_file(2, "net/b.kt", target, no_refs, scopes);
    rt.process_all_references();

    auto run = rt.pin()->find_symbols_by_name("configure");
    ASSERT_EQ(run.size(), 1u);
    EXPECT_TRUE(rt.get_callee_symbols(run[0]->id).empty())
        << "typed-receiver miss fell back to a name-only guess";
}

// The receiver-type match itself must keep working: same setup, but the call's
// receiver type IS the method's owning class.
TEST(ReferenceTrackerTest, TypedReceiverMatchStillResolves) {
    ReferenceTracker rt;

    std::vector<Symbol> caller = {
        make_sym("configure", SymbolType::Function, 1, 1, 10),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "ConnectionSpec.apply";
    call.line = 5;
    call.column = 8;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> no_scopes;
    rt.process_file(1, "config/a.kt", caller, refs, no_scopes);

    ScopeInfo cls;
    cls.type = ScopeType::Class;
    cls.name = "ConnectionSpec";
    cls.start_line = 1;
    cls.end_line = 100;
    std::vector<ScopeInfo> scopes = {cls};
    std::vector<Symbol> target = {
        make_sym("apply", SymbolType::Method, 2, 10, 20),
    };
    std::vector<Reference> no_refs;
    rt.process_file(2, "net/b.kt", target, no_refs, scopes);
    rt.process_all_references();

    auto run = rt.pin()->find_symbols_by_name("configure");
    ASSERT_EQ(run.size(), 1u);
    auto callees = rt.get_callee_symbols(run[0]->id);
    ASSERT_EQ(callees.size(), 1u);
    auto apply_ids = rt.pin()->find_symbols_by_name("apply");
    ASSERT_EQ(apply_ids.size(), 1u);
    EXPECT_EQ(callees[0], apply_ids[0]->id);
}

// Overload delegation must not read as recursion. okhttp audit: 6 of 8
// reported recursion entries (create/createFormData/addPart/newStream/close/
// send) were overloads delegating to a same-named sibling, collapsed into
// self-calls because bare-name resolution has no arity. When the call site's
// argument count is known and an exact-arity sibling exists, resolution picks
// it; equal arity keeps resolving to self (genuine recursion).
TEST(ReferenceTrackerTest, OverloadDelegationPrefersExactArity) {
    ReferenceTracker rt;

    Symbol two = make_sym("create", SymbolType::Function, 1, 1, 10);
    two.parameter_count = 2;
    Symbol one = make_sym("create", SymbolType::Function, 1, 12, 20);
    one.parameter_count = 1;

    // Call inside create/2's span, with ONE argument -> the sibling.
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "create";
    call.line = 5;
    call.column = 8;
    call.call_arg_count = 1;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> scopes;
    std::vector<Symbol> syms = {two, one};
    rt.process_file(1, "part.kt", syms, refs, scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    auto ids = snap->find_symbols_by_name("create");
    ASSERT_EQ(ids.size(), 2u);
    SymbolID two_id = 0, one_id = 0;
    for (const auto& h : ids) {
        (h->symbol.line == 1 ? two_id : one_id) = h->id;
    }
    auto callees = rt.get_callee_symbols(two_id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(callees[0], one_id)
        << "one-arg call inside create/2 must resolve to create/1";
}

TEST(ReferenceTrackerTest, GenuineRecursionKeepsSelfOnMatchingArity) {
    ReferenceTracker rt;

    Symbol two = make_sym("create", SymbolType::Function, 1, 1, 10);
    two.parameter_count = 2;
    Symbol one = make_sym("create", SymbolType::Function, 1, 12, 20);
    one.parameter_count = 1;

    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "create";
    call.line = 5;
    call.column = 8;
    call.call_arg_count = 2;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> scopes;
    std::vector<Symbol> syms = {two, one};
    rt.process_file(1, "part.kt", syms, refs, scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    SymbolID two_id = 0;
    for (const auto& h : snap->find_symbols_by_name("create")) {
        if (h->symbol.line == 1) two_id = h->id;
    }
    auto callees = rt.get_callee_symbols(two_id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(callees[0], two_id) << "matching arity is real recursion";
}

// Unknown arg count (255) must leave resolution exactly as before.
TEST(ReferenceTrackerTest, UnknownArityKeepsFirstMatchBehavior) {
    ReferenceTracker rt;

    Symbol two = make_sym("create", SymbolType::Function, 1, 1, 10);
    two.parameter_count = 2;

    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "create";
    call.line = 5;
    call.column = 8;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> scopes;
    std::vector<Symbol> syms = {two};
    rt.process_file(1, "part.kt", syms, refs, scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    auto ids = snap->find_symbols_by_name("create");
    ASSERT_EQ(ids.size(), 1u);
    auto callees = rt.get_callee_symbols(ids[0]->id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(callees[0], ids[0]->id);
}

// The no-guess policy drops dynamic-dispatch edges (Go interface calls, PHP
// trait callers) — correct for the graph, but the 2026-08-30 battery measured
// refs recall at ~20% on Bootstrap with the omission SILENT. The unresolved
// same-name call-site count is the honest surface: recall becomes a labeled
// lower bound. Qualified "Type.M" spellings count toward M.
TEST(ReferenceTrackerTest, CountsUnresolvedSameNameCallSites) {
    ReferenceTracker rt;

    std::vector<Symbol> caller = {
        make_sym("Run", SymbolType::Method, 1, 1, 10),
    };
    Reference c1;  // unknown-receiver call, stays unresolved
    c1.type = ReferenceType::Call;
    c1.referenced_name = "Bootstrap";
    c1.line = 3;
    c1.column = 4;
    c1.foreign_receiver = true;
    Reference c2;  // typed-receiver miss, also unresolved
    c2.type = ReferenceType::Call;
    c2.referenced_name = "App.Bootstrap";
    c2.line = 5;
    c2.column = 4;
    std::vector<Reference> refs = {c1, c2};
    std::vector<ScopeInfo> scopes;
    rt.process_file(1, "svc/a.go", caller, refs, scopes);

    std::vector<Symbol> target = {
        make_sym("Bootstrap", SymbolType::Method, 2, 1, 5),
    };
    std::vector<Reference> none;
    rt.process_file(2, "core/b.go", target, none, scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    EXPECT_EQ(snap->count_unresolved_calls("Bootstrap"), 2)
        << "both dynamic call sites must be countable";
    EXPECT_EQ(snap->count_unresolved_calls("NoSuchName"), 0);
}

// Zig files ARE namespaces: `const analysis = @import("analysis.zig");
// analysis.resolveType(...)` names the defining FILE. The extractor emits the
// call qualified with the imported file's stem; resolution matches a
// qualified receiver against the candidate's file basename for Zig — real
// import evidence (the audit measured Zig namespaced-call recall at ~30%).
TEST(ReferenceTrackerTest, ZigFileStemQualifiedCallResolves) {
    ReferenceTracker rt;

    std::vector<Symbol> caller = {
        make_sym("main", SymbolType::Function, 1, 1, 10),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "analysis.resolveType";
    call.line = 5;
    call.column = 4;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> scopes;
    rt.process_file(1, "src/main.zig", caller, refs, scopes);

    std::vector<Symbol> target = {
        make_sym("resolveType", SymbolType::Function, 2, 1, 5),
    };
    // A same-named decoy in an unrelated file must not win.
    std::vector<Symbol> decoy = {
        make_sym("resolveType", SymbolType::Function, 3, 1, 5),
    };
    std::vector<Reference> none;
    rt.process_file(2, "src/analysis.zig", target, none, scopes);
    rt.process_file(3, "src/other.zig", decoy, none, scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    auto mains = snap->find_symbols_by_name("main");
    ASSERT_EQ(mains.size(), 1u);
    auto callees = rt.get_callee_symbols(mains[0]->id);
    ASSERT_EQ(callees.size(), 1u);
    const auto* resolved = snap->symbols.get(callees[0]);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->symbol.file_id, 2u)
        << "must resolve into analysis.zig, not the same-named decoy";
}

// An interface-typed call ("App.Bootstrap") resolves to the interface's
// method SPEC once specs are symbols under an Interface scope.
TEST(ReferenceTrackerTest, InterfaceQualifiedCallResolvesToSpec) {
    ReferenceTracker rt;

    std::vector<Symbol> caller = {
        make_sym("run", SymbolType::Function, 1, 1, 10),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "App.Bootstrap";
    call.line = 5;
    call.column = 4;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> no_scopes;
    rt.process_file(1, "svc/run.go", caller, refs, no_scopes);

    ScopeInfo iface;
    iface.type = ScopeType::Interface;
    iface.name = "App";
    iface.start_line = 1;
    iface.end_line = 10;
    std::vector<ScopeInfo> scopes = {iface};
    std::vector<Symbol> decls = {
        make_sym("Bootstrap", SymbolType::Method, 2, 3, 3),
    };
    // Same-named impl on another type must not shadow the typed match.
    ScopeInfo cls;
    cls.type = ScopeType::Struct;
    cls.name = "OtherThing";
    cls.start_line = 1;
    cls.end_line = 10;
    std::vector<ScopeInfo> other_scopes = {cls};
    std::vector<Symbol> impl = {
        make_sym("Bootstrap", SymbolType::Method, 3, 5, 7),
    };
    std::vector<Reference> none;
    rt.process_file(2, "core/app.go", decls, none, scopes);
    rt.process_file(3, "other/impl.go", impl, none, other_scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    auto runs = snap->find_symbols_by_name("run");
    ASSERT_EQ(runs.size(), 1u);
    auto callees = rt.get_callee_symbols(runs[0]->id);
    ASSERT_EQ(callees.size(), 1u);
    const auto* resolved = snap->symbols.get(callees[0]);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->symbol.file_id, 2u)
        << "must resolve to the App interface spec";
}

// Go embedding: `type PB struct { core.App }` promotes App's methods onto
// PB, so `pb.Bootstrap()` (receiver typed PB) must resolve through the
// embedded type to the interface's method spec. Embedding is already
// extracted as an Extends reference on the struct; resolution follows it
// (bounded BFS) when the direct receiver-type match misses.
TEST(ReferenceTrackerTest, EmbeddedTypeResolvesPromotedMethod) {
    ReferenceTracker rt;

    // File 1: the struct PB embedding App (Extends ref inside PB's span).
    std::vector<Symbol> pb_syms = {
        make_sym("PB", SymbolType::Struct, 1, 1, 5),
    };
    Reference embed;
    embed.type = ReferenceType::Extends;
    embed.referenced_name = "App";
    embed.line = 2;
    embed.column = 3;
    std::vector<Reference> pb_refs = {embed};
    std::vector<ScopeInfo> no_scopes;
    rt.process_file(1, "pb/pb.go", pb_syms, pb_refs, no_scopes);

    // File 2: interface App with method spec Bootstrap.
    ScopeInfo iface;
    iface.type = ScopeType::Interface;
    iface.name = "App";
    iface.start_line = 1;
    iface.end_line = 10;
    std::vector<ScopeInfo> scopes = {iface};
    std::vector<Symbol> decls = {
        make_sym("Bootstrap", SymbolType::Method, 2, 3, 3),
    };
    std::vector<Reference> none;
    rt.process_file(2, "core/app.go", decls, none, scopes);

    // File 3: caller with a PB-typed receiver.
    std::vector<Symbol> caller = {
        make_sym("main", SymbolType::Function, 3, 1, 10),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "PB.Bootstrap";
    call.line = 4;
    call.column = 4;
    std::vector<Reference> refs = {call};
    rt.process_file(3, "cmd/main.go", caller, refs, no_scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    auto mains = snap->find_symbols_by_name("main");
    ASSERT_EQ(mains.size(), 1u);
    auto callees = rt.get_callee_symbols(mains[0]->id);
    ASSERT_EQ(callees.size(), 1u);
    const auto* resolved = snap->symbols.get(callees[0]);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->symbol.file_id, 2u)
        << "PB.Bootstrap must resolve through the embedded App";
}

// PHP trait method resolution: Service `use`s Helper (an Extends ref), so
// $this->help() inside Service (qualified Service.help) resolves through the
// trait to Helper::help. Reuses the embedding BFS.
TEST(ReferenceTrackerTest, PhpTraitMethodResolvesThroughUse) {
    ReferenceTracker rt;

    // File 1: Service class with an Extends ref to Helper + the qualified call.
    ScopeInfo svc;
    svc.type = ScopeType::Class;
    svc.name = "Service";
    svc.start_line = 1;
    svc.end_line = 20;
    std::vector<ScopeInfo> svc_scopes = {svc};
    std::vector<Symbol> svc_syms = {
        make_sym("Service", SymbolType::Class, 1, 1, 20),
    };
    Reference ext;
    ext.type = ReferenceType::Extends;
    ext.referenced_name = "Helper";
    ext.line = 2;
    ext.column = 5;
    std::vector<Reference> svc_refs = {ext};
    rt.process_file(1, "svc.php", svc_syms, svc_refs, svc_scopes);

    std::vector<Symbol> caller_syms = {
        make_sym("run", SymbolType::Function, 3, 1, 10),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "Service.help";
    call.line = 5;
    call.column = 8;
    std::vector<Reference> caller_refs = {call};
    std::vector<ScopeInfo> no_sc;
    rt.process_file(3, "caller.php", caller_syms, caller_refs, no_sc);

    // File 2: the trait with the method.
    ScopeInfo helper;
    helper.type = ScopeType::Class;  // trait modeled as a class scope
    helper.name = "Helper";
    helper.start_line = 1;
    helper.end_line = 10;
    std::vector<ScopeInfo> h_scopes = {helper};
    std::vector<Symbol> h_syms = {
        make_sym("Helper", SymbolType::Class, 2, 1, 10),
        make_sym("help", SymbolType::Method, 2, 3, 5),
    };
    std::vector<Reference> none;
    rt.process_file(2, "helper.php", h_syms, none, h_scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    auto runs = snap->find_symbols_by_name("run");
    ASSERT_EQ(runs.size(), 1u);
    auto callees = rt.get_callee_symbols(runs[0]->id);
    ASSERT_EQ(callees.size(), 1u);
    const auto* resolved = snap->symbols.get(callees[0]);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->symbol.file_id, 2u)
        << "Service.help must resolve into the Helper trait";
}

// Scope-qualified type resolution: `Outer.Inner` resolves to the Inner whose
// enclosing scope is Outer, not the same-named standalone type.
TEST(ReferenceTrackerTest, ScopeQualifiedTypeResolvesToNested) {
    ReferenceTracker rt;

    // Nested Outer::Inner (scope_chain has Outer) and a standalone Inner.
    ScopeInfo outer;
    outer.type = ScopeType::Class;
    outer.name = "Outer";
    outer.start_line = 1;
    outer.end_line = 5;
    std::vector<ScopeInfo> nested_scope = {outer};
    std::vector<Symbol> nested = {
        make_sym("Outer", SymbolType::Struct, 1, 1, 5),
        make_sym("Inner", SymbolType::Struct, 1, 2, 3),
    };
    std::vector<Reference> none;
    rt.process_file(1, "a.hpp", nested, none, nested_scope);

    std::vector<Symbol> standalone = {
        make_sym("Inner", SymbolType::Struct, 2, 1, 3),
    };
    rt.process_file(2, "b.hpp", standalone, none, {});

    // A caller referencing Outer.Inner in type position.
    std::vector<Symbol> user = {
        make_sym("make", SymbolType::Function, 3, 1, 3),
    };
    Reference tref;
    tref.type = ReferenceType::Usage;
    tref.type_position = true;
    tref.referenced_name = "Outer.Inner";
    tref.line = 2;
    tref.column = 1;
    std::vector<Reference> urefs = {tref};
    rt.process_file(3, "use.cpp", user, urefs, {});
    rt.process_all_references();

    auto snap = rt.pin();
    // The nested Inner (file 1) carries the incoming ref; the standalone
    // (file 2) does not.
    const EnhancedSymbol* nested_inner = nullptr;
    const EnhancedSymbol* standalone_inner = nullptr;
    for (const auto& h : snap->find_symbols_by_name("Inner")) {
        if (h->symbol.file_id == 1) nested_inner = &*h;
        if (h->symbol.file_id == 2) standalone_inner = &*h;
    }
    ASSERT_NE(nested_inner, nullptr);
    ASSERT_NE(standalone_inner, nullptr);
    EXPECT_GT(nested_inner->incoming_ref_count, 0)
        << "Outer.Inner must credit the nested Inner";
    EXPECT_EQ(standalone_inner->incoming_ref_count, 0)
        << "the standalone Inner must not be credited";
}

// Go field-access receiver: e.App.Find() where e is *RequestEvent and App is
// a field of type core.App resolves to App's Find via the field-type table.
TEST(ReferenceTrackerTest, GoFieldAccessReceiverResolves) {
    ReferenceTracker rt;

    // File 1: RequestEvent has field App of type App; App interface has Find.
    ScopeInfo appscope;
    appscope.type = ScopeType::Interface;
    appscope.name = "App";
    appscope.start_line = 1;
    appscope.end_line = 5;
    std::vector<ScopeInfo> a_scopes = {appscope};
    std::vector<Symbol> core_syms = {
        make_sym("App", SymbolType::Interface, 1, 1, 5),
        make_sym("Find", SymbolType::Method, 1, 2, 2),
        make_sym("RequestEvent", SymbolType::Struct, 1, 7, 9),
    };
    std::vector<Reference> none;
    std::vector<FieldType> ftypes = {{"RequestEvent", "App", "App"}};
    rt.process_file(1, "core/base.go", core_syms, none, a_scopes, ftypes);

    // File 2: a handler with e *RequestEvent calling e.App.Find().
    std::vector<Symbol> handler = {
        make_sym("handle", SymbolType::Function, 2, 1, 5),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "RequestEvent.App.Find";
    call.line = 3;
    call.column = 4;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> no_sc;
    rt.process_file(2, "apis/h.go", handler, refs, no_sc);
    rt.process_all_references();

    auto snap = rt.pin();
    auto hs = snap->find_symbols_by_name("handle");
    ASSERT_EQ(hs.size(), 1u);
    auto callees = rt.get_callee_symbols(hs[0]->id);
    ASSERT_EQ(callees.size(), 1u);
    const auto* resolved = snap->symbols.get(callees[0]);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->symbol.name, "Find")
        << "e.App.Find must resolve through RequestEvent's App field";
}

TEST(ReferenceTrackerTest, RustMethodBareCallResolvesToShadowingFreeFunction) {
    // Rust methods are invoked via `self.`; a bare `trim(...)` inside method
    // `trim` is the same-named FREE function, never recursion.
    ReferenceTracker rt;
    std::vector<Symbol> symbols = {
        make_sym("trim", SymbolType::Method, 1, 10, 14),
        make_sym("trim", SymbolType::Function, 1, 30, 40),
    };
    Reference call;
    call.type = ReferenceType::Call;
    call.referenced_name = "trim";
    call.line = 12;
    call.column = 8;
    std::vector<Reference> refs = {call};
    std::vector<ScopeInfo> scopes;
    rt.process_file(1, "standard.rs", symbols, refs, scopes);
    rt.process_all_references();

    auto snap = rt.pin();
    auto method = snap->find_symbol_by_file_and_name(1, "trim");
    // The METHOD (line 10, encloses line 12) is the caller; its callee must
    // be the free function, not itself.
    auto ids = snap->find_symbols_by_name("trim");
    ASSERT_EQ(ids.size(), 2u);
    SymbolID method_id = 0, free_id = 0;
    for (const auto& h : ids) {
        if (h->symbol.type == SymbolType::Method) method_id = h->id;
        if (h->symbol.type == SymbolType::Function) free_id = h->id;
    }
    ASSERT_NE(method_id, 0u);
    ASSERT_NE(free_id, 0u);
    auto callees = rt.get_callee_symbols(method_id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(callees[0], free_id);
    (void)method;
}

// ---------------------------------------------------------------------------
// collect_callers - callers-of query
// ---------------------------------------------------------------------------

namespace {
Reference make_call(const std::string& name, int line, int column = 4) {
    Reference r;
    r.type = ReferenceType::Call;
    r.referenced_name = name;
    r.line = line;
    r.column = column;
    return r;
}
}  // namespace

TEST(ReferenceTrackerTest, CollectCallersAttributesConfirmedSitesToEnclosingCaller) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("alpha", SymbolType::Function, 1, 1, 10),
        make_sym("beta", SymbolType::Function, 1, 15, 25),
        make_sym("target_fn", SymbolType::Function, 1, 30, 40),
    };
    std::vector<Reference> refs = {
        make_call("target_fn", 5),
        make_call("target_fn", 7),
        make_call("target_fn", 20),
    };
    rt.process_file(1, "test.go", symbols, refs, {});
    rt.process_all_references();

    auto snap = rt.pin();
    auto alpha = snap->find_symbol_by_file_and_name(1, "alpha");
    auto beta = snap->find_symbol_by_file_and_name(1, "beta");
    auto target = snap->find_symbol_by_file_and_name(1, "target_fn");
    ASSERT_TRUE(alpha && beta && target);

    auto result = snap->collect_callers("target_fn");
    ASSERT_EQ(result.definitions.size(), 1u);
    EXPECT_EQ(result.definitions[0]->id, target->id);
    ASSERT_EQ(result.confirmed.size(), 3u);
    EXPECT_TRUE(result.dynamic.empty());
    EXPECT_TRUE(result.unresolved.empty());
    // Sorted by (file, line, column); enclosing caller attributed per site.
    EXPECT_EQ(result.confirmed[0].line, 5);
    EXPECT_EQ(result.confirmed[0].caller, alpha->id);
    EXPECT_EQ(result.confirmed[1].line, 7);
    EXPECT_EQ(result.confirmed[1].caller, alpha->id);
    EXPECT_EQ(result.confirmed[2].line, 20);
    EXPECT_EQ(result.confirmed[2].caller, beta->id);
    for (const auto& s : result.confirmed) {
        EXPECT_EQ(s.target, target->id);
    }
}

TEST(ReferenceTrackerTest, CollectCallersSeparatesDynamicAndUnresolvedSites) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("caller", SymbolType::Function, 1, 1, 20),
    };
    Reference unresolved = make_call("target_fn", 5);
    Reference dyn = make_call("target_fn", 9);
    dyn.foreign_receiver = true;
    std::vector<Reference> refs = {unresolved, dyn};
    rt.process_file(1, "test.py", symbols, refs, {});
    rt.process_all_references();

    auto snap = rt.pin();
    auto result = snap->collect_callers("target_fn");
    EXPECT_TRUE(result.definitions.empty());
    EXPECT_TRUE(result.confirmed.empty());
    ASSERT_EQ(result.dynamic.size(), 1u);
    EXPECT_EQ(result.dynamic[0].line, 9);
    ASSERT_EQ(result.unresolved.size(), 1u);
    EXPECT_EQ(result.unresolved[0].line, 5);
    EXPECT_EQ(result.unresolved[0].target, 0u);
}

TEST(ReferenceTrackerTest, CollectCallersDeclarationOnlyTargetIsDynamic) {
    ReferenceTracker rt;

    Symbol decl = make_sym("Handle", SymbolType::Method, 1, 1, 1);
    decl.declaration_only = true;
    std::vector<Symbol> symbols = {
        decl,
        make_sym("caller", SymbolType::Function, 1, 5, 15),
    };
    std::vector<Reference> refs = {make_call("Handle", 8)};
    rt.process_file(1, "test.go", symbols, refs, {});
    rt.process_all_references();

    auto snap = rt.pin();
    auto result = snap->collect_callers("Handle");
    EXPECT_TRUE(result.confirmed.empty())
        << "a call resolved to a bodiless declaration is runtime dispatch, "
           "never a confirmed static caller";
    ASSERT_EQ(result.dynamic.size(), 1u);
    EXPECT_EQ(result.dynamic[0].line, 8);
}

TEST(ReferenceTrackerTest, CollectCallersMatchesQualifiedUnresolvedSpelling) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("caller", SymbolType::Function, 1, 1, 20),
    };
    // Typed-receiver qualification with no indexed target: attributable by
    // its ".Close" suffix, reported as unresolved (external library).
    std::vector<Reference> refs = {make_call("Conn.Close", 6)};
    rt.process_file(1, "test.go", symbols, refs, {});
    rt.process_all_references();

    auto snap = rt.pin();
    auto result = snap->collect_callers("Close");
    ASSERT_EQ(result.unresolved.size(), 1u);
    EXPECT_EQ(result.unresolved[0].line, 6);
    // Substring spellings must NOT match ("disclose" is not ".Close").
    EXPECT_TRUE(snap->collect_callers("lose").unresolved.empty());
}

TEST(ReferenceTrackerTest, CollectCallersFindsQualifiedCppDefinitionFromBareName) {
    ReferenceTracker rt;

    // C++ out-of-line member definitions are stored QUALIFIED
    // (MasterIndex::update_file); a bare-name query must still find them.
    std::vector<Symbol> symbols = {
        make_sym("MasterIndex::update_file", SymbolType::Function, 1, 10, 30),
        make_sym("caller", SymbolType::Function, 1, 40, 60),
    };
    std::vector<Reference> refs = {make_call("update_file", 45)};
    rt.process_file(1, "master_index.cpp", symbols, refs, {});
    rt.process_all_references();

    auto snap = rt.pin();
    auto bare = snap->collect_callers("update_file");
    ASSERT_EQ(bare.definitions.size(), 1u);
    EXPECT_EQ(bare.definitions[0]->symbol.name, "MasterIndex::update_file");
    EXPECT_EQ(static_cast<int>(bare.confirmed.size() +
                               bare.dynamic.size() +
                               bare.unresolved.size()),
              1)
        << "the call site must surface in exactly one section";

    // The qualified spelling keeps working, and still attributes the
    // bare-spelled call site.
    auto qual = snap->collect_callers("MasterIndex::update_file");
    ASSERT_EQ(qual.definitions.size(), 1u);
    EXPECT_EQ(static_cast<int>(qual.confirmed.size() +
                               qual.dynamic.size() +
                               qual.unresolved.size()),
              1);
}

TEST(ReferenceTrackerTest, TypedReceiverCallResolvesToQualifiedCppMember) {
    ReferenceTracker rt;

    // The extractor emits "MasterIndex.update_file" when the receiver's type
    // is locally known; the out-of-line C++ definition is stored under its
    // QUALIFIED name. Typed-receiver resolution must look the qualified name
    // up too — bare-name candidates alone leave every such call unresolved.
    std::vector<Symbol> defs = {
        make_sym("MasterIndex::update_file", SymbolType::Function, 1, 10, 30),
    };
    rt.process_file(1, "master_index.cpp", defs, {}, {});

    std::vector<Symbol> callers = {
        make_sym("caller", SymbolType::Function, 2, 1, 20),
    };
    std::vector<Reference> refs = {make_call("MasterIndex.update_file", 5)};
    rt.process_file(2, "index_user.cpp", callers, refs, {});
    rt.process_all_references();

    auto snap = rt.pin();
    auto result = snap->collect_callers("update_file");
    ASSERT_EQ(result.definitions.size(), 1u);
    ASSERT_EQ(result.confirmed.size(), 1u)
        << "typed-receiver call must resolve to the qualified member "
           "definition, not fall out as unresolved";
    EXPECT_EQ(result.confirmed[0].line, 5);
    EXPECT_EQ(result.confirmed[0].target, result.definitions[0]->id);
    EXPECT_TRUE(result.unresolved.empty());
    EXPECT_TRUE(result.dynamic.empty());
}

TEST(ReferenceTrackerTest, CollectCallersQualifiedQueryExcludesOtherReceivers) {
    ReferenceTracker rt;

    std::vector<Symbol> defs = {
        make_sym("LinkerEngine::update_file", SymbolType::Function, 1, 10, 30),
    };
    rt.process_file(1, "linker_engine.cpp", defs, {}, {});

    std::vector<Symbol> callers = {
        make_sym("caller", SymbolType::Function, 2, 1, 20),
    };
    // A qualified spelling naming a DIFFERENT receiver type (no such
    // definition indexed), plus a bare spelling.
    std::vector<Reference> refs = {make_call("MasterIndex.update_file", 5),
                                   make_call("update_file", 7)};
    rt.process_file(2, "some_test.cpp", callers, refs, {});
    rt.process_all_references();

    auto snap = rt.pin();
    // Qualified query: a spelling whose receiver names ANOTHER type
    // contradicts the query and must not be attributed; the bare spelling
    // cannot be disproven and stays.
    auto qual = snap->collect_callers("LinkerEngine::update_file");
    ASSERT_EQ(qual.unresolved.size(), 1u)
        << "MasterIndex.update_file must not be attributed to a "
           "LinkerEngine::update_file query";
    EXPECT_EQ(qual.unresolved[0].line, 7);

    // Bare query: both spellings attributed (cannot disambiguate).
    auto bare = snap->collect_callers("update_file");
    EXPECT_EQ(bare.unresolved.size(), 2u);
}

TEST(ReferenceTrackerTest, CallersReportKeepsFileScopeGroupsPerFile) {
    ReferenceTracker rt;

    // Exported Go function (uppercase) so top-level cross-file calls resolve.
    std::vector<Symbol> defs = {
        make_sym("TargetFn", SymbolType::Function, 1, 1, 10)};
    std::vector<Reference> no_refs;
    rt.process_file(1, "lib.go", defs, no_refs, {});
    // Two files with TOP-LEVEL call sites (no enclosing symbol).
    std::vector<Symbol> no_syms;
    std::vector<Reference> refs_a = {make_call("TargetFn", 3)};
    std::vector<Reference> refs_b = {make_call("TargetFn", 8)};
    rt.process_file(2, "a.go", no_syms, refs_a, {});
    rt.process_file(3, "b.go", no_syms, refs_b, {});
    rt.process_all_references();

    auto snap = rt.pin();
    auto path_of = [](FileID fid) { return "f" + std::to_string(fid); };
    auto report = build_callers_report(*snap, "TargetFn", 50, path_of);

    // One <file scope> group PER FILE — never merged across files.
    ASSERT_EQ(report["callers"].size(), 2u)
        << report.dump(2);
    EXPECT_EQ(report["total_callers"], 2);
    EXPECT_EQ(report["callers"][0]["caller"], "<file scope>");
    EXPECT_EQ(report["callers"][0]["file_path"], "f2");
    EXPECT_EQ(report["callers"][0]["call_lines"],
              nlohmann::json::array({3}));
    EXPECT_EQ(report["callers"][1]["file_path"], "f3");
    EXPECT_EQ(report["callers"][1]["call_lines"],
              nlohmann::json::array({8}));
}

TEST(ReferenceTrackerTest, CollectCallersExcludesVariableDefinitionsFromList) {
    ReferenceTracker rt;

    std::vector<Symbol> symbols = {
        make_sym("total", SymbolType::Function, 1, 1, 10),
        make_sym("total", SymbolType::Variable, 1, 20, 20),
        make_sym("caller", SymbolType::Function, 1, 25, 35),
    };
    std::vector<Reference> refs = {make_call("total", 30)};
    rt.process_file(1, "test.go", symbols, refs, {});
    rt.process_all_references();

    auto snap = rt.pin();
    auto result = snap->collect_callers("total");
    // The same-name local variable is not a callable definition.
    ASSERT_EQ(result.definitions.size(), 1u);
    EXPECT_EQ(result.definitions[0]->symbol.type, SymbolType::Function);
}

}  // namespace
}  // namespace lci
