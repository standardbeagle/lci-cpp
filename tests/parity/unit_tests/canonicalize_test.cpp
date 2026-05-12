#include "spec_diff/canonicalize.h"
#include "runner/descriptor.h"  // pulls spec_diff into lci::parity
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>

using lci::parity::canonicalize_json;
using lci::parity::canonicalize_text;
using lci::parity::CanonicalizeOptions;
using lci::parity::TextCanonicalizeOptions;
using nlohmann::json;

TEST(CanonicalizeJson, SortsObjectKeysRecursively) {
    auto in = json::parse(R"({"b":1,"a":{"y":2,"x":1}})");
    auto out = canonicalize_json(in, {});
    // The dump string is the canonical form
    EXPECT_EQ(out.dump(), R"({"a":{"x":1,"y":2},"b":1})");
}

TEST(CanonicalizeJson, NormalizesFloatsToSixSignificantDigits) {
    auto in = json::parse(R"({"score":0.123456789})");
    auto out = canonicalize_json(in, {});
    // 0.123457 — %.6g
    EXPECT_EQ(out["score"].get<std::string>(), "0.123457");
}

TEST(CanonicalizeJson, PreservesNumbersOnPreservedPaths) {
    auto in = json::parse(R"({"results":[{"score":0.987654}]})");
    CanonicalizeOptions opts;
    opts.preserve_number_paths = {"results[].score"};
    auto out = canonicalize_json(in, opts);
    // Still a number — diff engine ranked tier will apply tolerance.
    EXPECT_TRUE(out["results"][0]["score"].is_number());
    EXPECT_NEAR(out["results"][0]["score"].get<double>(), 0.987654, 1e-9);
}

TEST(CanonicalizeJson, KeepsIntsUnchanged) {
    auto in = json::parse(R"({"n":42})");
    auto out = canonicalize_json(in, {});
    EXPECT_EQ(out["n"].get<int64_t>(), 42);
}

TEST(CanonicalizeJson, StripsIgnoredJsonPaths) {
    auto in = json::parse(R"({"a":1,"server_pid":1234,"version":"x"})");
    CanonicalizeOptions opts;
    opts.ignore_paths = {"server_pid", "version"};
    auto out = canonicalize_json(in, opts);
    EXPECT_FALSE(out.contains("server_pid"));
    EXPECT_FALSE(out.contains("version"));
    EXPECT_EQ(out["a"].get<int>(), 1);
}

TEST(CanonicalizeJson, RewritesAbsoluteCorpusPathsToToken) {
    auto in = json::parse(R"({"results":[{"file":"/tmp/corpus-abc/src/a.go"}]})");
    CanonicalizeOptions opts;
    opts.corpus_prefix = "/tmp/corpus-abc";
    auto out = canonicalize_json(in, opts);
    EXPECT_EQ(out["results"][0]["file"].get<std::string>(),
              "${CORPUS}/src/a.go");
}

TEST(CanonicalizeText, TrimsTrailingWhitespacePerLine) {
    EXPECT_EQ(canonicalize_text("a   \nb\t \nc", {}), "a\nb\nc");
}

// -------- Text normalization tests --------

TEST(CanonicalizeText, ScrubsTimingMillisecondsByDefault) {
    TextCanonicalizeOptions opts;
    EXPECT_EQ(canonicalize_text("Found 2 results in 1.0ms (mode)", opts),
              "Found 2 results in <MS> (mode)");
    EXPECT_EQ(canonicalize_text("Found 2 results in 2.4ms\n", opts),
              "Found 2 results in <MS>\n");
    EXPECT_EQ(canonicalize_text("elapsed: 134ms done", opts),
              "elapsed: <MS> done");
    // Multiple occurrences on one line both replaced.
    EXPECT_EQ(canonicalize_text("a 1.0ms b 2.5ms c", opts),
              "a <MS> b <MS> c");
}

TEST(CanonicalizeText, TimingScrubCanBeDisabled) {
    TextCanonicalizeOptions opts;
    opts.scrub_timing = false;
    EXPECT_EQ(canonicalize_text("Found 2 results in 1.0ms", opts),
              "Found 2 results in 1.0ms");
}

TEST(CanonicalizeText, RewritesCorpusPrefixToToken) {
    TextCanonicalizeOptions opts;
    opts.corpus_prefix = "/home/u/corpus";
    EXPECT_EQ(canonicalize_text("Root Path: /home/u/corpus\n", opts),
              "Root Path: ${CORPUS}\n");
    EXPECT_EQ(canonicalize_text("/home/u/corpus/a.go:1\n", opts),
              "${CORPUS}/a.go:1\n");
}

TEST(CanonicalizeText, EmptyCorpusPrefixIsIgnored) {
    TextCanonicalizeOptions opts;
    opts.corpus_prefix = "";
    EXPECT_EQ(canonicalize_text("/home/u/corpus/a.go", opts),
              "/home/u/corpus/a.go");
}

