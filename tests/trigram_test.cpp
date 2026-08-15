#include <gtest/gtest.h>

#include <lci/core/reference_tracker.h>
#include <lci/core/trigram.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// is_pure_ascii
// ---------------------------------------------------------------------------
TEST(IsPureAsciiTest, AsciiContent) {
    EXPECT_TRUE(is_pure_ascii("hello world"));
    EXPECT_TRUE(is_pure_ascii("func main() {}"));
    EXPECT_TRUE(is_pure_ascii(""));
}

TEST(IsPureAsciiTest, UnicodeContent) {
    EXPECT_FALSE(is_pure_ascii("hello\xC3\xA9world"));
    EXPECT_FALSE(is_pure_ascii("\xE4\xB8\xAD\xE6\x96\x87"));
}

// ---------------------------------------------------------------------------
// extract_simple_trigrams - ASCII trigram extraction
// ---------------------------------------------------------------------------
TEST(ExtractSimpleTrigramsTest, ShortContent) {
    auto trigrams = extract_simple_trigrams("ab");
    EXPECT_TRUE(trigrams.empty());
}

TEST(ExtractSimpleTrigramsTest, ExactlyThreeChars) {
    auto trigrams = extract_simple_trigrams("abc");
    ASSERT_EQ(trigrams.size(), 1u);
    EXPECT_TRUE(trigrams.contains(0));

    uint32_t expected = (uint32_t('a') << 16) | (uint32_t('b') << 8) | uint32_t('c');
    EXPECT_EQ(trigrams[0], expected);
}

TEST(ExtractSimpleTrigramsTest, MultipleTrigramsFromWord) {
    auto trigrams = extract_simple_trigrams("hello");
    // "hel", "ell", "llo" -> 3 trigrams
    EXPECT_EQ(trigrams.size(), 3u);
    EXPECT_TRUE(trigrams.contains(0));
    EXPECT_TRUE(trigrams.contains(1));
    EXPECT_TRUE(trigrams.contains(2));
}

TEST(ExtractSimpleTrigramsTest, SkipsNonAlphanumericOnly) {
    // "   " has no alphanumeric, should be skipped.
    // "  a" has 'a', should be included.
    auto trigrams = extract_simple_trigrams("   abc");
    // Position 0: "   " -> no alpha -> skip
    // Position 1: "  a" -> has 'a' -> include
    // Position 2: " ab" -> has 'a','b' -> include
    // Position 3: "abc" -> include
    EXPECT_FALSE(trigrams.contains(0));
    EXPECT_TRUE(trigrams.contains(1));
    EXPECT_TRUE(trigrams.contains(2));
    EXPECT_TRUE(trigrams.contains(3));
}

TEST(ExtractSimpleTrigramsTest, BitShiftEncoding) {
    auto trigrams = extract_simple_trigrams("ABC");
    uint32_t expected = (uint32_t('A') << 16) | (uint32_t('B') << 8) | uint32_t('C');
    EXPECT_EQ(trigrams[0], expected);
}

// ---------------------------------------------------------------------------
// extract_unicode_trigrams
// ---------------------------------------------------------------------------
TEST(ExtractUnicodeTrigramsTest, AsciiContentWorks) {
    auto trigrams = extract_unicode_trigrams("hello");
    EXPECT_EQ(trigrams.size(), 3u);
}

TEST(ExtractUnicodeTrigramsTest, MultiByteChars) {
    // 3 CJK characters = 1 trigram
    std::string content = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97";
    auto trigrams = extract_unicode_trigrams(content);
    EXPECT_EQ(trigrams.size(), 1u);
    EXPECT_TRUE(trigrams.contains(0));
    EXPECT_EQ(trigrams[0], content);
}

TEST(ExtractUnicodeTrigramsTest, ShortContent) {
    auto trigrams = extract_unicode_trigrams("ab");
    EXPECT_TRUE(trigrams.empty());
}

// ---------------------------------------------------------------------------
// predict_trigram_count
// ---------------------------------------------------------------------------
TEST(PredictTrigramCountTest, ZeroOrNegative) {
    TrigramIndex idx;
    EXPECT_EQ(idx.predict_trigram_count(0), 0);
    EXPECT_EQ(idx.predict_trigram_count(-5), 0);
}

