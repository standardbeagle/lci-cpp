// tests/parity/unit_tests/ignore_baseline_test.cpp
//
// Unit tests for the BASE_IGNORE_NON_DETERMINISM contract: the centralized
// set of always-ignored non-deterministic fields (pid/timestamp/elapsed/etc.)
// and the merged_ignore() helper the diff engine uses to combine it with any
// per-spec extras.
//
// (The descriptor-linting tests that scanned the Go-parity descriptor tree were
// removed when the Go reference was retired — the migration is complete.)

#include "runner/ignore_baseline.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>
#include <vector>

TEST(IgnoreBaseline, BaseSetIsNonEmptyAndUnique) {
    using namespace lci::parity;
    std::set<std::string_view> seen;
    for (auto p : BASE_IGNORE_NON_DETERMINISM) {
        EXPECT_TRUE(seen.insert(p).second)
            << "duplicate entry in BASE_IGNORE_NON_DETERMINISM: " << p;
    }
    EXPECT_FALSE(seen.empty());
}

TEST(IgnoreBaseline, MergedIgnoreContainsBaseAndExtras) {
    using namespace lci::parity;
    std::vector<std::string> extras = {"foo", "bar"};
    auto merged = merged_ignore(extras);
    // BASE comes first, then extras
    ASSERT_EQ(merged.size(), BASE_IGNORE_NON_DETERMINISM.size() + 2);
    EXPECT_EQ(merged.back(), "bar");
    EXPECT_EQ(merged[merged.size() - 2], "foo");
    EXPECT_TRUE(is_base_ignore(merged.front()));
}

TEST(IgnoreBaseline, MergedIgnoreDedupesBaseOverlap) {
    using namespace lci::parity;
    // If a spec still lists BASE entries, merge must dedupe so canonicalize
    // doesn't get duplicates.
    std::vector<std::string> stale = {"pid", "timestamp", "custom_extra"};
    auto merged = merged_ignore(stale);
    EXPECT_EQ(merged.size(), BASE_IGNORE_NON_DETERMINISM.size() + 1);
    EXPECT_EQ(merged.back(), "custom_extra");
}
