#include <gtest/gtest.h>

#include <lci/error.h>
#include <lci/string_pool.h>
#include <lci/string_ref.h>

#include <thread>
#include <vector>

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// Error creation and context
// ---------------------------------------------------------------------------
TEST(ErrorTest, IndexingErrorCarriesContext) {
    auto err = make_indexing_error("scan", "disk full");
    EXPECT_EQ(err.type, ErrorType::Indexing);
    EXPECT_EQ(err.operation, "scan");
    EXPECT_EQ(err.message, "disk full");
    EXPECT_EQ(err.to_string(), "indexing scan failed: disk full");
}

TEST(ErrorTest, IndexingErrorWithFilePath) {
    auto err = make_indexing_error("scan", "disk full");
    err.file_path = "/tmp/foo.go";
    EXPECT_EQ(err.to_string(), "indexing scan failed for /tmp/foo.go: disk full");
}

TEST(ErrorTest, ParseErrorFormatsLocation) {
    auto err = make_parse_error(42, "main.go", 10, 5, "func", "unexpected token");
    EXPECT_EQ(err.type, ErrorType::Parse);
    EXPECT_EQ(err.file_id, 42u);
    EXPECT_EQ(err.line, 10);
    EXPECT_EQ(err.column, 5);
    auto s = err.to_string();
    EXPECT_NE(s.find("main.go:10:5"), std::string::npos);
    EXPECT_NE(s.find("func"), std::string::npos);
}

TEST(ErrorTest, SearchErrorIncludesPattern) {
    auto err = make_search_error("foo.*bar", "regex compile failed");
    EXPECT_EQ(err.type, ErrorType::Search);
    auto s = err.to_string();
    EXPECT_NE(s.find("foo.*bar"), std::string::npos);
}

TEST(ErrorTest, FileErrorAutoDetectsPermission) {
    auto err = make_file_error("open", "/etc/shadow", "permission denied");
    EXPECT_EQ(err.type, ErrorType::Permission);
}

TEST(ErrorTest, FileErrorDefaultsToNotFound) {
    auto err = make_file_error("open", "/missing", "no such file");
    EXPECT_EQ(err.type, ErrorType::FileNotFound);
}

TEST(ErrorTest, ConfigErrorFormats) {
    auto err = make_config_error("max_files", "abc", "not a number");
    EXPECT_EQ(err.type, ErrorType::Config);
    auto s = err.to_string();
    EXPECT_NE(s.find("max_files"), std::string::npos);
    EXPECT_NE(s.find("abc"), std::string::npos);
}

// ---------------------------------------------------------------------------
// MultiError
// ---------------------------------------------------------------------------
TEST(MultiErrorTest, EmptyMultiError) {
    MultiError me;
    EXPECT_EQ(me.to_string(), "no errors");
}

TEST(MultiErrorTest, SingleError) {
    MultiError me;
    me.errors.push_back(make_indexing_error("scan", "fail"));
    EXPECT_EQ(me.to_string(), me.errors[0].to_string());
}

