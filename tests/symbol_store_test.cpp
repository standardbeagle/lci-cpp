#include <gtest/gtest.h>

#include <lci/core/symbol_store.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// Helper to create test symbols
// ---------------------------------------------------------------------------
EnhancedSymbol make_symbol(SymbolID id, const std::string& name,
                           SymbolType type, FileID file_id,
                           int line, int column,
                           int end_line, int end_column) {
    EnhancedSymbol es;
    es.id = id;
    es.symbol.name = name;
    es.symbol.type = type;
    es.symbol.file_id = file_id;
    es.symbol.line = line;
    es.symbol.column = column;
    es.symbol.end_line = end_line;
    es.symbol.end_column = end_column;
    return es;
}

// ---------------------------------------------------------------------------
// SymbolStore - basic operations
// ---------------------------------------------------------------------------
TEST(SymbolStoreTest, EmptyStore) {
    SymbolStore store;
    EXPECT_EQ(store.size(), 0);
    EXPECT_EQ(store.get(1), nullptr);
    EXPECT_TRUE(store.get_all().empty());
}

TEST(SymbolStoreTest, SetAndGet) {
    SymbolStore store;
    auto sym = make_symbol(100, "foo", SymbolType::Function, 1, 10, 0, 20, 0);
    store.set(100, sym);

    EXPECT_EQ(store.size(), 1);
    const auto* found = store.get(100);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->symbol.name, "foo");
    EXPECT_EQ(found->id, 100u);
}

TEST(SymbolStoreTest, SetUpdatesExisting) {
    SymbolStore store;
    store.set(100, make_symbol(100, "foo", SymbolType::Function, 1, 10, 0, 20, 0));
    store.set(100, make_symbol(100, "bar", SymbolType::Method, 1, 10, 0, 20, 0));

    EXPECT_EQ(store.size(), 1);
    EXPECT_EQ(store.get(100)->symbol.name, "bar");
    EXPECT_EQ(store.stats().total_functions, 0);
    EXPECT_EQ(store.stats().total_methods, 1);
}

TEST(SymbolStoreTest, Remove) {
    SymbolStore store;
    store.set(1, make_symbol(1, "a", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "b", SymbolType::Method, 1, 6, 0, 10, 0));
    store.set(3, make_symbol(3, "c", SymbolType::Class, 1, 11, 0, 20, 0));

    EXPECT_TRUE(store.remove(2));
    EXPECT_EQ(store.size(), 2);
    EXPECT_EQ(store.get(2), nullptr);
    EXPECT_NE(store.get(1), nullptr);
    EXPECT_NE(store.get(3), nullptr);
}

TEST(SymbolStoreTest, RemoveNonExistent) {
    SymbolStore store;
    EXPECT_FALSE(store.remove(999));
}

TEST(SymbolStoreTest, Clear) {
    SymbolStore store;
    store.set(1, make_symbol(1, "a", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "b", SymbolType::Method, 1, 6, 0, 10, 0));
    store.clear();

    EXPECT_EQ(store.size(), 0);
    EXPECT_EQ(store.get(1), nullptr);
    EXPECT_EQ(store.stats().total_symbols, 0);
}

TEST(SymbolStoreTest, GetAllReturnsSpan) {
    SymbolStore store;
    store.set(1, make_symbol(1, "a", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "b", SymbolType::Method, 1, 6, 0, 10, 0));

    auto all = store.get_all();
    EXPECT_EQ(static_cast<int>(all.size()), 2);
}

