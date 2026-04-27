#include "diff_engine/diff.h"
#include <gtest/gtest.h>

using lci::parity::compare;
using lci::parity::DiffOptions;
using lci::parity::DiffResult;
using lci::parity::TierMap;
using nlohmann::json;

TEST(Diff, EqualStableJsonPasses) {
    auto a = json::parse(R"({"file":"x","line":1})");
    auto b = a;
    auto r = compare(a, b, {});
    EXPECT_TRUE(r.passed);
    EXPECT_TRUE(r.reasons.empty());
}

TEST(Diff, StableMismatchFails) {
    auto a = json::parse(R"({"file":"x","line":1})");
    auto b = json::parse(R"({"file":"x","line":2})");
    auto r = compare(a, b, {});
    EXPECT_FALSE(r.passed);
    ASSERT_FALSE(r.reasons.empty());
    EXPECT_NE(r.reasons[0].find("line"), std::string::npos);
}

TEST(Diff, TimedFieldWithinRangePasses) {
    auto a = json::parse(R"({"elapsed_ms":42})");
    auto b = json::parse(R"({"elapsed_ms":99})");
    DiffOptions opts;
    opts.tiers.timed = {"elapsed_ms"};
    opts.timed_max_ms = 60000;
    auto r = compare(a, b, opts);
    EXPECT_TRUE(r.passed) << (r.reasons.empty() ? "" : r.reasons[0]);
}

TEST(Diff, TimedFieldOutOfRangeFails) {
    auto a = json::parse(R"({"elapsed_ms":42})");
    auto b = json::parse(R"({"elapsed_ms":120000})");
    DiffOptions opts;
    opts.tiers.timed = {"elapsed_ms"};
    opts.timed_max_ms = 60000;
    auto r = compare(a, b, opts);
    EXPECT_FALSE(r.passed);
}

TEST(Diff, IgnoredFieldDoesNotAffectResult) {
    auto a = json::parse(R"({"server_pid":1,"x":42})");
    auto b = json::parse(R"({"server_pid":2,"x":42})");
    DiffOptions opts;
    opts.tiers.ignore = {"server_pid"};
    auto r = compare(a, b, opts);
    EXPECT_TRUE(r.passed);
}

TEST(Diff, RankedFieldScoreWithinTolerance) {
    auto a = json::parse(R"({"results":[{"file":"x","line":1,"score":0.91}]})");
    auto b = json::parse(R"({"results":[{"file":"x","line":1,"score":0.91005}]})");
    DiffOptions opts;
    opts.tiers.stable = {"results[].file","results[].line"};
    opts.tiers.ranked = {"results[].score"};
    opts.score_abs    = 0.01;
    auto r = compare(a, b, opts);
    EXPECT_TRUE(r.passed) << (r.reasons.empty() ? "" : r.reasons[0]);
}

TEST(Diff, RankedFieldScoreBeyondToleranceFails) {
    auto a = json::parse(R"({"results":[{"file":"x","line":1,"score":0.91}]})");
    auto b = json::parse(R"({"results":[{"file":"x","line":1,"score":0.5}]})");
    DiffOptions opts;
    opts.tiers.stable = {"results[].file","results[].line"};
    opts.tiers.ranked = {"results[].score"};
    opts.score_abs    = 0.01;
    auto r = compare(a, b, opts);
    EXPECT_FALSE(r.passed);
}

TEST(Diff, IdFieldFormatMatchPasses) {
    auto a = json::parse(R"({"request_id":"abc-123"})");
    auto b = json::parse(R"({"request_id":"xyz-987"})");
    DiffOptions opts;
    opts.tiers.ids = {"request_id"};
    opts.id_pattern = R"(^[a-z]+-[0-9]+$)";
    auto r = compare(a, b, opts);
    EXPECT_TRUE(r.passed);
}

TEST(Diff, IdFieldFormatMismatchFails) {
    auto a = json::parse(R"({"request_id":"abc-123"})");
    auto b = json::parse(R"({"request_id":"NOT_AN_ID"})");
    DiffOptions opts;
    opts.tiers.ids = {"request_id"};
    opts.id_pattern = R"(^[a-z]+-[0-9]+$)";
    auto r = compare(a, b, opts);
    EXPECT_FALSE(r.passed);
}
