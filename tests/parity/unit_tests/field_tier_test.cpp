#include "diff_engine/field_tier.h"
#include <gtest/gtest.h>

using lci::parity::FieldTier;
using lci::parity::TierMap;
using lci::parity::classify_path;

TEST(FieldTier, ClassifiesStableByDefault) {
    TierMap m;
    EXPECT_EQ(classify_path(m, "anything"), FieldTier::Stable);
}

TEST(FieldTier, ExplicitMappingsTakePrecedence) {
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

TEST(FieldTier, ArrayWildcardMatchesAnyIndex) {
    // Internally we strip indexes to "[]" before lookup, so callers
    // should pass paths in array-wildcard form already. The classifier
    // just does literal lookup over the maps.
    TierMap m;
    m.ranked = {"results[].score"};
    EXPECT_EQ(classify_path(m, "results[].score"), FieldTier::Ranked);
    EXPECT_EQ(classify_path(m, "results[3].score"), FieldTier::Stable)
        << "Caller must normalize indexes before classifying";
}
