#include "diff_engine/canonicalize.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using lci::parity::canonicalize_json;
using lci::parity::CanonicalizeOptions;
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
    using lci::parity::canonicalize_text;
    EXPECT_EQ(canonicalize_text("a   \nb\t \nc"), "a\nb\nc");
}