TEST(CanonicalizeText, StripsLinesContainingSubstring) {
    TextCanonicalizeOptions opts;
    opts.strip_lines = {"DEBUG:", "Building index...", "Linking symbols"};
    std::string in =
        "DEBUG: verbose=false, compact=false\n"
        "Building index...\n"
        "Linking symbols and graph...\n"
        "Found 2 results in 1.0ms (mode)\n"
        "ok\n";
    std::string expected =
        "Found 2 results in <MS> (mode)\n"
        "ok\n";
    EXPECT_EQ(canonicalize_text(in, opts), expected);
}

TEST(CanonicalizeText, StripsLinesAlsoWorksOnFinalLineWithoutNewline) {
    TextCanonicalizeOptions opts;
    opts.strip_lines = {"DEBUG:"};
    EXPECT_EQ(canonicalize_text("DEBUG: tail", opts), "");
    EXPECT_EQ(canonicalize_text("keep\nDEBUG: tail", opts), "keep\n");
}

TEST(CanonicalizeText, StripsLeadingEmojiPrefix) {
    TextCanonicalizeOptions opts;
    opts.strip_emoji_prefix = true;
    // Common Go emoji prefixes followed by a space.
    EXPECT_EQ(canonicalize_text("\xE2\x9C\x85 Configuration file is valid\n", opts),
              "Configuration file is valid\n");
    EXPECT_EQ(canonicalize_text("\xF0\x9F\x93\x8D Config source: .lci.kdl\n", opts),
              "Config source: .lci.kdl\n");
    EXPECT_EQ(canonicalize_text("\xF0\x9F\x93\x8A Settings: 10000\n", opts),
              "Settings: 10000\n");
    EXPECT_EQ(canonicalize_text("\xE2\x9A\xA0\xEF\xB8\x8F  Warnings:\n", opts),
              "Warnings:\n");
    EXPECT_EQ(canonicalize_text("\xE2\x9C\x93 All checks passed\n", opts),
              "All checks passed\n");
}

TEST(CanonicalizeText, EmojiPrefixDisabledByDefault) {
    TextCanonicalizeOptions opts;  // default: strip_emoji_prefix=false
    EXPECT_EQ(canonicalize_text("\xE2\x9C\x85 ok\n", opts),
              "\xE2\x9C\x85 ok\n");
}

TEST(CanonicalizeText, AppliesRegexReplacements) {
    TextCanonicalizeOptions opts;
    opts.replace.push_back({R"(\(integrated mode[^)]*\))", "(MODE)"});
    opts.replace.push_back({R"(\(standard mode\))", "(MODE)"});
    EXPECT_EQ(canonicalize_text("Found 2 in 1ms (integrated mode - foo)\n", opts),
              "Found 2 in <MS> (MODE)\n");
    EXPECT_EQ(canonicalize_text("Found 2 in 2ms (standard mode)\n", opts),
              "Found 2 in <MS> (MODE)\n");
}

TEST(CanonicalizeText, CombinesAllNormalizers) {
    TextCanonicalizeOptions opts;
    opts.corpus_prefix = "/abs/corpus";
    opts.strip_lines = {"DEBUG:", "=== Direct Matches ==="};
    opts.replace.push_back({R"(\(integrated mode[^)]*\))", "(MODE)"});
    opts.replace.push_back({R"(\(standard mode\))", "(MODE)"});

    std::string go_in =
        "DEBUG: verbose=false\n"
        "Found 2 results in 1.0ms (integrated mode - no assembly matches)\n"
        "\n"
        "=== Direct Matches ===\n"
        "d.rs:1\n"
        "\n"
        "b.py:1\n";
    std::string cpp_in =
        "Found 2 results in 2.4ms (standard mode)\n"
        "\n"
        "/abs/corpus/d.rs:1\n"
        "\n"
        "/abs/corpus/b.py:1\n";

    auto go_canon  = canonicalize_text(go_in, opts);
    auto cpp_canon = canonicalize_text(cpp_in, opts);
    // After normalization the file paths in the cpp side rewrite to ${CORPUS}/d.rs.
    // Go side has bare basenames; this test just confirms each side normalizes
    // correctly — convergence to identical strings is the descriptor's job
    // when paths are also bare on the Go side. Here we accept divergence in
    // that one dimension and verify the other normalizers fired.
    EXPECT_EQ(go_canon,
              "Found 2 results in <MS> (MODE)\n"
              "\n"
              "d.rs:1\n"
              "\n"
              "b.py:1\n");
    EXPECT_EQ(cpp_canon,
              "Found 2 results in <MS> (MODE)\n"
              "\n"
              "${CORPUS}/d.rs:1\n"
              "\n"
              "${CORPUS}/b.py:1\n");
}

// ---------- sort_lines ----------