TEST(PredictTrigramCountTest, SmallContent) {
    TrigramIndex idx;
    EXPECT_EQ(idx.predict_trigram_count(10), 8);
}

TEST(PredictTrigramCountTest, MediumContent) {
    TrigramIndex idx;
    EXPECT_EQ(idx.predict_trigram_count(100), 50);
}

TEST(PredictTrigramCountTest, LargeContent) {
    TrigramIndex idx;
    EXPECT_EQ(idx.predict_trigram_count(10000), 1000);
}

// ---------------------------------------------------------------------------
// TrigramIndex - indexing and search
// ---------------------------------------------------------------------------
TEST(TrigramIndexTest, IndexAndFindCandidates) {
    TrigramIndex idx;
    idx.index_file(1, "func main() { return 0; }");
    idx.index_file(2, "class Foo { void bar() {} }");

    auto candidates = idx.find_candidates("func");
    EXPECT_FALSE(candidates.empty());
    EXPECT_TRUE(std::find(candidates.begin(), candidates.end(), 1u)
                != candidates.end());
}

TEST(TrigramIndexTest, ShortPatternReturnsEmpty) {
    TrigramIndex idx;
    idx.index_file(1, "hello world");

    auto candidates = idx.find_candidates("hi");
    EXPECT_TRUE(candidates.empty());
}

TEST(TrigramIndexTest, FileCount) {
    TrigramIndex idx;
    EXPECT_EQ(idx.file_count(), 0);

    idx.index_file(1, "hello world foo bar");
    idx.index_file(2, "another test file baz");
    EXPECT_EQ(idx.file_count(), 2);
}

TEST(TrigramIndexTest, CaseInsensitiveSearch) {
    TrigramIndex idx;
    idx.index_file(1, "function testcase value");

    // Exact case match should find the file.
    auto sensitive = idx.find_candidates_with_options("function", false);
    EXPECT_FALSE(sensitive.empty());

    // Lowercased pattern against lowercased content works.
    auto insensitive = idx.find_candidates_with_options("FUNCTION", true);
    EXPECT_FALSE(insensitive.empty());
}

// ---------------------------------------------------------------------------
// File invalidation
// ---------------------------------------------------------------------------
TEST(TrigramIndexTest, RemoveFileInvalidation) {
    TrigramIndex idx;
    idx.index_file(1, "function test() { return 42; }");
    idx.index_file(2, "function other() { return 0; }");

    EXPECT_EQ(idx.file_count(), 2);

    idx.remove_file(1);
    // File 1 is now invalidated (lazy).
    EXPECT_EQ(idx.get_invalidation_count(), 1);
    // file_count should exclude invalidated files.
    EXPECT_EQ(idx.file_count(), 1);

    // Candidates should not include invalidated file.
    auto candidates = idx.find_candidates("function");
    for (auto fid : candidates) {
        EXPECT_NE(fid, 1u);
    }
}

TEST(TrigramIndexTest, ForceCleanupRemovesInvalidated) {
    TrigramIndex idx;
    idx.index_file(1, "hello world foo bar baz");

    idx.remove_file(1);
    EXPECT_EQ(idx.get_invalidation_count(), 1);

    idx.force_cleanup();
    EXPECT_EQ(idx.get_invalidation_count(), 0);
    EXPECT_EQ(idx.file_count(), 0);
}

TEST(TrigramIndexTest, CleanupThreshold) {
    TrigramIndex idx;
    idx.set_cleanup_threshold(3);

    idx.index_file(1, "content one alpha beta");
    idx.index_file(2, "content two gamma delta");
    idx.index_file(3, "content three epsilon zeta");

    idx.remove_file(1);
    idx.remove_file(2);
    EXPECT_EQ(idx.get_invalidation_count(), 2);

    // Third removal should trigger cleanup (threshold=3).
    idx.remove_file(3);
    EXPECT_EQ(idx.get_invalidation_count(), 0);
}

