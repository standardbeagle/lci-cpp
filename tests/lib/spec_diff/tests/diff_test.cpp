#include "spec_diff/diff.h"
#include <gtest/gtest.h>

using spec_diff::diff;
using spec_diff::DiffOptions;
using spec_diff::DiffResult;
using spec_diff::TierMap;
using nlohmann::json;

TEST(SpecDiffDiff, EqualStableJsonPasses) {
    auto a = json::parse(R"({"file":"x","line":1})");
    auto b = a;
    auto r = diff(a, b, {});
    EXPECT_TRUE(r.passed);
    EXPECT_TRUE(r.reasons.empty());
    EXPECT_TRUE(r.unified_diff.empty());
}

TEST(SpecDiffDiff, StableMismatchFailsWithUnifiedDiff) {
    auto a = json::parse(R"({"file":"x","line":1})");
    auto b = json::parse(R"({"file":"x","line":2})");
    auto r = diff(a, b, {});
    EXPECT_FALSE(r.passed);
    ASSERT_FALSE(r.reasons.empty());
    EXPECT_NE(r.reasons[0].find("line"), std::string::npos);
    EXPECT_FALSE(r.unified_diff.empty());
}

TEST(SpecDiffDiff, TimedFieldWithinRangePasses) {
    auto a = json::parse(R"({"elapsed_ms":42})");
    auto b = json::parse(R"({"elapsed_ms":99})");
    DiffOptions opts;
    opts.tiers.timed = {"elapsed_ms"};
    opts.timed_max_ms = 60000;
    auto r = diff(a, b, opts);
    EXPECT_TRUE(r.passed) << (r.reasons.empty() ? "" : r.reasons[0]);
}

TEST(SpecDiffDiff, TimedFieldOutOfRangeFails) {
    auto a = json::parse(R"({"elapsed_ms":42})");
    auto b = json::parse(R"({"elapsed_ms":120000})");
    DiffOptions opts;
    opts.tiers.timed = {"elapsed_ms"};
    opts.timed_max_ms = 60000;
    auto r = diff(a, b, opts);
    EXPECT_FALSE(r.passed);
}

TEST(SpecDiffDiff, IgnoredFieldDoesNotAffectResult) {
    auto a = json::parse(R"({"server_pid":1,"x":42})");
    auto b = json::parse(R"({"server_pid":2,"x":42})");
    DiffOptions opts;
    opts.tiers.ignore = {"server_pid"};
    auto r = diff(a, b, opts);
    EXPECT_TRUE(r.passed);
}

TEST(SpecDiffDiff, RankedFieldScoreWithinTolerance) {
    auto a = json::parse(R"({"results":[{"file":"x","line":1,"score":0.91}]})");
    auto b = json::parse(R"({"results":[{"file":"x","line":1,"score":0.91005}]})");
    DiffOptions opts;
    opts.tiers.stable = {"results[].file","results[].line"};
    opts.tiers.ranked = {"results[].score"};
    opts.score_abs    = 0.01;
    auto r = diff(a, b, opts);
    EXPECT_TRUE(r.passed) << (r.reasons.empty() ? "" : r.reasons[0]);
}

TEST(SpecDiffDiff, RankedFieldScoreBeyondToleranceFails) {
    auto a = json::parse(R"({"results":[{"file":"x","line":1,"score":0.91}]})");
    auto b = json::parse(R"({"results":[{"file":"x","line":1,"score":0.5}]})");
    DiffOptions opts;
    opts.tiers.stable = {"results[].file","results[].line"};
    opts.tiers.ranked = {"results[].score"};
    opts.score_abs    = 0.01;
    auto r = diff(a, b, opts);
    EXPECT_FALSE(r.passed);
}

TEST(SpecDiffDiff, IdFieldFormatMatchPasses) {
    auto a = json::parse(R"({"request_id":"abc-123"})");
    auto b = json::parse(R"({"request_id":"xyz-987"})");
    DiffOptions opts;
    opts.tiers.ids = {"request_id"};
    opts.id_pattern = R"(^[a-z]+-[0-9]+$)";
    auto r = diff(a, b, opts);
    EXPECT_TRUE(r.passed);
}

TEST(SpecDiffDiff, IdFieldFormatMismatchFails) {
    auto a = json::parse(R"({"request_id":"abc-123"})");
    auto b = json::parse(R"({"request_id":"NOT_AN_ID"})");
    DiffOptions opts;
    opts.tiers.ids = {"request_id"};
    opts.id_pattern = R"(^[a-z]+-[0-9]+$)";
    auto r = diff(a, b, opts);
    EXPECT_FALSE(r.passed);
}

TEST(SpecDiffDiff, ArrayLengthMismatchFails) {
    auto a = json::parse(R"({"results":[1,2,3]})");
    auto b = json::parse(R"({"results":[1,2]})");
    auto r = diff(a, b, {});
    EXPECT_FALSE(r.passed);
    ASSERT_FALSE(r.reasons.empty());
    EXPECT_NE(r.reasons[0].find("array length"), std::string::npos);
}

TEST(SpecDiffDiff, TypeMismatchAtLeafFails) {
    auto a = json::parse(R"({"x":1})");
    auto b = json::parse(R"({"x":"1"})");
    auto r = diff(a, b, {});
    EXPECT_FALSE(r.passed);
    ASSERT_FALSE(r.reasons.empty());
    EXPECT_NE(r.reasons[0].find("type mismatch"), std::string::npos);
}

TEST(SpecDiffDiff, CompareAliasMatchesDiff) {
    auto a = json::parse(R"({"x":1})");
    auto b = json::parse(R"({"x":2})");
    auto r1 = spec_diff::diff(a, b, {});
    auto r2 = spec_diff::compare(a, b, {});
    EXPECT_EQ(r1.passed, r2.passed);
    EXPECT_EQ(r1.reasons.size(), r2.reasons.size());
}
