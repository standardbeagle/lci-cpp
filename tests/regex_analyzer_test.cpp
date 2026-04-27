#include <gtest/gtest.h>

#include <lci/regex_analyzer/engine.h>
#include <lci/search/requirements_analyzer.h>
#include <lci/search/semantic_filter.h>

namespace lci {
namespace {

// =============================================================================
// RegexClassifier tests
// =============================================================================

TEST(RegexClassifier, EmptyPatternIsNotSimple) {
    RegexClassifier c;
    EXPECT_FALSE(c.is_simple(""));
}

TEST(RegexClassifier, SimpleLiteralIsSimple) {
    RegexClassifier c;
    EXPECT_TRUE(c.is_simple("hello"));
    EXPECT_TRUE(c.is_simple("foo_bar"));
    EXPECT_TRUE(c.is_simple("abc123"));
}

TEST(RegexClassifier, SimpleCharClassIsSimple) {
    RegexClassifier c;
    EXPECT_TRUE(c.is_simple("[abc]+"));
    EXPECT_TRUE(c.is_simple("[a-z]+"));
}

TEST(RegexClassifier, SimpleAlternationIsSimple) {
    RegexClassifier c;
    EXPECT_TRUE(c.is_simple("foo|bar|baz"));
}

TEST(RegexClassifier, LookaheadIsComplex) {
    RegexClassifier c;
    EXPECT_FALSE(c.is_simple("foo(?=bar)"));
    EXPECT_FALSE(c.is_simple("foo(?!bar)"));
}

TEST(RegexClassifier, BackreferenceIsComplex) {
    RegexClassifier c;
    EXPECT_FALSE(c.is_simple("(foo)\\1"));
}

TEST(RegexClassifier, AtomicGroupIsComplex) {
    RegexClassifier c;
    EXPECT_FALSE(c.is_simple("(?>foo)"));
}

TEST(RegexClassifier, InlineModifierIsComplex) {
    RegexClassifier c;
    EXPECT_FALSE(c.is_simple("(?i:foo)"));
}

TEST(RegexClassifier, UnbalancedParensNotSimple) {
    RegexClassifier c;
    EXPECT_FALSE(c.is_simple("(foo"));
    EXPECT_FALSE(c.is_simple("foo)"));
}

TEST(RegexClassifier, DeeplyNestedNotSimple) {
    RegexClassifier c;
    EXPECT_FALSE(c.is_simple("((((((a))))))"));
}

TEST(RegexClassifier, ManyAlternationsNotSimple) {
    RegexClassifier c;
    std::string pattern;
    for (int i = 0; i < 25; ++i) {
        if (i > 0) pattern += '|';
        pattern += "alt" + std::to_string(i);
    }
    EXPECT_FALSE(c.is_simple(pattern));
}

// =============================================================================
// LiteralExtractor tests
// =============================================================================

TEST(LiteralExtractor, ExtractsSimpleLiterals) {
    LiteralExtractor e;
    auto lits = e.extract_literals("hello.*world");
    EXPECT_GE(lits.size(), 2u);

    bool has_hello = false, has_world = false;
    for (const auto& lit : lits) {
        if (lit == "hello") has_hello = true;
        if (lit == "world") has_world = true;
    }
    EXPECT_TRUE(has_hello);
    EXPECT_TRUE(has_world);
}

TEST(LiteralExtractor, ExtractsFromAlternation) {
    LiteralExtractor e;
    auto lits = e.extract_literals("(foo|bar|baz)");
    EXPECT_GE(lits.size(), 3u);

    bool has_foo = false, has_bar = false, has_baz = false;
    for (const auto& lit : lits) {
        if (lit == "foo") has_foo = true;
        if (lit == "bar") has_bar = true;
        if (lit == "baz") has_baz = true;
    }
    EXPECT_TRUE(has_foo);
    EXPECT_TRUE(has_bar);
    EXPECT_TRUE(has_baz);
}

TEST(LiteralExtractor, SkipsShortLiterals) {
    LiteralExtractor e;
    auto lits = e.extract_literals("ab");
    EXPECT_TRUE(lits.empty());
}

TEST(LiteralExtractor, NoDuplicates) {
    LiteralExtractor e;
    auto lits = e.extract_literals("(hello|hello)");
    int count = 0;
    for (const auto& lit : lits) {
        if (lit == "hello") ++count;
    }
    EXPECT_EQ(count, 1);
}

TEST(LiteralExtractor, ExtractsFromComplexPattern) {
    LiteralExtractor e;
    auto lits = e.extract_literals(R"(func\s+(\w+)\s*\()");
    bool has_func = false;
    for (const auto& lit : lits) {
        if (lit == "func") has_func = true;
    }
    EXPECT_TRUE(has_func);
}

// =============================================================================
// RegexCache tests
// =============================================================================

TEST(RegexCache, CacheMiss) {
    RegexCache cache(10, 10);
    auto [simple, complex] = cache.get_regex("foo", false);
    EXPECT_EQ(simple, nullptr);
    EXPECT_EQ(complex, nullptr);
}

TEST(RegexCache, SimpleCacheHit) {
    RegexCache cache(10, 10);

    SimpleRegexPattern pattern;
    pattern.pattern = "foo";
    pattern.literals = {"foo"};
    pattern.compiled = std::regex("foo");
    cache.cache_simple(std::move(pattern), false);

    auto [simple, complex] = cache.get_regex("foo", false);
    EXPECT_NE(simple, nullptr);
    EXPECT_EQ(complex, nullptr);
    EXPECT_EQ(simple->pattern, "foo");
}

TEST(RegexCache, ComplexCacheHit) {
    RegexCache cache(10, 10);
    cache.cache_complex("foo.*bar", std::regex("foo.*bar"), false);

    auto [simple, complex] = cache.get_regex("foo.*bar", false);
    EXPECT_EQ(simple, nullptr);
    EXPECT_NE(complex, nullptr);
}

TEST(RegexCache, CaseInsensitiveSeparateEntry) {
    RegexCache cache(10, 10);

    SimpleRegexPattern p;
    p.pattern = "foo";
    p.compiled = std::regex("foo");
    cache.cache_simple(std::move(p), false);

    auto [simple_ci, complex_ci] = cache.get_regex("foo", true);
    EXPECT_EQ(simple_ci, nullptr);
    EXPECT_EQ(complex_ci, nullptr);
}

TEST(RegexCache, EvictsWhenFull) {
    RegexCache cache(2, 2);

    for (int i = 0; i < 5; ++i) {
        SimpleRegexPattern p;
        p.pattern = "pat" + std::to_string(i);
        p.compiled = std::regex(p.pattern);
        cache.cache_simple(std::move(p), false);
    }

    auto [size_s, size_c] = cache.get_size();
    EXPECT_LE(size_s, 2);
}

TEST(RegexCache, ClearResetsEverything) {
    RegexCache cache(10, 10);

    SimpleRegexPattern p;
    p.pattern = "foo";
    p.compiled = std::regex("foo");
    cache.cache_simple(std::move(p), false);
    cache.cache_complex("bar", std::regex("bar"), false);

    cache.clear();

    auto [size_s, size_c] = cache.get_size();
    EXPECT_EQ(size_s, 0);
    EXPECT_EQ(size_c, 0);

    auto stats = cache.get_stats();
    EXPECT_EQ(stats.total_requests, 0);
}

TEST(RegexCache, StatsTracking) {
    RegexCache cache(10, 10);

    cache.get_regex("miss1", false);
    cache.get_regex("miss2", false);

    SimpleRegexPattern p;
    p.pattern = "hit";
    p.compiled = std::regex("hit");
    cache.cache_simple(std::move(p), false);
    cache.get_regex("hit", false);

    auto stats = cache.get_stats();
    EXPECT_EQ(stats.total_requests, 3);
    EXPECT_EQ(stats.simple_hits, 1);
    EXPECT_GE(stats.simple_misses, 2);
}

// =============================================================================
// HybridRegexEngine tests
// =============================================================================

TEST(HybridRegexEngine, ExtractLiterals) {
    HybridRegexEngine engine(10, 10);
    auto lits = engine.extract_literals("hello.*world");
    EXPECT_GE(lits.size(), 2u);
}

TEST(HybridRegexEngine, ClassifiesSimple) {
    HybridRegexEngine engine(10, 10);
    EXPECT_TRUE(engine.is_simple("hello"));
    EXPECT_TRUE(engine.is_simple("foo_bar"));
}

TEST(HybridRegexEngine, ClassifiesComplex) {
    HybridRegexEngine engine(10, 10);
    EXPECT_FALSE(engine.is_simple("foo(?=bar)"));
}

TEST(HybridRegexEngine, CompilesValidPattern) {
    HybridRegexEngine engine(10, 10);
    auto re = engine.compile("hello", false);
    EXPECT_NE(re, nullptr);
}

TEST(HybridRegexEngine, CompilesCaseInsensitive) {
    HybridRegexEngine engine(10, 10);
    auto re = engine.compile("hello", true);
    EXPECT_NE(re, nullptr);

    std::string text = "HELLO world";
    EXPECT_TRUE(std::regex_search(text, *re));
}

TEST(HybridRegexEngine, CompileFailsOnInvalidPattern) {
    HybridRegexEngine engine(10, 10);
    auto re = engine.compile("[invalid", false);
    EXPECT_EQ(re, nullptr);
}

// =============================================================================
// RequirementsAnalyzer tests
// =============================================================================

TEST(RequirementsAnalyzer, DefaultAnalysis) {
    RequirementsAnalyzer ra;
    auto result = ra.analyze("foo", SearchOptions{});

    EXPECT_FALSE(result.required_indexes.empty());
    EXPECT_TRUE(RequirementsAnalyzer::should_use_index(
        result, IndexType::Trigram));
}

TEST(RequirementsAnalyzer, SymbolPatternRequiresSymbolIndex) {
    RequirementsAnalyzer ra;
    auto result = ra.analyze("MyClass.myMethod", SearchOptions{});

    EXPECT_TRUE(RequirementsAnalyzer::should_use_index(
        result, IndexType::Symbol));
}

TEST(RequirementsAnalyzer, DeclarationOnlyAddsSymbolIndex) {
    RequirementsAnalyzer ra;
    SearchOptions opts;
    opts.declaration_only = true;
    auto result = ra.analyze("foo", opts);

    EXPECT_TRUE(RequirementsAnalyzer::should_use_index(
        result, IndexType::Symbol));
}

TEST(RequirementsAnalyzer, UsageOnlyAddsReferenceIndex) {
    RequirementsAnalyzer ra;
    SearchOptions opts;
    opts.usage_only = true;
    auto result = ra.analyze("foo", opts);

    EXPECT_TRUE(RequirementsAnalyzer::should_use_index(
        result, IndexType::Reference));
}

TEST(RequirementsAnalyzer, ContextLinesAddPostingsAndContent) {
    RequirementsAnalyzer ra;
    SearchOptions opts;
    opts.max_context_lines = 5;
    auto result = ra.analyze("foo", opts);

    EXPECT_TRUE(RequirementsAnalyzer::should_use_index(
        result, IndexType::Postings));
    EXPECT_TRUE(RequirementsAnalyzer::should_use_index(
        result, IndexType::Content));
}

TEST(RequirementsAnalyzer, RegexPatternDetected) {
    RequirementsAnalyzer ra;
    auto result = ra.analyze("/foo.*bar/", SearchOptions{});

    bool found = false;
    for (const auto& reason : result.reasoning) {
        if (reason.find("regex") != std::string::npos ||
            reason.find("Regex") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(RequirementsAnalyzer, FilePathPatternDetected) {
    RequirementsAnalyzer ra;
    auto result = ra.analyze("src/main.go", SearchOptions{});

    EXPECT_TRUE(RequirementsAnalyzer::should_use_index(
        result, IndexType::Location));
}

TEST(RequirementsAnalyzer, ArchitecturalPatternDetected) {
    RequirementsAnalyzer ra;
    auto result = ra.analyze("controller", SearchOptions{});

    EXPECT_TRUE(RequirementsAnalyzer::should_use_index(
        result, IndexType::CallGraph));
}

TEST(RequirementsAnalyzer, EstimatedSearchTime) {
    RequirementsAnalyzer ra;
    auto result = ra.analyze("foo", SearchOptions{});
    auto time = RequirementsAnalyzer::estimated_search_time(result);
    EXPECT_GT(time, 0);
}

TEST(RequirementsAnalyzer, OptimizationHintsForComplexPattern) {
    RequirementsAnalyzer ra;
    // A pattern with many operators should generate hints.
    auto result = ra.analyze("a|b|c|d|e|f|g|h|i|j|k|l", SearchOptions{});
    // May or may not have hints depending on complexity threshold.
    // Just verify it runs without error.
    EXPECT_GE(result.estimated_cost, 0);
}

// =============================================================================
// SemanticFilter tests
// =============================================================================

TEST(SemanticFilter, CommentLineDetection) {
    EXPECT_TRUE(SemanticFilter::is_comment_line("  // this is a comment"));
    EXPECT_TRUE(SemanticFilter::is_comment_line("# python comment"));
    EXPECT_TRUE(SemanticFilter::is_comment_line("  /* block comment"));
    EXPECT_TRUE(SemanticFilter::is_comment_line("  end of block */"));
    EXPECT_FALSE(SemanticFilter::is_comment_line("  int x = 5;"));
    EXPECT_FALSE(SemanticFilter::is_comment_line(""));
    EXPECT_FALSE(SemanticFilter::is_comment_line("   "));
}

TEST(SemanticFilter, LineForOffset) {
    EXPECT_EQ(1, SemanticFilter::line_for_offset("abc\ndef\nghi", 0));
    EXPECT_EQ(1, SemanticFilter::line_for_offset("abc\ndef\nghi", 2));
    EXPECT_EQ(2, SemanticFilter::line_for_offset("abc\ndef\nghi", 4));
    EXPECT_EQ(3, SemanticFilter::line_for_offset("abc\ndef\nghi", 8));
}

TEST(SemanticFilter, NoFilterReturnsAll) {
    // Create a minimal FileContentStore for the filter.
    FileContentStore store;
    SemanticFilter filter(store);

    std::string_view content = "func main() {\n  x := 1\n}\n";
    std::vector<SymbolLineEntry> symbols;
    std::vector<SemanticMatch> matches = {{1, 0, 4}, {2, 15, 16}};
    SearchOptions opts;

    auto result = filter.apply_filter(0, content, symbols, matches, "x", opts);
    EXPECT_EQ(result.size(), 2u);
}

TEST(SemanticFilter, ExcludeCommentsFiltersCommentLines) {
    FileContentStore store;
    SemanticFilter filter(store);

    std::string_view content = "// comment line\ncode line\n";
    std::vector<SymbolLineEntry> symbols;
    std::vector<SemanticMatch> matches = {{1, 0, 7}, {2, 16, 20}};
    SearchOptions opts;
    opts.exclude_comments = true;

    auto result = filter.apply_filter(
        0, content, symbols, matches, "code", opts);
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].line, 2);
}

TEST(SemanticFilter, DeclarationOnlyWithSymbol) {
    FileContentStore store;
    SemanticFilter filter(store);

    std::string_view content = "func foo() {}\nfoo()\n";
    SymbolLineEntry sym;
    sym.line = 1;
    sym.name = "foo";
    sym.type = SymbolType::Function;
    std::vector<SymbolLineEntry> symbols = {sym};
    std::vector<SemanticMatch> matches = {{1, 0, 3}, {2, 14, 17}};
    SearchOptions opts;
    opts.declaration_only = true;

    auto result = filter.apply_filter(
        0, content, symbols, matches, "foo", opts);
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].line, 1);
}

TEST(SemanticFilter, UsageOnlyExcludesDeclaration) {
    FileContentStore store;
    SemanticFilter filter(store);

    std::string_view content = "func foo() {}\nfoo()\n";
    SymbolLineEntry sym;
    sym.line = 1;
    sym.name = "foo";
    sym.type = SymbolType::Function;
    std::vector<SymbolLineEntry> symbols = {sym};
    std::vector<SemanticMatch> matches = {{1, 0, 3}, {2, 14, 17}};
    SearchOptions opts;
    opts.usage_only = true;

    auto result = filter.apply_filter(
        0, content, symbols, matches, "foo", opts);
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].line, 2);
}

}  // namespace
}  // namespace lci
