#include "spec_diff/tiers.h"
#include <gtest/gtest.h>

using spec_diff::FieldTier;
using spec_diff::TierMap;
using spec_diff::classify_path;
using spec_diff::normalize_indexes;

TEST(SpecDiffTiers, ClassifiesStableByDefault) {
    TierMap m;
    EXPECT_EQ(classify_path(m, "anything"), FieldTier::Stable);
}

TEST(SpecDiffTiers, ExplicitMappingsTakePrecedence) {
    TierMap m;
    m.ranked = {"results[].score"};
    m.timed  = {"elapsed_ms"};
    m.ids    = {"request_id"};
    m.ignore = {"server_pid"};
    m.stable = {"results[].file"};
    EXPECT_EQ(classify_path(m, "results[].score"), FieldTier::Ranked);
    EXPECT_EQ(classify_path(m, "elapsed_ms"),      FieldTier::Timed);
    EXPECT_EQ(classify_path(m, "request_id"),      FieldTier::Id);
    EXPECT_EQ(classify_path(m, "server_pid"),      FieldTier::Ignore);
    EXPECT_EQ(classify_path(m, "results[].file"),  FieldTier::Stable);
}

TEST(SpecDiffTiers, IgnoreOutranksOtherTiers) {
    // If the same path appears in both ignore and stable, ignore wins.
    TierMap m;
    m.ignore = {"x"};
    m.stable = {"x"};
    EXPECT_EQ(classify_path(m, "x"), FieldTier::Ignore);
}

TEST(SpecDiffTiers, NormalizeIndexesCollapsesNumericIndexes) {
    EXPECT_EQ(normalize_indexes("results[3].score"),  "results[].score");
    EXPECT_EQ(normalize_indexes("a[0].b[12].c"),      "a[].b[].c");
    EXPECT_EQ(normalize_indexes("plain.path"),        "plain.path");
    EXPECT_EQ(normalize_indexes("results[]"),         "results[]");
}

TEST(SpecDiffTiers, NormalizeIndexesLeavesNonNumericBracketsAlone) {
    // Non-numeric bracket content is left untouched (we only strip
    // integer indexes, not field-name brackets).
    EXPECT_EQ(normalize_indexes("a[name]"), "a[name]");
}

TEST(SpecDiffTiers, ClassifyAfterNormalizeMatchesWildcard) {
    TierMap m;
    m.ranked = {"results[].score"};
    EXPECT_EQ(classify_path(m, normalize_indexes("results[3].score")),
              FieldTier::Ranked);
}