// ---------------------------------------------------------------------------
// Find-candidates behavior
// ---------------------------------------------------------------------------
TEST(TrigramIndexTest, RepeatedFindCandidatesIsDeterministic) {
    TrigramIndex idx;
    idx.index_file(1, "function test() { return value; }");

    auto first = idx.find_candidates("function");
    auto second = idx.find_candidates("function");

    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

TEST(TrigramIndexTest, ResultsClearedAfterRemoval) {
    TrigramIndex idx;
    idx.index_file(1, "function test() { return value; }");

    auto before = idx.find_candidates("function");
    EXPECT_FALSE(before.empty());

    idx.remove_file(1);
    idx.force_cleanup();

    auto after = idx.find_candidates("function");
    EXPECT_TRUE(after.empty());
}

// ---------------------------------------------------------------------------
// index_file_with_trigrams (pre-computed)
// ---------------------------------------------------------------------------
TEST(TrigramIndexTest, IndexWithPrecomputedTrigrams) {
    TrigramIndex idx;

    absl::flat_hash_map<uint32_t, std::vector<uint32_t>> trigrams;
    uint32_t abc = (uint32_t('a') << 16) | (uint32_t('b') << 8) | uint32_t('c');
    trigrams[abc] = {0, 10, 20};

    idx.index_file_with_trigrams(1, trigrams);

    auto candidates = idx.find_candidates("abc");
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0], 1u);
}

// ---------------------------------------------------------------------------
// ShardedTrigramStorage
// ---------------------------------------------------------------------------
TEST(ShardedTrigramStorageTest, BucketCountDefault) {
    ShardedTrigramStorage storage;
    EXPECT_EQ(storage.get_bucket_count(), 256);
}

TEST(ShardedTrigramStorageTest, SearchAfterMerge) {
    ShardedTrigramStorage storage(256);

    BucketedTrigramResult result;
    result.file_id = 1;
    result.buckets.resize(256);

    uint32_t trigram = (uint32_t('f') << 16) | (uint32_t('o') << 8) | uint32_t('o');
    uint16_t bid = static_cast<uint16_t>(trigram & 255);
    result.buckets[bid].trigrams[trigram] = {0, 5, 10};

    storage.merge_bucketed_trigrams(result);

    auto locs = storage.search_trigram(trigram);
    ASSERT_EQ(locs.size(), 3u);
    EXPECT_EQ(locs[0].file_id, 1u);
    EXPECT_EQ(locs[0].offset, 0u);
    EXPECT_EQ(locs[1].offset, 5u);
    EXPECT_EQ(locs[2].offset, 10u);
}

TEST(ShardedTrigramStorageTest, RemoveFile) {
    ShardedTrigramStorage storage(256);

    BucketedTrigramResult r1;
    r1.file_id = 1;
    r1.buckets.resize(256);
    uint32_t trigram = (uint32_t('b') << 16) | (uint32_t('a') << 8) | uint32_t('r');
    uint16_t bid = static_cast<uint16_t>(trigram & 255);
    r1.buckets[bid].trigrams[trigram] = {0};

    BucketedTrigramResult r2;
    r2.file_id = 2;
    r2.buckets.resize(256);
    r2.buckets[bid].trigrams[trigram] = {100};

    storage.merge_bucketed_trigrams(r1);
    storage.merge_bucketed_trigrams(r2);

    auto before = storage.search_trigram(trigram);
    EXPECT_EQ(before.size(), 2u);

    storage.remove_file(1);

    auto after = storage.search_trigram(trigram);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].file_id, 2u);
}

TEST(ShardedTrigramStorageTest, Clear) {
    ShardedTrigramStorage storage(256);

    BucketedTrigramResult result;
    result.file_id = 1;
    result.buckets.resize(256);
    uint32_t trigram = (uint32_t('x') << 16) | (uint32_t('y') << 8) | uint32_t('z');
    uint16_t bid = static_cast<uint16_t>(trigram & 255);
    result.buckets[bid].trigrams[trigram] = {0};
    storage.merge_bucketed_trigrams(result);

    storage.clear();
    auto locs = storage.search_trigram(trigram);
    EXPECT_TRUE(locs.empty());
}

