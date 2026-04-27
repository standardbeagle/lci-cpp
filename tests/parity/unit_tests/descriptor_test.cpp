#include "runner/descriptor.h"
#include <gtest/gtest.h>

using lci::parity::Descriptor;
using lci::parity::Mode;
using lci::parity::ParseStyle;
using lci::parity::parse_descriptor;

static const char* kSampleCli = R"({
  "id": "cli/search/json-basic",
  "mode": "cli",
  "corpus": "lci-go-repo",
  "go_binary": "${LCI_GO}",
  "cpp_binary": "${LCI_CPP}",
  "invocation": {
    "args": ["search", "--json", "MasterIndex"],
    "env": {"LCI_NO_DAEMON": "1"},
    "cwd": "${CORPUS}"
  },
  "capture": ["stdout","exit"],
  "parse": "json",
  "tiers": {
    "stable": ["results[].file","results[].line"],
    "ranked": ["results[].score"],
    "timed":  ["elapsed_ms"],
    "ids":    ["request_id"],
    "ignore": ["server_pid","version"]
  },
  "tolerances": {"score_abs": 0.01, "timed_max_ms": 60000},
  "expect_exit": 0
})";

TEST(Descriptor, ParsesAllFields) {
    auto d = parse_descriptor(kSampleCli);
    EXPECT_EQ(d.id, "cli/search/json-basic");
    EXPECT_EQ(d.mode, Mode::Cli);
    EXPECT_EQ(d.corpus, "lci-go-repo");
    EXPECT_EQ(d.invocation.args.size(), 3u);
    EXPECT_EQ(d.invocation.args[1], "--json");
    EXPECT_EQ(d.invocation.env.at("LCI_NO_DAEMON"), "1");
    EXPECT_EQ(d.parse, ParseStyle::Json);
    EXPECT_DOUBLE_EQ(d.tolerances.score_abs, 0.01);
    EXPECT_EQ(d.tolerances.timed_max_ms, 60000);
    EXPECT_EQ(d.expect_exit, 0);
}

TEST(Descriptor, RejectsUnknownMode) {
    std::string bad = R"({"id":"x","mode":"wat","corpus":"c","invocation":{"args":[]}})";
    EXPECT_THROW(parse_descriptor(bad), std::runtime_error);
}

TEST(Descriptor, MissingIdFieldThrows) {
    std::string bad = R"({"mode":"cli","corpus":"c","invocation":{"args":[]}})";
    EXPECT_THROW(parse_descriptor(bad), std::runtime_error);
}