TEST(CanonicalizeTextSortLines, EmptyInputProducesEmptyOutput) {
    TextCanonicalizeOptions opts;
    opts.sort_lines = true;
    EXPECT_EQ(canonicalize_text("", opts), "");
}

TEST(CanonicalizeTextSortLines, SortsLinesAlphabetically) {
    TextCanonicalizeOptions opts;
    opts.sort_lines = true;
    auto out = canonicalize_text("c\nb\na\n", opts);
    EXPECT_EQ(out, "a\nb\nc\n");
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 3);
    EXPECT_LT(out.find("a\n"), out.find("b\n"));
    EXPECT_LT(out.find("b\n"), out.find("c\n"));
}

TEST(CanonicalizeTextSortLines, PreservesLastLineNoNewline) {
    TextCanonicalizeOptions opts;
    opts.sort_lines = true;
    // Last input line "a" has no trailing newline. After sort it ends up
    // first; the line that originally had a newline ("b") ends up last,
    // and the trailing newline travels with it.
    auto out = canonicalize_text("b\na", opts);
    EXPECT_EQ(out, "ab\n");
}

TEST(CanonicalizeTextSortLines, SortRunsAfterStrip) {
    TextCanonicalizeOptions opts;
    opts.sort_lines = true;
    opts.strip_lines = {"SKIP"};
    auto out = canonicalize_text("c\nSKIP\na\nb\n", opts);
    EXPECT_EQ(out, "a\nb\nc\n");
    EXPECT_EQ(out.find("SKIP"), std::string::npos);
}

TEST(CanonicalizeTextSortLines, SortRunsAfterReplace) {
    TextCanonicalizeOptions opts;
    opts.sort_lines = true;
    opts.replace.push_back({R"(zebra)", "aardvark"});
    auto out = canonicalize_text("zebra\nbear\n", opts);
    // Replace runs per-line before sort. "zebra" -> "aardvark"; sort puts it first.
    EXPECT_EQ(out, "aardvark\nbear\n");
}

TEST(CanonicalizeTextSortLines, BlankLinesSortFirst) {
    TextCanonicalizeOptions opts;
    opts.sort_lines = true;
    auto out = canonicalize_text("c\n\nb\na\n\n", opts);
    // Two blanks + three letters; blanks sort first.
    EXPECT_EQ(out.substr(0, 2), "\n\n");
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 5);
}

// ---------- collapse_blank_lines ----------

TEST(CanonicalizeTextCollapse, DisabledByDefault) {
    TextCanonicalizeOptions opts;
    auto out = canonicalize_text("a\n\n\n\nb\n", opts);
    EXPECT_EQ(out, "a\n\n\n\nb\n");
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 5);
}

TEST(CanonicalizeTextCollapse, CollapsesTwoBlanksToOne) {
    TextCanonicalizeOptions opts;
    opts.collapse_blank_lines = true;
    auto out = canonicalize_text("a\n\n\nb\n", opts);
    EXPECT_EQ(out, "a\n\nb\n");
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 3);
}

TEST(CanonicalizeTextCollapse, CollapsesLongRunsToOne) {
    TextCanonicalizeOptions opts;
    opts.collapse_blank_lines = true;
    auto out = canonicalize_text("x\n\n\n\n\n\n\ny\n", opts);
    EXPECT_EQ(out, "x\n\ny\n");
}

TEST(CanonicalizeTextCollapse, CollapsesLeadingBlanks) {
    TextCanonicalizeOptions opts;
    opts.collapse_blank_lines = true;
    auto out = canonicalize_text("\n\n\nfirst\n", opts);
    EXPECT_EQ(out, "\nfirst\n");
}

TEST(CanonicalizeTextCollapse, CollapsesTrailingBlanks) {
    TextCanonicalizeOptions opts;
    opts.collapse_blank_lines = true;
    auto out = canonicalize_text("last\n\n\n\n", opts);
    EXPECT_EQ(out, "last\n\n");
}

TEST(CanonicalizeTextCollapse, WhitespaceOnlyLinesCollapse) {
    TextCanonicalizeOptions opts;
    opts.collapse_blank_lines = true;
    auto out = canonicalize_text("a\n   \n\nb\n", opts);
    EXPECT_EQ(out, "a\n\nb\n");
}

TEST(CanonicalizeTextCollapse, CombinedWithSortLines) {
    TextCanonicalizeOptions opts;
    opts.sort_lines = true;
    opts.collapse_blank_lines = true;
    auto out = canonicalize_text("c\n\n\n\nb\n\n\na\n", opts);
    // Collapse first: "c\n\nb\n\na\n" -> 5 lines including 2 blanks.
    // Sort: 2 empty strings first, then a, b, c.
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 5);
    EXPECT_EQ(out.substr(0, 2), "\n\n");
    // a, b, c each present once.
    EXPECT_EQ(out.find("a\n"), 2u);
    EXPECT_EQ(out.find("b\n"), 4u);
    EXPECT_EQ(out.find("c\n"), 6u);
}