TEST(MultiErrorTest, MultipleErrors) {
    MultiError me;
    me.errors.push_back(make_indexing_error("a", "1"));
    me.errors.push_back(make_indexing_error("b", "2"));
    EXPECT_NE(me.to_string().find("2 errors"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Result type
// ---------------------------------------------------------------------------
TEST(ResultTest, HoldsValue) {
    Result<int> r(42);
    EXPECT_TRUE(r.has_value());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.value(), 42);
    EXPECT_EQ(*r, 42);
}

TEST(ResultTest, HoldsError) {
    Result<int> r(make_indexing_error("op", "bad"));
    EXPECT_FALSE(r.has_value());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.error().type, ErrorType::Indexing);
}

TEST(ResultTest, MutableValue) {
    Result<std::string> r(std::string("hello"));
    EXPECT_TRUE(r.has_value());
    *r = "world";
    EXPECT_EQ(*r, "world");
}

// ---------------------------------------------------------------------------
// Hash functions
// ---------------------------------------------------------------------------
TEST(HashTest, Fnv1aConsistent) {
    EXPECT_EQ(hash_fnv1a("hello"), hash_fnv1a("hello"));
    EXPECT_NE(hash_fnv1a("hello"), hash_fnv1a("world"));
}

TEST(HashTest, Fnv1aEmptyString) {
    EXPECT_EQ(hash_fnv1a(""), kFnvOffset64);
}

TEST(HashTest, Fnv1aCombine) {
    uint64_t h = kFnvOffset64;
    uint64_t h2 = hash_fnv1a_combine(h, 42);
    EXPECT_NE(h, h2);
}

// ---------------------------------------------------------------------------
// StringRef
// ---------------------------------------------------------------------------
TEST(StringRefTest, EmptyByDefault) {
    StringRef ref{};
    EXPECT_TRUE(ref.is_empty());
}

TEST(StringRefTest, MakeAndResolve) {
    std::string_view content = "hello world";
    auto ref = make_string_ref(1, content, 6, 5);
    EXPECT_FALSE(ref.is_empty());
    EXPECT_EQ(ref.file_id, 1u);
    EXPECT_EQ(ref.offset, 6u);
    EXPECT_EQ(ref.length, 5u);
    EXPECT_EQ(ref.resolve(content), "world");
}

TEST(StringRefTest, InvalidBoundsReturnsEmpty) {
    std::string_view content = "short";
    auto ref = make_string_ref(1, content, 10, 5);
    EXPECT_TRUE(ref.is_empty());
}

TEST(StringRefTest, EqualSameLocation) {
    std::string_view content = "abcdef";
    auto ref1 = make_string_ref(1, content, 0, 3);
    auto ref2 = make_string_ref(1, content, 0, 3);
    EXPECT_TRUE(ref1.equal(ref2));
}

TEST(StringRefTest, EqualRequiresSlowPathForDifferentLocations) {
    // Same content at different offsets: equal() returns false because
    // it cannot verify content equality without the backing store.
    // This matches the Go implementation's behavior.
    std::string_view content = "abcabc";
    auto ref1 = make_string_ref(1, content, 0, 3);
    auto ref2 = make_string_ref(1, content, 3, 3);
    EXPECT_FALSE(ref1.equal(ref2));
}

TEST(StringRefTest, NotEqualDifferentContent) {
    std::string_view content = "abcdef";
    auto ref1 = make_string_ref(1, content, 0, 3);
    auto ref2 = make_string_ref(1, content, 3, 3);
    EXPECT_FALSE(ref1.equal(ref2));
}

TEST(StringRefTest, ContainsOffset) {
    StringRef ref{1, 10, 5, 0};
    EXPECT_TRUE(ref.contains(10));
    EXPECT_TRUE(ref.contains(14));
    EXPECT_FALSE(ref.contains(15));
    EXPECT_FALSE(ref.contains(9));
}

TEST(StringRefTest, Overlaps) {
    StringRef a{1, 10, 5, 0};
    StringRef b{1, 12, 5, 0};
    StringRef c{1, 20, 5, 0};
    StringRef d{2, 10, 5, 0};  // different file
    EXPECT_TRUE(a.overlaps(b));
    EXPECT_FALSE(a.overlaps(c));
    EXPECT_FALSE(a.overlaps(d));
}

TEST(StringRefTest, Substring) {
    StringRef ref{1, 10, 20, 99};
    auto sub = ref.substring(5, 10);
    EXPECT_EQ(sub.file_id, 1u);
    EXPECT_EQ(sub.offset, 15u);
    EXPECT_EQ(sub.length, 10u);
    EXPECT_EQ(sub.hash, 0u);
}

TEST(StringRefTest, SubstringClampedLength) {
    StringRef ref{1, 0, 10, 0};
    auto sub = ref.substring(8, 5);
    EXPECT_EQ(sub.length, 2u);
}

TEST(StringRefTest, SubstringOutOfBounds) {
    StringRef ref{1, 0, 10, 0};
    auto sub = ref.substring(10, 5);
    EXPECT_TRUE(sub.is_empty());
}

TEST(StringRefTest, CompareOrdering) {
    StringRef a{1, 0, 5, 100};
    StringRef b{1, 0, 5, 200};
    EXPECT_LT(a.compare(b), 0);
    EXPECT_GT(b.compare(a), 0);
    EXPECT_EQ(a.compare(a), 0);
}

// ---------------------------------------------------------------------------
// StringPool
// ---------------------------------------------------------------------------
TEST(StringPoolTest, InternReturnsConsistentID) {
    StringPool pool;
    uint32_t id1 = pool.intern("hello");
    uint32_t id2 = pool.intern("hello");
    EXPECT_EQ(id1, id2);
    EXPECT_EQ(pool.size(), 1u);
}

TEST(StringPoolTest, DifferentStringsGetDifferentIDs) {
    StringPool pool;
    uint32_t id1 = pool.intern("hello");
    uint32_t id2 = pool.intern("world");
    EXPECT_NE(id1, id2);
    EXPECT_EQ(pool.size(), 2u);
}

TEST(StringPoolTest, GetStringRoundTrip) {
    StringPool pool;
    uint32_t id = pool.intern("test string");
    auto [s, ok] = pool.get_string(id);
    EXPECT_TRUE(ok);
    EXPECT_EQ(s, "test string");
}

TEST(StringPoolTest, GetStringUnknownID) {
    StringPool pool;
    auto [s, ok] = pool.get_string(999);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(s.empty());
}

TEST(StringPoolTest, InternRangeFullString) {
    StringPool pool;
    auto range = pool.intern_range("hello");
    EXPECT_EQ(range.start, 0u);
    EXPECT_EQ(range.length, 5u);

    auto [s, ok] = pool.get_range_string(range);
    EXPECT_TRUE(ok);
    EXPECT_EQ(s, "hello");
}

TEST(StringPoolTest, GetRangeStringSubstring) {
    StringPool pool;
    auto range = pool.intern_range("hello world");
    auto sub = StringPool::create_subrange(range, 6, 5);

    auto [s, ok] = pool.get_range_string(sub);
    EXPECT_TRUE(ok);
    EXPECT_EQ(s, "world");
}

TEST(StringPoolTest, GetRangeStringOutOfBounds) {
    StringPool pool;
    auto range = pool.intern_range("short");
    StringRange bad{range.pool_id, 100, 5};

    auto [s, ok] = pool.get_range_string(bad);
    EXPECT_FALSE(ok);
}

TEST(StringPoolTest, GetRangeStringClamps) {
    StringPool pool;
    auto range = pool.intern_range("hello");
    StringRange over{range.pool_id, 3, 100};

    auto [s, ok] = pool.get_range_string(over);
    EXPECT_TRUE(ok);
    EXPECT_EQ(s, "lo");
}

// ---------------------------------------------------------------------------
// StringPool concurrent access
// ---------------------------------------------------------------------------
TEST(StringPoolTest, ConcurrentIntern) {
    StringPool pool;
    constexpr int kThreads = 8;
    constexpr int kStringsPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pool, t]() {
            for (int i = 0; i < kStringsPerThread; ++i) {
                std::string s = "str_" + std::to_string(t) + "_" + std::to_string(i);
                uint32_t id = pool.intern(s);
                auto [retrieved, ok] = pool.get_string(id);
                EXPECT_TRUE(ok);
                EXPECT_EQ(retrieved, s);
            }
        });
    }

    for (auto& th : threads) th.join();

    EXPECT_EQ(pool.size(), static_cast<size_t>(kThreads * kStringsPerThread));
}