// ---------------------------------------------------------------------------
// bucket_trigrams utility
// ---------------------------------------------------------------------------
TEST(BucketTrigramsTest, DistributesToCorrectBuckets) {
    absl::flat_hash_map<int, uint32_t> trigrams;
    uint32_t abc = (uint32_t('a') << 16) | (uint32_t('b') << 8) | uint32_t('c');
    uint32_t def = (uint32_t('d') << 16) | (uint32_t('e') << 8) | uint32_t('f');
    trigrams[0] = abc;
    trigrams[3] = def;

    auto result = bucket_trigrams(1, trigrams, 256);
    EXPECT_EQ(result.file_id, 1u);
    EXPECT_EQ(result.buckets.size(), 256u);

    uint16_t abc_bucket = static_cast<uint16_t>(abc & 255);
    uint16_t def_bucket = static_cast<uint16_t>(def & 255);

    EXPECT_FALSE(result.buckets[abc_bucket].trigrams.empty());
    EXPECT_FALSE(result.buckets[def_bucket].trigrams.empty());
}

// ---------------------------------------------------------------------------
// Bucketing integration
// ---------------------------------------------------------------------------
TEST(TrigramIndexTest, GetBucketForTrigram) {
    TrigramIndex idx;
    uint32_t trigram = 0x00ABCD;
    uint16_t bucket = idx.get_bucket_for_trigram(trigram);
    EXPECT_EQ(bucket, static_cast<uint16_t>(trigram & 255));
}

TEST(TrigramIndexTest, CreateBucketedResult) {
    TrigramIndex idx;
    auto result = idx.create_bucketed_result(42);
    EXPECT_EQ(result.file_id, 42u);
    EXPECT_EQ(result.buckets.size(), 256u);
}

TEST(TrigramIndexTest, IndexWithBucketedTrigrams) {
    TrigramIndex idx;

    auto result = idx.create_bucketed_result(1);
    uint32_t trigram = (uint32_t('f') << 16) | (uint32_t('u') << 8) | uint32_t('n');
    uint16_t bid = idx.get_bucket_for_trigram(trigram);
    result.buckets[bid].trigrams[trigram] = {0, 10};

    idx.index_file_with_bucketed_trigrams(result);

    auto locs = idx.sharded_storage().search_trigram(trigram);
    ASSERT_EQ(locs.size(), 2u);
    EXPECT_EQ(locs[0].file_id, 1u);
}

