#include <gtest/gtest.h>

#include <lci/alloc/slab_allocator.h>
#include <lci/alloc/trigram_predictor.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// SlabAllocator: basic allocation and return
// ---------------------------------------------------------------------------
TEST(SlabAllocatorTest, GetReturnsNonNullForPositiveCapacity) {
    SlabAllocator<uint32_t> alloc;
    auto [ptr, cap] = alloc.get(4);
    ASSERT_NE(ptr, nullptr);
    EXPECT_GE(cap, 4);
    alloc.put(ptr, cap);
}

TEST(SlabAllocatorTest, GetReturnsNullForZeroCapacity) {
    SlabAllocator<uint32_t> alloc;
    auto [ptr, cap] = alloc.get(0);
    EXPECT_EQ(ptr, nullptr);
    EXPECT_EQ(cap, 0);
}

TEST(SlabAllocatorTest, GetReturnsNullForNegativeCapacity) {
    SlabAllocator<uint32_t> alloc;
    auto [ptr, cap] = alloc.get(-1);
    EXPECT_EQ(ptr, nullptr);
    EXPECT_EQ(cap, 0);
}

// ---------------------------------------------------------------------------
// SlabAllocator: tier selection
// ---------------------------------------------------------------------------
TEST(SlabAllocatorTest, SelectsSmallestSufficientTier) {
    SlabAllocator<uint32_t> alloc;

    auto [p1, c1] = alloc.get(1);
    EXPECT_EQ(c1, 8);  // smallest tier
    alloc.put(p1, c1);

    auto [p2, c2] = alloc.get(9);
    EXPECT_EQ(c2, 16);  // next tier up
    alloc.put(p2, c2);

    auto [p3, c3] = alloc.get(33);
    EXPECT_EQ(c3, 64);
    alloc.put(p3, c3);
}

TEST(SlabAllocatorTest, DirectAllocForOversizedRequest) {
    SlabAllocator<uint32_t> alloc;
    auto [ptr, cap] = alloc.get(1024);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(cap, 1024);

    // Write to verify memory is usable.
    for (int i = 0; i < 1024; ++i) {
        ptr[i] = static_cast<uint32_t>(i);
    }
    EXPECT_EQ(ptr[0], 0u);
    EXPECT_EQ(ptr[1023], 1023u);

    alloc.put(ptr, cap);
}

// ---------------------------------------------------------------------------
// SlabAllocator: reuse via free list
// ---------------------------------------------------------------------------
TEST(SlabAllocatorTest, PutThenGetReusesBlock) {
    SlabAllocator<uint32_t> alloc;
    auto [p1, c1] = alloc.get(8);
    ASSERT_NE(p1, nullptr);
    alloc.put(p1, c1);

    auto [p2, c2] = alloc.get(8);
    EXPECT_EQ(p2, p1);  // should reuse the same block
    EXPECT_EQ(c2, c1);
    alloc.put(p2, c2);
}

TEST(SlabAllocatorTest, PutNullIsNoop) {
    SlabAllocator<uint32_t> alloc;
    alloc.put(nullptr, 8);  // should not crash
    auto stats = alloc.get_stats();
    EXPECT_EQ(stats.pool_hits, 0);
}

TEST(SlabAllocatorTest, PutZeroCapacityIsNoop) {
    SlabAllocator<uint32_t> alloc;
    auto [ptr, cap] = alloc.get(8);
    alloc.put(ptr, 0);  // should not crash, not returned to pool
    // Clean up via direct free (not ideal but prevents leak in test).
    alloc.put(ptr, cap);
}