TEST(SymbolStoreTest, GetIds) {
    SymbolStore store;
    store.set(10, make_symbol(10, "x", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(20, make_symbol(20, "y", SymbolType::Function, 1, 6, 0, 10, 0));

    auto ids = store.get_ids();
    EXPECT_EQ(ids.size(), 2u);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids[0], 10u);
    EXPECT_EQ(ids[1], 20u);
}

TEST(SymbolStoreTest, Range) {
    SymbolStore store;
    store.set(1, make_symbol(1, "a", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "b", SymbolType::Method, 1, 6, 0, 10, 0));
    store.set(3, make_symbol(3, "c", SymbolType::Class, 1, 11, 0, 20, 0));

    int count = 0;
    store.range([&](SymbolID, const EnhancedSymbol&) {
        ++count;
        return true;
    });
    EXPECT_EQ(count, 3);
}

TEST(SymbolStoreTest, RangeEarlyStop) {
    SymbolStore store;
    store.set(1, make_symbol(1, "a", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "b", SymbolType::Method, 1, 6, 0, 10, 0));

    int count = 0;
    store.range([&](SymbolID, const EnhancedSymbol&) {
        ++count;
        return false;
    });
    EXPECT_EQ(count, 1);
}

// ---------------------------------------------------------------------------
// SymbolStore - secondary indices
// ---------------------------------------------------------------------------
TEST(SymbolStoreTest, SymbolsByFile) {
    SymbolStore store;
    store.set(1, make_symbol(1, "a", SymbolType::Function, 10, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "b", SymbolType::Method, 10, 6, 0, 10, 0));
    store.set(3, make_symbol(3, "c", SymbolType::Class, 20, 1, 0, 20, 0));

    auto file10 = store.get_symbols_by_file(10);
    EXPECT_EQ(static_cast<int>(file10.size()), 2);

    auto file20 = store.get_symbols_by_file(20);
    EXPECT_EQ(static_cast<int>(file20.size()), 1);

    auto file99 = store.get_symbols_by_file(99);
    EXPECT_TRUE(file99.empty());
}

TEST(SymbolStoreTest, SymbolsByName) {
    SymbolStore store;
    store.set(1, make_symbol(1, "init", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "init", SymbolType::Function, 2, 1, 0, 5, 0));
    store.set(3, make_symbol(3, "main", SymbolType::Function, 1, 6, 0, 10, 0));

    auto init_syms = store.get_symbols_by_name("init");
    EXPECT_EQ(static_cast<int>(init_syms.size()), 2);

    auto main_syms = store.get_symbols_by_name("main");
    EXPECT_EQ(static_cast<int>(main_syms.size()), 1);
}

// ---------------------------------------------------------------------------
// SymbolStore - statistics
// ---------------------------------------------------------------------------
TEST(SymbolStoreTest, StatsTracking) {
    SymbolStore store;
    store.set(1, make_symbol(1, "f1", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "m1", SymbolType::Method, 1, 6, 0, 10, 0));
    store.set(3, make_symbol(3, "c1", SymbolType::Class, 1, 11, 0, 20, 0));
    store.set(4, make_symbol(4, "v1", SymbolType::Variable, 1, 21, 0, 22, 0));
    store.set(5, make_symbol(5, "k1", SymbolType::Constant, 1, 23, 0, 24, 0));

    const auto& s = store.stats();
    EXPECT_EQ(s.total_symbols, 5);
    EXPECT_EQ(s.total_functions, 1);
    EXPECT_EQ(s.total_methods, 1);
    EXPECT_EQ(s.total_classes, 1);
    EXPECT_EQ(s.total_variables, 1);
    EXPECT_EQ(s.total_constants, 1);
    EXPECT_EQ(s.files_indexed, 1);
}

TEST(SymbolStoreTest, StatsAfterRemove) {
    SymbolStore store;
    store.set(1, make_symbol(1, "f1", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "f2", SymbolType::Function, 1, 6, 0, 10, 0));
    store.remove(1);

    EXPECT_EQ(store.stats().total_symbols, 1);
    EXPECT_EQ(store.stats().total_functions, 1);
}

TEST(SymbolStoreTest, GetEntryPoints) {
    SymbolStore store;
    auto sym1 = make_symbol(1, "public_fn", SymbolType::Function, 1, 1, 0, 5, 0);
    sym1.is_exported = true;
    store.set(1, sym1);

    auto sym2 = make_symbol(2, "private_fn", SymbolType::Function, 1, 6, 0, 10, 0);
    sym2.is_exported = false;
    store.set(2, sym2);

    auto sym3 = make_symbol(3, "public_method", SymbolType::Method, 1, 11, 0, 20, 0);
    sym3.is_exported = true;
    store.set(3, sym3);

    auto sym4 = make_symbol(4, "public_class", SymbolType::Class, 1, 21, 0, 30, 0);
    sym4.is_exported = true;
    store.set(4, sym4);

    auto entries = store.get_entry_points();
    EXPECT_EQ(entries.size(), 2u);
}

TEST(SymbolStoreTest, GetTopSymbols) {
    SymbolStore store;

    auto sym1 = make_symbol(1, "popular", SymbolType::Function, 1, 1, 0, 5, 0);
    sym1.incoming_refs.resize(10);
    store.set(1, sym1);

    auto sym2 = make_symbol(2, "medium", SymbolType::Function, 1, 6, 0, 10, 0);
    sym2.incoming_refs.resize(5);
    store.set(2, sym2);

    auto sym3 = make_symbol(3, "unpopular", SymbolType::Function, 1, 11, 0, 20, 0);
    store.set(3, sym3);

    auto top = store.get_top_symbols(2);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0], 1u);
    EXPECT_EQ(top[1], 2u);
}