// ---------------------------------------------------------------------------
// Concurrent read safety of ShardedTrigramStorage
// ---------------------------------------------------------------------------
TEST(ShardedTrigramStorageTest, ConcurrentReads) {
    ShardedTrigramStorage storage(256);

    // Populate with some data.
    BucketedTrigramResult result;
    result.file_id = 1;
    result.buckets.resize(256);

    uint32_t trigram = (uint32_t('t') << 16) | (uint32_t('s') << 8) | uint32_t('t');
    uint16_t bid = static_cast<uint16_t>(trigram & 255);
    result.buckets[bid].trigrams[trigram] = {0, 5, 10, 15, 20};
    storage.merge_bucketed_trigrams(result);

    // Launch multiple threads reading simultaneously.
    constexpr int kThreads = 4;
    constexpr int kIterations = 100;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&storage, trigram, &success_count]() {
            for (int i = 0; i < kIterations; ++i) {
                auto locs = storage.search_trigram(trigram);
                if (locs.size() == 5) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(success_count.load(), kThreads * kIterations);
}

// ---------------------------------------------------------------------------
// TrigramIndex clear
// ---------------------------------------------------------------------------
TEST(TrigramIndexTest, ClearResetsEverything) {
    TrigramIndex idx;
    idx.index_file(1, "function test value");
    idx.remove_file(1);
    idx.find_candidates("function");

    idx.clear();

    EXPECT_EQ(idx.file_count(), 0);
    EXPECT_EQ(idx.get_invalidation_count(), 0);
    EXPECT_TRUE(idx.find_candidates("function").empty());
}

// ---------------------------------------------------------------------------
// Unicode indexing via TrigramIndex
// ---------------------------------------------------------------------------
TEST(TrigramIndexTest, UnicodeIndexing) {
    TrigramIndex idx;
    // 4 CJK characters -> 2 Unicode trigrams
    std::string content = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97\xE7\xAC\xA6";
    idx.index_file(1, content);

    EXPECT_EQ(idx.file_count(), 1);
}

// ---------------------------------------------------------------------------
// Bulk indexing flag
// ---------------------------------------------------------------------------
TEST(TrigramIndexTest, BulkIndexingFlag) {
    TrigramIndex idx;
    idx.set_bulk_indexing(true);
    idx.index_file(1, "function test value");

    // During bulk indexing, cache should not be populated.
    // After disabling bulk indexing, search works normally.
    idx.set_bulk_indexing(false);

    auto candidates = idx.find_candidates("function");
    EXPECT_FALSE(candidates.empty());
}

TEST(TrigramHostileTest, NormalCodeIsNotHostile) {
    EXPECT_FALSE(is_trigram_hostile("package main\nfunc main() {}\n"));
}

TEST(TrigramHostileTest, MinifiedBundleIsHostile) {
    // One >4 KB line in a >64 KB file — the minified-bundle signature.
    std::string minified(128 * 1024, 'a');
    for (size_t i = 0; i < minified.size(); i += 10000) minified[i] = '\n';
    EXPECT_TRUE(is_trigram_hostile(minified));
}

TEST(TrigramHostileTest, SmallSingleLineFileIsNotHostile) {
    // Long-line rule only applies past the size floor; a small one-liner
    // (lockfile fragment, embedded data URI) stays trigram-indexed.
    std::string small(8 * 1024, 'b');
    EXPECT_FALSE(is_trigram_hostile(small));
}

TEST(TrigramHostileTest, OversizeFileIsHostileRegardlessOfLines) {
    std::string big;
    big.reserve(3 * 1024 * 1024);
    while (big.size() < 3 * 1024 * 1024) big += "short line\n";
    EXPECT_TRUE(is_trigram_hostile(big));
}

// ---------------------------------------------------------------------------
// has_high_entropy_section: payload-in-code detection.
// Two distinct payload signatures -- compressed/base64 (high byte entropy)
// and hash/hex dumps (LOW entropy: 16-symbol alphabet, ~4 bits/byte, so a
// pure entropy trigger misses them; the token-shape rule is what catches
// them). Both need >= 16 KB of qualifying blocks: a lone key or digest is
// normal code, not a payload.
// ---------------------------------------------------------------------------

namespace {

// Deterministic pseudo-random bytes (no <random>: fixed LCG, same output
// everywhere) restricted to a chosen alphabet.
std::string make_payload(size_t bytes, std::string_view alphabet) {
    std::string out;
    out.reserve(bytes);
    uint64_t state = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < bytes; ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        out.push_back(alphabet[(state >> 33) % alphabet.size()]);
    }
    return out;
}

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kHexAlphabet = "0123456789abcdef";

std::string normal_code(size_t approx_bytes) {
    std::string out;
    while (out.size() < approx_bytes) {
        out += "int compute_total(const std::vector<int>& xs) {\n"
               "    int total = 0;\n"
               "    for (int x : xs) total += x;\n"
               "    return total;\n"
               "}\n\n";
    }
    return out;
}

}  // namespace

TEST(HighEntropySectionTest, NormalCodeDoesNotTrigger) {
    EXPECT_FALSE(has_high_entropy_section(normal_code(64 * 1024)));
}

TEST(HighEntropySectionTest, Base64PayloadTriggers) {
    // 32 KB of base64 embedded in code: the compressed/base64 signature.
    std::string f = normal_code(32 * 1024);
    f += make_payload(32 * 1024, kBase64Alphabet);
    f += normal_code(32 * 1024);
    EXPECT_TRUE(has_high_entropy_section(f));
}

TEST(HighEntropySectionTest, HexDumpTriggersDespiteLowEntropy) {
    // 32 KB of hex ids: ~4 bits/byte, entropy alone would MISS this --
    // the digit-saturation rule is the discriminating signature. This is
    // the err-lookup blob shape.
    std::string f = normal_code(32 * 1024);
    f += make_payload(32 * 1024, kHexAlphabet);
    f += normal_code(32 * 1024);
    EXPECT_TRUE(has_high_entropy_section(f));
}

