#include "spec_diff/assert_matches.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

using spec_diff::SpecDescriptor;
using spec_diff::assert_matches;
using spec_diff::assert_matches_with_golden;
using spec_diff::canonicalize;
using spec_diff::diff;

namespace {

// Tiny RAII helper: write text to a temp file, return its path, unlink
// on destruction. Avoids pulling in std::filesystem temp_directory plumbing
// for what's a one-line fixture.
class TempGolden {
public:
    explicit TempGolden(const std::string& contents) {
        char tmpl[] = "/tmp/spec_diff_golden_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) std::abort();
        path_ = tmpl;
        ::close(fd);
        std::ofstream f(path_, std::ios::binary | std::ios::trunc);
        f << contents;
    }
    ~TempGolden() { std::remove(path_.c_str()); }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

} // namespace

// ---------- JSON-mode success / failure ----------

TEST(SpecDiffAssertMatches, JsonExactMatchPasses) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    std::string golden = R"({"a":1,"b":2})";
    auto r = assert_matches_with_golden(R"({"b":2,"a":1})", golden, d);
    EXPECT_TRUE(r.passed) << (r.reasons.empty() ? "" : r.reasons[0]);
}

TEST(SpecDiffAssertMatches, JsonStableMismatchFailsWithReason) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    auto r = assert_matches_with_golden(R"({"a":1,"b":3})",
                                         R"({"a":1,"b":2})", d);
    EXPECT_FALSE(r.passed);
    ASSERT_FALSE(r.reasons.empty());
    EXPECT_NE(r.reasons[0].find("b"), std::string::npos);
}

TEST(SpecDiffAssertMatches, JsonAppliesTierMapForRankedTolerance) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    d.tiers.stable = {"results[].file"};
    d.tiers.ranked = {"results[].score"};
    d.score_abs    = 0.01;
    std::string golden = R"({"results":[{"file":"x","score":0.91}]})";
    std::string actual = R"({"results":[{"file":"x","score":0.91005}]})";
    auto r = assert_matches_with_golden(actual, golden, d);
    EXPECT_TRUE(r.passed) << (r.reasons.empty() ? "" : r.reasons[0]);
}

TEST(SpecDiffAssertMatches, JsonStripsIgnoredPaths) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    d.tiers.ignore = {"server_pid"};
    auto r = assert_matches_with_golden(
        R"({"a":1,"server_pid":2})",
        R"({"a":1,"server_pid":99})",
        d);
    EXPECT_TRUE(r.passed);
}

TEST(SpecDiffAssertMatches, JsonRewritesCorpusPrefixOnBothSides) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    d.corpus_prefix = "/tmp/abc";
    // Golden uses the literal prefix; actual uses the same prefix.
    // Both should rewrite to ${CORPUS} and compare equal.
    auto r = assert_matches_with_golden(
        R"({"file":"/tmp/abc/x.go"})",
        R"({"file":"/tmp/abc/x.go"})",
        d);
    EXPECT_TRUE(r.passed);
}

TEST(SpecDiffAssertMatches, JsonStripsPreambleLinesBeforeParse) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    d.json_preamble_strip = {"DEBUG:"};
    std::string actual =
        "DEBUG: verbose=true\n"
        "{\"a\":1}\n";
    std::string golden = R"({"a":1})";
    auto r = assert_matches_with_golden(actual, golden, d);
    EXPECT_TRUE(r.passed) << (r.reasons.empty() ? "" : r.reasons[0]);
}

TEST(SpecDiffAssertMatches, JsonParseErrorRaises) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    EXPECT_THROW(
        assert_matches_with_golden("not-json", R"({"a":1})", d),
        std::runtime_error);
}

TEST(SpecDiffAssertMatches, CanonicalizeJsonWrapperReturnsCanonicalJson) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    d.tiers.ignore = {"server_pid"};
    auto value = canonicalize(R"({"b":2,"a":1,"server_pid":123})", d);
    ASSERT_TRUE(std::holds_alternative<nlohmann::json>(value));
    auto json = std::get<nlohmann::json>(value);
    EXPECT_EQ(json.dump(), R"({"a":1,"b":2})");
}

TEST(SpecDiffAssertMatches, CanonicalizeTextWrapperReturnsCanonicalText) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Text;
    auto value = canonicalize("Found 1 in 5.4ms\n", d);
    ASSERT_TRUE(std::holds_alternative<std::string>(value));
    EXPECT_EQ(std::get<std::string>(value), "Found 1 in <MS>\n");
}

TEST(SpecDiffAssertMatches, DiffWrapperUsesDescriptorJsonSemantics) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    d.tiers.stable = {"results[].file"};
    d.tiers.ranked = {"results[].score"};
    auto expected = canonicalize(R"({"results":[{"file":"x","score":0.91}]})", d);
    auto actual = canonicalize(R"({"results":[{"file":"x","score":0.91005}]})", d);
    auto result = diff(expected, actual, d);
    EXPECT_TRUE(result.passed) << (result.reasons.empty() ? "" : result.reasons[0]);
}

// ---------- Text-mode ----------

TEST(SpecDiffAssertMatches, TextExactMatchPasses) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Text;
    auto r = assert_matches_with_golden("hello\n", "hello\n", d);
    EXPECT_TRUE(r.passed);
}

TEST(SpecDiffAssertMatches, TextScrubsTimingByDefault) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Text;
    auto r = assert_matches_with_golden(
        "Found 1 in 5.4ms\n",
        "Found 1 in 1.0ms\n",
        d);
    EXPECT_TRUE(r.passed);
}

TEST(SpecDiffAssertMatches, TextMismatchPopulatesUnifiedDiff) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Text;
    auto r = assert_matches_with_golden("apple\n", "banana\n", d);
    EXPECT_FALSE(r.passed);
    EXPECT_FALSE(r.unified_diff.empty());
}

// ---------- File-loading variant ----------

TEST(SpecDiffAssertMatches, LoadsGoldenFromFile) {
    TempGolden g(R"({"a":1,"b":2})");
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    auto r = assert_matches(R"({"a":1,"b":2})", d, g.path());
    EXPECT_TRUE(r.passed);
}

TEST(SpecDiffAssertMatches, MissingGoldenFileRaises) {
    SpecDescriptor d;
    d.parse = SpecDescriptor::Parse::Json;
    EXPECT_THROW(
        assert_matches("{}", d, "/nonexistent/path/golden.json"),
        std::runtime_error);
}