TEST(SymbolStoreTest, RebuildIndices) {
    SymbolStore store;
    store.set(1, make_symbol(1, "a", SymbolType::Function, 10, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "b", SymbolType::Method, 20, 6, 0, 10, 0));

    store.rebuild_indices();

    EXPECT_EQ(store.stats().total_symbols, 2);
    EXPECT_EQ(static_cast<int>(store.get_symbols_by_file(10).size()), 1);
    EXPECT_EQ(static_cast<int>(store.get_symbols_by_file(20).size()), 1);
}

// ---------------------------------------------------------------------------
// SymbolStore - swap-and-delete correctness
// ---------------------------------------------------------------------------
TEST(SymbolStoreTest, SwapAndDeleteMaintainsCorrectness) {
    SymbolStore store;
    store.set(1, make_symbol(1, "first", SymbolType::Function, 1, 1, 0, 5, 0));
    store.set(2, make_symbol(2, "second", SymbolType::Method, 1, 6, 0, 10, 0));
    store.set(3, make_symbol(3, "third", SymbolType::Class, 1, 11, 0, 20, 0));

    store.remove(1);

    EXPECT_EQ(store.get(2)->symbol.name, "second");
    EXPECT_EQ(store.get(3)->symbol.name, "third");
    EXPECT_EQ(store.size(), 2);
}

// ---------------------------------------------------------------------------
// SymbolStore - O(1) lookup benchmark
// ---------------------------------------------------------------------------
TEST(SymbolStoreTest, BenchmarkLookup) {
    SymbolStore store(10000);
    for (int i = 1; i <= 10000; ++i) {
        store.set(static_cast<SymbolID>(i),
                  make_symbol(static_cast<SymbolID>(i),
                              "sym_" + std::to_string(i),
                              SymbolType::Function, 1,
                              i, 0, i + 10, 0));
    }

    auto start = std::chrono::steady_clock::now();
    volatile const EnhancedSymbol* ptr = nullptr;
    for (int iter = 0; iter < 100000; ++iter) {
        auto id = static_cast<SymbolID>((iter % 10000) + 1);
        ptr = store.get(id);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    (void)ptr;

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    auto ns_per_lookup = ns / 100000;

    EXPECT_LT(ns_per_lookup, 2000) << "O(1) lookup should be under 2us";
}

// ---------------------------------------------------------------------------
// SymbolLocationIndex - basic operations
// ---------------------------------------------------------------------------
TEST(SymbolLocationIndexTest, EmptyIndex) {
    SymbolLocationIndex idx;
    EXPECT_EQ(idx.file_count(), 0);
    EXPECT_EQ(idx.total_symbols(), 0);
    EXPECT_EQ(idx.find_symbol_at_position(1, 1, 0), nullptr);
    EXPECT_EQ(idx.find_symbol_id_at_position(1, 1, 0), 0u);
}

TEST(SymbolLocationIndexTest, IndexAndFindSingleLine) {
    SymbolLocationIndex idx;

    std::vector<Symbol> symbols;
    Symbol s;
    s.name = "foo";
    s.type = SymbolType::Function;
    s.file_id = 1;
    s.line = 5;
    s.column = 4;
    s.end_line = 5;
    s.end_column = 20;
    symbols.push_back(s);

    std::vector<EnhancedSymbol> enhanced;
    EnhancedSymbol es;
    es.id = 42;
    es.symbol = s;
    enhanced.push_back(es);

    idx.index_file_symbols(1, symbols, enhanced);

    const auto* found = idx.find_symbol_at_position(1, 5, 10);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "foo");

    EXPECT_EQ(idx.find_symbol_id_at_position(1, 5, 10), 42u);
}

