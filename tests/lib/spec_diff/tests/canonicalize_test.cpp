#include "spec_diff/canonicalize.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using spec_diff::canonicalize_json;
using spec_diff::canonicalize_text;
using spec_diff::strip_preamble_lines;
using spec_diff::CanonicalizeOptions;
using spec_diff::TextCanonicalizeOptions;
using nlohmann::json;

// ---------- canonicalize_json ----------

TEST(SpecDiffCanonicalizeJson, SortsObjectKeysRecursively) {
    auto in = json::parse(R"({"b":1,"a":{"y":2,"x":1}})");
    auto out = canonicalize_json(in, {});
    EXPECT_EQ(out.dump(), R"({"a":{"x":1,"y":2},"b":1})");
}

TEST(SpecDiffCanonicalizeJson, NormalizesFloatsToSixSignificantDigits) {
    auto in = json::parse(R"({"score":0.123456789})");
    auto out = canonicalize_json(in, {});
    EXPECT_EQ(out["score"].get<std::string>(), "0.123457");
}

TEST(SpecDiffCanonicalizeJson, PreservesNumbersOnPreservedPaths) {
    auto in = json::parse(R"({"results":[{"score":0.987654}]})");
    CanonicalizeOptions opts;
    opts.preserve_number_paths = {"results[].score"};
    auto out = canonicalize_json(in, opts);
    EXPECT_TRUE(out["results"][0]["score"].is_number());
    EXPECT_NEAR(out["results"][0]["score"].get<double>(), 0.987654, 1e-9);
}

TEST(SpecDiffCanonicalizeJson, KeepsIntsUnchanged) {
    auto in = json::parse(R"({"n":42})");
    auto out = canonicalize_json(in, {});
    EXPECT_EQ(out["n"].get<int64_t>(), 42);
}

TEST(SpecDiffCanonicalizeJson, StripsIgnoredJsonPaths) {
    auto in = json::parse(R"({"a":1,"server_pid":1234,"version":"x"})");
    CanonicalizeOptions opts;
    opts.ignore_paths = {"server_pid", "version"};
    auto out = canonicalize_json(in, opts);
    EXPECT_FALSE(out.contains("server_pid"));
    EXPECT_FALSE(out.contains("version"));
    EXPECT_EQ(out["a"].get<int>(), 1);
}

TEST(SpecDiffCanonicalizeJson, RewritesAbsoluteCorpusPathsToToken) {
    auto in = json::parse(R"({"results":[{"file":"/tmp/corpus-abc/src/a.go"}]})");
    CanonicalizeOptions opts;
    opts.corpus_prefix = "/tmp/corpus-abc";
    auto out = canonicalize_json(in, opts);
    EXPECT_EQ(out["results"][0]["file"].get<std::string>(),
              "${CORPUS}/src/a.go");
}

TEST(SpecDiffCanonicalizeJson, SortArraysSortsByCanonicalDump) {
    auto in = json::parse(R"({"symbols":[{"name":"b"},{"name":"a"},{"name":"c"}]})");
    CanonicalizeOptions opts;
    opts.sort_array_paths = {"symbols"};
    auto out = canonicalize_json(in, opts);
    EXPECT_EQ(out["symbols"][0]["name"].get<std::string>(), "a");
    EXPECT_EQ(out["symbols"][1]["name"].get<std::string>(), "b");
    EXPECT_EQ(out["symbols"][2]["name"].get<std::string>(), "c");
}

// ---------- canonicalize_text ----------

TEST(SpecDiffCanonicalizeText, TrimsTrailingWhitespacePerLine) {
    EXPECT_EQ(canonicalize_text("a   \nb\t \nc", {}), "a\nb\nc");
}

TEST(SpecDiffCanonicalizeText, ScrubsTimingMillisecondsByDefault) {
    TextCanonicalizeOptions opts;
    EXPECT_EQ(canonicalize_text("Found 2 results in 1.0ms (mode)", opts),
              "Found 2 results in <MS> (mode)");
    EXPECT_EQ(canonicalize_text("elapsed: 134ms done", opts),
              "elapsed: <MS> done");
    EXPECT_EQ(canonicalize_text("a 1.0ms b 2.5ms c", opts),
              "a <MS> b <MS> c");
}

TEST(SpecDiffCanonicalizeText, TimingScrubCanBeDisabled) {
    TextCanonicalizeOptions opts;
    opts.scrub_timing = false;
    EXPECT_EQ(canonicalize_text("Found 2 results in 1.0ms", opts),
              "Found 2 results in 1.0ms");
}

