#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>

#include <gtest/gtest.h>
#include <tree_sitter/api.h>

#include <thread>
#include <vector>

namespace lci::parser {
namespace {

// Verify all 13 language grammars can be loaded.
TEST(ParserTest, AllGrammarsLoad) {
    for (int i = 0; i < kLanguageCount; ++i) {
        auto lang = static_cast<Language>(i);
        const TSLanguage* ts_lang = get_ts_language(lang);
        ASSERT_NE(ts_lang, nullptr)
            << "Grammar failed to load for " << to_string(lang);
    }
}

// Verify make_parser creates valid parsers for all languages.
TEST(ParserTest, MakeParserAllLanguages) {
    for (int i = 0; i < kLanguageCount; ++i) {
        auto lang = static_cast<Language>(i);
        UniqueParser p = make_parser(lang);
        ASSERT_NE(p.get(), nullptr)
            << "make_parser failed for " << to_string(lang);
    }
}

// Verify RAII cleanup: UniqueParser frees on destruction.
TEST(ParserTest, UniqueParserRAII) {
    {
        UniqueParser p = make_parser(Language::Go);
        ASSERT_NE(p.get(), nullptr);
    }
    // If we get here without crashing, RAII cleanup worked.
}

// Verify RAII cleanup: UniqueTree frees on destruction.
TEST(ParserTest, UniqueTreeRAII) {
    UniqueParser parser = make_parser(Language::Go);
    ASSERT_NE(parser.get(), nullptr);

    const char* src = "package main\n";
    TSTree* raw = ts_parser_parse_string(
        parser.get(), nullptr, src, static_cast<uint32_t>(strlen(src)));
    ASSERT_NE(raw, nullptr);

    {
        UniqueTree tree(raw);
        TSNode root = ts_tree_root_node(tree.get());
        EXPECT_NE(ts_node_child_count(root), 0u);
    }
    // If we get here without crashing, tree RAII cleanup worked.
}

// Verify language detection from file extensions.
TEST(ParserTest, LanguageFromExtension) {
    struct Case {
        std::string_view ext;
        Language expected;
    };

    Case cases[] = {
        {".go", Language::Go},
        {".py", Language::Python},
        {".js", Language::JavaScript},
        {".jsx", Language::JavaScript},
        {".ts", Language::TypeScript},
        {".tsx", Language::TypeScript},
        {".rs", Language::Rust},
        {".c", Language::C},
        {".cpp", Language::Cpp},
        {".cc", Language::Cpp},
        {".cxx", Language::Cpp},
        {".h", Language::Cpp},
        {".hpp", Language::Cpp},
        {".java", Language::Java},
        {".cs", Language::CSharp},
        {".php", Language::PHP},
        {".phtml", Language::PHP},
        {".kt", Language::Kotlin},
        {".kts", Language::Kotlin},
        {".zig", Language::Zig},
        {".rb", Language::Ruby},
    };

    for (const auto& tc : cases) {
        Language lang{};
        bool ok = language_from_extension(tc.ext, lang);
        ASSERT_TRUE(ok) << "Unrecognized extension: " << tc.ext;
        EXPECT_EQ(lang, tc.expected)
            << "Wrong language for " << tc.ext
            << ": got " << to_string(lang)
            << ", want " << to_string(tc.expected);
    }

    // Unknown extension returns false.
    Language lang{};
    EXPECT_FALSE(language_from_extension(".xyz", lang));
}

// Verify pool acquire returns a valid parser.
TEST(ParserPoolTest, AcquireReturnsParser) {
    auto& pool = thread_pool();
    TSParser* p = pool.acquire(Language::Go);
    ASSERT_NE(p, nullptr);
    pool.release(Language::Go, p);
}

// Verify pool recycles parsers.
TEST(ParserPoolTest, Recycle) {
    auto& pool = thread_pool();
    TSParser* p1 = pool.acquire(Language::Python);
    ASSERT_NE(p1, nullptr);

    pool.release(Language::Python, p1);
    EXPECT_EQ(pool.idle_count(Language::Python), 1u);

    TSParser* p2 = pool.acquire(Language::Python);
    EXPECT_EQ(p2, p1) << "Pool should recycle the same parser instance";
    EXPECT_EQ(pool.idle_count(Language::Python), 0u);

    pool.release(Language::Python, p2);
}

// Verify different languages use separate slots.
TEST(ParserPoolTest, LanguageIsolation) {
    auto& pool = thread_pool();
    TSParser* go_p = pool.acquire(Language::Go);
    TSParser* rs_p = pool.acquire(Language::Rust);

    ASSERT_NE(go_p, nullptr);
    ASSERT_NE(rs_p, nullptr);
    EXPECT_NE(go_p, rs_p);

    pool.release(Language::Go, go_p);
    pool.release(Language::Rust, rs_p);

    EXPECT_GE(pool.idle_count(Language::Go), 1u);
    EXPECT_GE(pool.idle_count(Language::Rust), 1u);
}

// Verify thread-local pools are distinct per thread.
TEST(ParserPoolTest, ThreadLocalIsolation) {
    TSParser* main_p = thread_pool().acquire(Language::Java);
    thread_pool().release(Language::Java, main_p);

    TSParser* other_p = nullptr;
    std::thread t([&other_p] {
        other_p = thread_pool().acquire(Language::Java);
        thread_pool().release(Language::Java, other_p);
    });
    t.join();

    // Different threads get different pool instances, so parsers
    // are independently managed (both should be valid).
    EXPECT_NE(main_p, nullptr);
    EXPECT_NE(other_p, nullptr);
}

// Verify PooledParser RAII guard acquires and releases.
TEST(ParserPoolTest, PooledParserRAII) {
    {
        PooledParser guard(Language::JavaScript);
        ASSERT_TRUE(guard);
        EXPECT_NE(guard.get(), nullptr);
    }
    // Parser returned to pool after guard destruction.
    EXPECT_GE(thread_pool().idle_count(Language::JavaScript), 1u);
}

// Verify PooledParser move semantics.
TEST(ParserPoolTest, PooledParserMove) {
    PooledParser a(Language::Rust);
    TSParser* raw = a.get();
    ASSERT_NE(raw, nullptr);

    PooledParser b(std::move(a));
    EXPECT_EQ(b.get(), raw);
    EXPECT_EQ(a.get(), nullptr);  // NOLINT(bugprone-use-after-move)
}

// Verify all 13 languages work through the pool.
TEST(ParserPoolTest, AllLanguagesThroughPool) {
    auto& pool = thread_pool();
    for (int i = 0; i < kLanguageCount; ++i) {
        auto lang = static_cast<Language>(i);
        TSParser* p = pool.acquire(lang);
        ASSERT_NE(p, nullptr)
            << "Pool acquire failed for " << to_string(lang);
        pool.release(lang, p);
    }
}

// Verify releasing nullptr is safe.
TEST(ParserPoolTest, ReleaseNullptrSafe) {
    auto& pool = thread_pool();
    pool.release(Language::Go, nullptr);
}

}  // namespace
}  // namespace lci::parser