TEST(SymbolLocationIndexTest, FindMostSpecific) {
    SymbolLocationIndex idx;

    std::vector<Symbol> symbols;
    Symbol outer;
    outer.name = "MyClass";
    outer.type = SymbolType::Class;
    outer.file_id = 1;
    outer.line = 1;
    outer.column = 0;
    outer.end_line = 50;
    outer.end_column = 1;
    symbols.push_back(outer);

    Symbol inner;
    inner.name = "my_method";
    inner.type = SymbolType::Method;
    inner.file_id = 1;
    inner.line = 10;
    inner.column = 4;
    inner.end_line = 20;
    inner.end_column = 5;
    symbols.push_back(inner);

    std::vector<EnhancedSymbol> enhanced;
    EnhancedSymbol es1;
    es1.id = 1;
    es1.symbol = outer;
    enhanced.push_back(es1);
    EnhancedSymbol es2;
    es2.id = 2;
    es2.symbol = inner;
    enhanced.push_back(es2);

    idx.index_file_symbols(1, symbols, enhanced);

    const auto* found = idx.find_symbol_at_position(1, 15, 8);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "my_method");
}

TEST(SymbolLocationIndexTest, PositionOutsideAllSymbols) {
    SymbolLocationIndex idx;

    std::vector<Symbol> symbols;
    Symbol s;
    s.name = "foo";
    s.type = SymbolType::Function;
    s.file_id = 1;
    s.line = 10;
    s.column = 0;
    s.end_line = 20;
    s.end_column = 1;
    symbols.push_back(s);

    std::vector<EnhancedSymbol> enhanced;
    EnhancedSymbol es;
    es.id = 1;
    es.symbol = s;
    enhanced.push_back(es);

    idx.index_file_symbols(1, symbols, enhanced);

    EXPECT_EQ(idx.find_symbol_at_position(1, 5, 0), nullptr);
    EXPECT_EQ(idx.find_symbol_at_position(1, 25, 0), nullptr);
}

TEST(SymbolLocationIndexTest, GetFileSymbols) {
    SymbolLocationIndex idx;

    std::vector<Symbol> symbols;
    for (int i = 0; i < 3; ++i) {
        Symbol s;
        s.name = "sym_" + std::to_string(i);
        s.type = SymbolType::Function;
        s.file_id = 1;
        s.line = i * 10 + 1;
        s.column = 0;
        s.end_line = i * 10 + 5;
        s.end_column = 0;
        symbols.push_back(s);
    }

    std::vector<EnhancedSymbol> enhanced;
    for (int i = 0; i < 3; ++i) {
        EnhancedSymbol es;
        es.id = static_cast<SymbolID>(i + 1);
        es.symbol = symbols[static_cast<size_t>(i)];
        enhanced.push_back(es);
    }

    idx.index_file_symbols(1, symbols, enhanced);

    auto file_syms = idx.get_file_symbols(1);
    EXPECT_EQ(file_syms.size(), 3u);
}