TEST(SpecDiffCanonicalizeText, RewritesCorpusPrefixToToken) {
    TextCanonicalizeOptions opts;
    opts.corpus_prefix = "/home/u/corpus";
    EXPECT_EQ(canonicalize_text("Root Path: /home/u/corpus\n", opts),
              "Root Path: ${CORPUS}\n");
    EXPECT_EQ(canonicalize_text("/home/u/corpus/a.go:1\n", opts),
              "${CORPUS}/a.go:1\n");
}

TEST(SpecDiffCanonicalizeText, EmptyCorpusPrefixIsIgnored) {
    TextCanonicalizeOptions opts;
    opts.corpus_prefix = "";
    EXPECT_EQ(canonicalize_text("/home/u/corpus/a.go", opts),
              "/home/u/corpus/a.go");
}

TEST(SpecDiffCanonicalizeText, StripsLinesContainingSubstring) {
    TextCanonicalizeOptions opts;
    opts.strip_lines = {"DEBUG:", "Building index...", "Linking symbols"};
    std::string in =
        "DEBUG: verbose=false\n"
        "Building index...\n"
        "Linking symbols and graph...\n"
        "Found 2 results in 1.0ms (mode)\n"
        "ok\n";
    std::string expected =
        "Found 2 results in <MS> (mode)\n"
        "ok\n";
    EXPECT_EQ(canonicalize_text(in, opts), expected);
}

TEST(SpecDiffCanonicalizeText, StripsFinalLineWithoutNewline) {
    TextCanonicalizeOptions opts;
    opts.strip_lines = {"DEBUG:"};
    EXPECT_EQ(canonicalize_text("DEBUG: tail", opts), "");
    EXPECT_EQ(canonicalize_text("keep\nDEBUG: tail", opts), "keep\n");
}

TEST(SpecDiffCanonicalizeText, StripsLeadingEmojiPrefix) {
    TextCanonicalizeOptions opts;
    opts.strip_emoji_prefix = true;
    EXPECT_EQ(canonicalize_text("\xE2\x9C\x85 Configuration file is valid\n", opts),
              "Configuration file is valid\n");
    EXPECT_EQ(canonicalize_text("\xF0\x9F\x93\x8D Config source: .lci.kdl\n", opts),
              "Config source: .lci.kdl\n");
    EXPECT_EQ(canonicalize_text("\xE2\x9A\xA0\xEF\xB8\x8F  Warnings:\n", opts),
              "Warnings:\n");
    EXPECT_EQ(canonicalize_text("\xE2\x9C\x93 All checks passed\n", opts),
              "All checks passed\n");
}

TEST(SpecDiffCanonicalizeText, EmojiPrefixDisabledByDefault) {
    TextCanonicalizeOptions opts;
    EXPECT_EQ(canonicalize_text("\xE2\x9C\x85 ok\n", opts),
              "\xE2\x9C\x85 ok\n");
}

TEST(SpecDiffCanonicalizeText, AppliesRegexReplacements) {
    TextCanonicalizeOptions opts;
    opts.replace.push_back({R"(\(integrated mode[^)]*\))", "(MODE)"});
    opts.replace.push_back({R"(\(standard mode\))", "(MODE)"});
    EXPECT_EQ(canonicalize_text("Found 2 in 1ms (integrated mode - foo)\n", opts),
              "Found 2 in <MS> (MODE)\n");
    EXPECT_EQ(canonicalize_text("Found 2 in 2ms (standard mode)\n", opts),
              "Found 2 in <MS> (MODE)\n");
}

// ---------- strip_preamble_lines ----------

TEST(SpecDiffStripPreamble, NoOpWhenPatternsEmpty) {
    EXPECT_EQ(strip_preamble_lines("DEBUG: x\nkeep\n", {}),
              "DEBUG: x\nkeep\n");
}

TEST(SpecDiffStripPreamble, DropsMatchingLinesAndKeepsTimingTokensIntact) {
    // strip_preamble_lines must NOT scrub timing tokens — that would
    // corrupt the JSON the runner is about to parse.
    std::string in =
        "DEBUG: t=1.0ms\n"
        "{\"elapsed_ms\":1.0,\"ok\":true}\n";
    std::string out = strip_preamble_lines(in, {"DEBUG:"});
    EXPECT_EQ(out, "{\"elapsed_ms\":1.0,\"ok\":true}\n");
}

TEST(SpecDiffStripPreamble, KeepsAllNonMatchingLinesUnchanged) {
    // No matches => identity transform (modulo trailing-whitespace trim
    // which canonicalize_text always applies).
    EXPECT_EQ(strip_preamble_lines("a\nb\nc\n", {"NOMATCH"}),
              "a\nb\nc\n");
}