TEST(StringPoolTest, ConcurrentInternDeduplicate) {
    StringPool pool;
    constexpr int kThreads = 8;

    std::vector<uint32_t> ids(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pool, &ids, t]() {
            ids[static_cast<size_t>(t)] = pool.intern("shared_string");
        });
    }

    for (auto& th : threads) th.join();

    for (int t = 1; t < kThreads; ++t) {
        EXPECT_EQ(ids[0], ids[static_cast<size_t>(t)]);
    }
    EXPECT_EQ(pool.size(), 1u);
}

// ---------------------------------------------------------------------------
// FileStringPool
// ---------------------------------------------------------------------------
TEST(FileStringPoolTest, LineCountSingleLine) {
    StringPool pool;
    FileStringPool fsp(pool, "no newlines here");
    EXPECT_EQ(fsp.line_count(), 1);
}

TEST(FileStringPoolTest, LineCountMultipleLines) {
    StringPool pool;
    FileStringPool fsp(pool, "line1\nline2\nline3");
    EXPECT_EQ(fsp.line_count(), 3);
}

TEST(FileStringPoolTest, GetLineContent) {
    StringPool pool;
    std::string_view content = "aaa\nbbb\nccc";
    FileStringPool fsp(pool, content);

    auto [range0, ok0] = fsp.get_line(0);
    EXPECT_TRUE(ok0);
    auto [s0, ok0s] = pool.get_range_string(range0);
    EXPECT_TRUE(ok0s);
    EXPECT_EQ(s0, "aaa");

    auto [range2, ok2] = fsp.get_line(2);
    EXPECT_TRUE(ok2);
    auto [s2, ok2s] = pool.get_range_string(range2);
    EXPECT_TRUE(ok2s);
    EXPECT_EQ(s2, "ccc");
}

TEST(FileStringPoolTest, GetLineOutOfBounds) {
    StringPool pool;
    FileStringPool fsp(pool, "one\ntwo");
    auto [range, ok] = fsp.get_line(-1);
    EXPECT_FALSE(ok);
    auto [range2, ok2] = fsp.get_line(5);
    EXPECT_FALSE(ok2);
}

TEST(FileStringPoolTest, GetContextLines) {
    StringPool pool;
    FileStringPool fsp(pool, "a\nb\nc\nd\ne");
    auto ranges = fsp.get_context_lines(2, 1, 1);
    EXPECT_EQ(ranges.size(), 3u);
}

// ---------------------------------------------------------------------------
// ErrorType to_string
// ---------------------------------------------------------------------------
TEST(ErrorTypeTest, AllVariantsHaveNames) {
    EXPECT_EQ(to_string(ErrorType::Indexing), "indexing");
    EXPECT_EQ(to_string(ErrorType::Parse), "parse");
    EXPECT_EQ(to_string(ErrorType::Search), "search");
    EXPECT_EQ(to_string(ErrorType::FileNotFound), "file_not_found");
    EXPECT_EQ(to_string(ErrorType::FileTooLarge), "file_too_large");
    EXPECT_EQ(to_string(ErrorType::Permission), "permission");
    EXPECT_EQ(to_string(ErrorType::Config), "config");
    EXPECT_EQ(to_string(ErrorType::Internal), "internal");
}

}  // namespace
}  // namespace lci