TEST(SymbolLocationIndexTest, RemoveFile) {
    SymbolLocationIndex idx;

    std::vector<Symbol> symbols;
    Symbol s;
    s.name = "foo";
    s.type = SymbolType::Function;
    s.file_id = 1;
    s.line = 1;
    s.column = 0;
    s.end_line = 10;
    s.end_column = 0;
    symbols.push_back(s);

    std::vector<EnhancedSymbol> enhanced;
    EnhancedSymbol es;
    es.id = 1;
    es.symbol = s;
    enhanced.push_back(es);

    idx.index_file_symbols(1, symbols, enhanced);
    EXPECT_EQ(idx.file_count(), 1);

    idx.remove_file(1);
    EXPECT_EQ(idx.file_count(), 0);
    EXPECT_EQ(idx.find_symbol_at_position(1, 5, 0), nullptr);
}

TEST(SymbolLocationIndexTest, Clear) {
    SymbolLocationIndex idx;

    std::vector<Symbol> symbols;
    Symbol s;
    s.name = "foo";
    s.type = SymbolType::Function;
    s.file_id = 1;
    s.line = 1;
    s.column = 0;
    s.end_line = 10;
    s.end_column = 0;
    symbols.push_back(s);

    std::vector<EnhancedSymbol> enhanced;
    EnhancedSymbol es;
    es.id = 1;
    es.symbol = s;
    enhanced.push_back(es);

    idx.index_file_symbols(1, symbols, enhanced);
    idx.index_file_symbols(2, symbols, enhanced);

    idx.clear();
    EXPECT_EQ(idx.file_count(), 0);
    EXPECT_EQ(idx.total_symbols(), 0);
}

TEST(SymbolLocationIndexTest, MultiLineSymbolLookup) {
    SymbolLocationIndex idx;

    std::vector<Symbol> symbols;
    Symbol s;
    s.name = "big_function";
    s.type = SymbolType::Function;
    s.file_id = 1;
    s.line = 5;
    s.column = 0;
    s.end_line = 50;
    s.end_column = 1;
    symbols.push_back(s);

    std::vector<EnhancedSymbol> enhanced;
    EnhancedSymbol es;
    es.id = 99;
    es.symbol = s;
    enhanced.push_back(es);

    idx.index_file_symbols(1, symbols, enhanced);

    EXPECT_EQ(idx.find_symbol_id_at_position(1, 5, 0), 99u);
    EXPECT_EQ(idx.find_symbol_id_at_position(1, 8, 4), 99u);
    EXPECT_EQ(idx.find_symbol_id_at_position(1, 50, 0), 99u);
}

TEST(SymbolLocationIndexTest, BenchmarkPositionLookup) {
    SymbolLocationIndex idx;

    std::vector<Symbol> symbols;
    std::vector<EnhancedSymbol> enhanced;
    for (int i = 0; i < 500; ++i) {
        Symbol s;
        s.name = "func_" + std::to_string(i);
        s.type = SymbolType::Function;
        s.file_id = 1;
        s.line = i * 10 + 1;
        s.column = 0;
        s.end_line = i * 10 + 8;
        s.end_column = 1;
        symbols.push_back(s);

        EnhancedSymbol es;
        es.id = static_cast<SymbolID>(i + 1);
        es.symbol = s;
        enhanced.push_back(es);
    }

    idx.index_file_symbols(1, symbols, enhanced);

    auto start = std::chrono::steady_clock::now();
    volatile SymbolID result = 0;
    for (int iter = 0; iter < 100000; ++iter) {
        int line = (iter % 500) * 10 + 3;
        result = idx.find_symbol_id_at_position(1, line, 4);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    (void)result;

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    auto ns_per_lookup = ns / 100000;

    EXPECT_LT(ns_per_lookup, 10000) << "Position lookup should be under 10us";
}

}  // namespace
}  // namespace lci