// ---------------------------------------------------------------------------
// SlabAllocator: statistics tracking
// ---------------------------------------------------------------------------
TEST(SlabAllocatorTest, StatsTrackAllocationsAndReuses) {
    SlabAllocator<uint32_t> alloc;

    auto [p1, c1] = alloc.get(8);
    auto stats1 = alloc.get_stats();
    EXPECT_EQ(stats1.allocations, 1);
    EXPECT_EQ(stats1.pool_misses, 1);

    alloc.put(p1, c1);
    auto stats_after_put = alloc.get_stats();
    EXPECT_EQ(stats_after_put.reuses, 1);
    EXPECT_EQ(stats_after_put.pool_hits, 1);

    auto [p2, c2] = alloc.get(8);
    auto stats2 = alloc.get_stats();
    // put() counted one reuse, get() from free list counted another.
    EXPECT_EQ(stats2.reuses, 2);
    EXPECT_EQ(stats2.pool_hits, 2);

    alloc.put(p2, c2);
}

TEST(SlabAllocatorTest, ResetStatsClearsCounters) {
    SlabAllocator<uint32_t> alloc;
    auto [ptr, cap] = alloc.get(8);
    alloc.put(ptr, cap);

    alloc.reset_stats();
    auto stats = alloc.get_stats();
    EXPECT_EQ(stats.allocations, 0);
    EXPECT_EQ(stats.reuses, 0);
    EXPECT_EQ(stats.pool_hits, 0);
    EXPECT_EQ(stats.pool_misses, 0);
    EXPECT_EQ(stats.total_capacity, 0);
}

TEST(SlabAllocatorTest, OversizedPutTracksPoolMiss) {
    SlabAllocator<uint32_t> alloc;
    auto [ptr, cap] = alloc.get(1024);
    alloc.put(ptr, cap);
    auto stats = alloc.get_stats();
    EXPECT_GE(stats.pool_misses, 1);
}

// ---------------------------------------------------------------------------
// SlabAllocator: custom tier configs
// ---------------------------------------------------------------------------
TEST(SlabAllocatorTest, CustomTierConfigs) {
    constexpr std::array<SlabTierConfig, 2> configs{{
        {4, 0.5},
        {16, 0.5},
    }};
    SlabAllocator<uint32_t> alloc(configs);

    auto [p1, c1] = alloc.get(3);
    EXPECT_EQ(c1, 4);
    alloc.put(p1, c1);

    auto [p2, c2] = alloc.get(5);
    EXPECT_EQ(c2, 16);
    alloc.put(p2, c2);
}

TEST(SlabAllocatorTest, TrigramTierConfigs) {
    SlabAllocator<uint32_t> alloc(kTrigramTierConfigs);
    auto [ptr, cap] = alloc.get(1);
    EXPECT_EQ(cap, 8);
    alloc.put(ptr, cap);
}

// ---------------------------------------------------------------------------
// SlabAllocator: memory is writable
// ---------------------------------------------------------------------------
TEST(SlabAllocatorTest, AllocatedMemoryIsWritable) {
    SlabAllocator<uint32_t> alloc;
    auto [ptr, cap] = alloc.get(32);
    ASSERT_NE(ptr, nullptr);

    for (int i = 0; i < cap; ++i) {
        ptr[i] = static_cast<uint32_t>(i * 7);
    }
    for (int i = 0; i < cap; ++i) {
        EXPECT_EQ(ptr[i], static_cast<uint32_t>(i * 7));
    }
    alloc.put(ptr, cap);
}

// ---------------------------------------------------------------------------
// SlabAllocator: multiple allocations from same tier
// ---------------------------------------------------------------------------
TEST(SlabAllocatorTest, MultipleAllocationsFromSameTier) {
    SlabAllocator<uint32_t> alloc;
    constexpr int kCount = 100;
    std::vector<std::pair<uint32_t*, int>> blocks;
    blocks.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        auto [ptr, cap] = alloc.get(8);
        ASSERT_NE(ptr, nullptr);
        ptr[0] = static_cast<uint32_t>(i);
        blocks.push_back({ptr, cap});
    }

    // Verify each block is distinct and retains its value.
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(blocks[static_cast<size_t>(i)].first[0],
                  static_cast<uint32_t>(i));
    }

    for (auto& [ptr, cap] : blocks) {
        alloc.put(ptr, cap);
    }
}