TEST(HighEntropySectionTest, StructuredQuotedHashTableTriggers) {
    // The realistic shape: a generated code file of INDENTED, QUOTED hex
    // literals. Quotes + indent + newline dilute token density to ~80%
    // and raise whitespace to ~12% -- a strict dense/starved rule misses
    // it (the first cut of this detector did); digit saturation is what
    // has to carry the classification.
    std::string f = normal_code(8 * 1024);
    f += "const char* kHashes[] = {\n";
    std::string hex = make_payload(32 * 32 * 1024 / 42, kHexAlphabet);
    for (size_t i = 0; i + 32 <= hex.size(); i += 32) {
        f += "    \"";
        f.append(hex, i, 32);
        f += "\",\n";
    }
    f += "};\n";
    f += normal_code(8 * 1024);
    EXPECT_TRUE(has_high_entropy_section(f));
}

TEST(HighEntropySectionTest, CjkTextDoesNotTrigger) {
    // UTF-8 CJK text clears 5.5 bits/byte on a raw byte histogram (lead +
    // continuation bytes spread wide) -- an entropy rule without the ASCII
    // gate classifies a Chinese README as a compressed payload. Pin the
    // negative: prose in any script is not a payload.
    std::string f;
    while (f.size() < 64 * 1024) {
        f += "\xe9\x80\x99\xe6\x98\xaf\xe4\xb8\x80\xe6\xae\xb5"
             "\xe6\xb8\xac\xe8\xa9\xa6\xe6\x96\x87\xe5\xad\x97"
             "\xef\xbc\x8c\xe7\x94\xa8\xe6\x96\xbc\xe9\xa9\x97"
             "\xe8\xad\x89\xe5\x81\xb5\xe6\xb8\xac\xe5\x99\xa8"
             "\xe3\x80\x82\n";
    }
    EXPECT_FALSE(has_high_entropy_section(f));
}

TEST(PostingsTokenCapPolicyTest, CodeGetsWideCeilingDataGetsCap) {
    // Harm-based policy: classifiers pick the ceiling, the ceiling always
    // exists. A payload the classifier cannot recognize is still bounded
    // by 4x on a code extension.
    EXPECT_EQ(postings_token_cap(/*is_code=*/false, false, 4096), 4096u);
    EXPECT_EQ(postings_token_cap(/*is_code=*/true, /*payload=*/true, 4096),
              4096u);
    EXPECT_EQ(postings_token_cap(/*is_code=*/true, /*payload=*/false, 4096),
              16384u);
    EXPECT_EQ(postings_token_cap(true, false, 0), 0u);   // 0 disables
    EXPECT_EQ(postings_token_cap(false, true, 0), 0u);
}

TEST(HighEntropySectionTest, SmallPayloadDoesNotTrigger) {
    // A single 8 KB blob (inlined key, one digest table) is below the
    // 16 KB payload floor: ordinary code, not a special case.
    std::string f = normal_code(64 * 1024);
    f += make_payload(8 * 1024, kBase64Alphabet);
    f += normal_code(64 * 1024);
    EXPECT_FALSE(has_high_entropy_section(f));
}

TEST(HighEntropySectionTest, SmallFileNeverTriggers) {
    EXPECT_FALSE(
        has_high_entropy_section(make_payload(8 * 1024, kBase64Alphabet)));
}

TEST(HighEntropySectionTest, ScatteredPayloadAccumulates) {
    // Five 4 KB payload blocks spread through the file: individually small,
    // 20 KB in total -- past the floor, so the special case fires.
    std::string f;
    for (int i = 0; i < 5; ++i) {
        f += normal_code(12 * 1024);
        f += make_payload(4 * 1024, kBase64Alphabet);
    }
    EXPECT_TRUE(has_high_entropy_section(f));
}