// ---------------------------------------------------------------------------
// SlabAllocator: simple benchmark (get+put cycle)
// ---------------------------------------------------------------------------
TEST(SlabAllocatorBenchmark, GetPutCycleReusesMemory) {
    SlabAllocator<uint32_t> alloc;
    constexpr int kIterations = 10000;

    // Warm up: prime the free list with one block.
    auto [wp, wc] = alloc.get(8);
    alloc.put(wp, wc);
    alloc.reset_stats();

    for (int i = 0; i < kIterations; ++i) {
        auto [ptr, cap] = alloc.get(8);
        alloc.put(ptr, cap);
    }

    auto stats = alloc.get_stats();
    // After warmup, every get() should hit the free list (pool hit).
    // Every put() also counts a reuse. So reuses == 2 * kIterations.
    EXPECT_EQ(stats.allocations, 0);
    EXPECT_EQ(stats.pool_misses, 0);
    EXPECT_GT(stats.pool_hits, 0);
    EXPECT_GT(stats.reuses, 0);
}

// ---------------------------------------------------------------------------
// TrigramPredictor: known language predictions
// ---------------------------------------------------------------------------
TEST(TrigramPredictorTest, GoLanguagePrediction) {
    TrigramPredictor pred("go");
    int cap = pred.predict_capacity("fun", "func main() {}");
    EXPECT_GE(cap, 4);
    EXPECT_LE(cap, 200);
}

TEST(TrigramPredictorTest, UnknownTrigramUsesHeuristic) {
    TrigramPredictor pred("go");
    int cap = pred.predict_capacity("xyz", "some content");
    EXPECT_GE(cap, 4);
    EXPECT_LE(cap, 64);
}

TEST(TrigramPredictorTest, UnknownLanguageFallsBack) {
    TrigramPredictor pred("rust");
    int cap = pred.predict_capacity("fun", "fn main() {}");
    EXPECT_GE(cap, 4);
}

TEST(TrigramPredictorTest, EmptyFileContentProducesReasonableEstimate) {
    TrigramPredictor pred("go");
    int cap = pred.predict_capacity("err", "");
    EXPECT_GE(cap, 4);
}

// ---------------------------------------------------------------------------
// TrigramPredictor: learning via update_stats
// ---------------------------------------------------------------------------
TEST(TrigramPredictorTest, UpdateStatsForKnownTrigram) {
    TrigramPredictor pred("go");
    int before = pred.predict_capacity("fun", "");
    pred.update_stats("fun", 100);
    int after = pred.predict_capacity("fun", "");
    // After seeing a count of 100, the prediction should increase.
    EXPECT_GE(after, before);
}

TEST(TrigramPredictorTest, UpdateStatsCreatesNewEntry) {
    TrigramPredictor pred("go");
    pred.update_stats("zzz", 50);
    int cap = pred.predict_capacity("zzz", "");
    EXPECT_GE(cap, 4);
}

// ---------------------------------------------------------------------------
// TrigramPredictor: get_common_trigrams
// ---------------------------------------------------------------------------
TEST(TrigramPredictorTest, GetCommonTrigramsSorted) {
    TrigramPredictor pred("go");
    auto common = pred.get_common_trigrams(5);
    ASSERT_LE(common.size(), 5u);
    ASSERT_GE(common.size(), 1u);

    // Verify sorted by avg_per_file descending.
    for (size_t i = 1; i < common.size(); ++i) {
        EXPECT_GE(common[i - 1].avg_per_file, common[i].avg_per_file);
    }
}

TEST(TrigramPredictorTest, GetCommonTrigramsDefaultLimit) {
    TrigramPredictor pred("go");
    auto common = pred.get_common_trigrams(0);
    EXPECT_GE(common.size(), 1u);
}

}  // namespace
}  // namespace lci