TEST(TrigramIndexTest, PayloadSectionFileFallsBackToUnfilteredCandidate) {
    // A quoted-hash table dodges the long-line hostile rule (42-byte
    // lines) but is per-occurrence poison: ~12 bytes per trigram
    // occurrence stored for content that matches nearly any query. The
    // entropy/digit classifier must route it to the same unfiltered
    // fallback -- stored nowhere, surfaced everywhere.
    TrigramIndex index;
    std::string table = "const char* kHashes[] = {\n";
    uint64_t state = 42;
    for (int line = 0; line < 1200; ++line) {
        table += "    \"";
        for (int i = 0; i < 32; ++i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            table += "0123456789abcdef"[(state >> 33) & 0xF];
        }
        table += "\",\n";
    }
    table += "};\n";
    index.index_file(FileID{9}, table);

    auto candidates = index.find_candidates("needle");
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0], FileID{9});
}

TEST(TrigramIndexTest, HostileFileFallsBackToUnfilteredCandidate) {
    TrigramIndex index;
    std::string minified(128 * 1024, 'x');
    for (size_t i = 0; i < minified.size(); i += 10000) minified[i] = '\n';
    index.index_file(FileID{7}, minified);

    // No trigram data was stored, but the file must still surface as a
    // candidate for any pattern — the prefilter may not hide it.
    auto candidates = index.find_candidates("needle");
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0], FileID{7});
}

TEST(TrigramIndexTest, RemovedUnfilteredFileStopsBeingACandidate) {
    TrigramIndex index;
    std::string minified(128 * 1024, 'x');
    for (size_t i = 0; i < minified.size(); i += 10000) minified[i] = '\n';
    index.index_file(FileID{7}, minified);
    index.remove_file(FileID{7});

    EXPECT_TRUE(index.find_candidates("needle").empty());
}

// -- TrigramIndex::narrow (coverage-scoped certified absence) -----------------

TEST(TrigramIndexNarrowTest, CertifiesAbsenceOnlyInCoveredFiles) {
    TrigramIndex index;
    index.index_file(FileID{1}, "func alpha() { return beta; }");

    auto n = index.narrow("gamma", /*case_insensitive=*/false);
    ASSERT_TRUE(n.informative());
    // File 1 is covered and holds none of the pattern's trigrams.
    EXPECT_TRUE(n.certifies_absent(FileID{1}));
    // File 2 was never trigram-indexed (bulk-indexed files never are) —
    // absence of data must not read as absence of the pattern.
    EXPECT_FALSE(n.certifies_absent(FileID{2}));
}

TEST(TrigramIndexNarrowTest, DoesNotCertifyPresentPattern) {
    TrigramIndex index;
    index.index_file(FileID{1}, "call handle_gadget now");

    auto n = index.narrow("handle_g", false);
    ASSERT_TRUE(n.informative());
    EXPECT_FALSE(n.certifies_absent(FileID{1}));
}

TEST(TrigramIndexNarrowTest, CaseInsensitiveQueriesAreUninformative) {
    // Stored trigrams keep original case; probing with a folded pattern
    // would fabricate absences, so ci queries certify nothing.
    TrigramIndex index;
    index.index_file(FileID{1}, "type PageWindow struct{}");

    EXPECT_FALSE(index.narrow("pagewindow", true).informative());
}

TEST(TrigramIndexNarrowTest, UnfilteredAndRemovedFilesAreNeverCertified) {
    TrigramIndex index;
    std::string minified(128 * 1024, 'x');
    for (size_t i = 0; i < minified.size(); i += 10000) minified[i] = '\n';
    index.index_file(FileID{7}, minified);   // Hostile → unfiltered.
    index.index_file(FileID{1}, "func alpha() {}");
    index.remove_file(FileID{1});

    auto n = index.narrow("needle", false);
    if (n.informative()) {
        EXPECT_FALSE(n.certifies_absent(FileID{7}));
        EXPECT_FALSE(n.certifies_absent(FileID{1}));
    }
}

TEST(TrigramIndexNarrowTest, AsciiPatternProbesUnicodeIndexedFiles) {
    // A file with any non-ASCII byte indexes ALL its trigrams (including
    // pure-ASCII windows) in the unicode map. An ASCII pattern probe must
    // still see them, or covered non-ASCII files get false absences.
    TrigramIndex index;
    index.index_file(FileID{1}, "// café utils\nfunc needle() {}\n");

    auto n = index.narrow("needle", false);
    ASSERT_TRUE(n.informative());
    EXPECT_FALSE(n.certifies_absent(FileID{1}));
}

}  // namespace
}  // namespace lci
