// KDL reader tests. The grammar is shared by .lci.kdl, the user defaults
// file, and the attribute ruleset shipped in the binary, so a token that
// parses here parses in all three.

#include <gtest/gtest.h>

#include <lci/kdl.h>

namespace lci {
namespace {

std::vector<kdl::Node> parse_ok(std::string_view src) {
    std::string error;
    auto nodes = kdl::parse(src, error);
    EXPECT_TRUE(error.empty()) << error;
    return nodes;
}

TEST(KdlTest, ParsesArgsAndChildren) {
    auto nodes = parse_ok(R"(
        project {
            root "/tmp/repo"
            workers 8
            enabled true
        }
    )");
    ASSERT_EQ(1u, nodes.size());
    EXPECT_EQ("project", nodes[0].name);
    ASSERT_EQ(3u, nodes[0].children.size());
    EXPECT_EQ("/tmp/repo", nodes[0].children[0].first_string());
    EXPECT_EQ(8, static_cast<int>(nodes[0].children[1].args[0].num_val));
    EXPECT_TRUE(nodes[0].children[2].args[0].bool_val);
}

// -- Properties ----------------------------------------------------------------

// `key=value` is how an attribute states its rank without spending a child
// node on a single scalar.
TEST(KdlTest, ParsesProperties) {
    auto nodes = parse_ok(R"(
        attributes {
            vendored rank=0 active=false {
                dir "vendor"
            }
        }
    )");
    ASSERT_EQ(1u, nodes.size());
    ASSERT_EQ(1u, nodes[0].children.size());
    const auto& vendored = nodes[0].children[0];
    EXPECT_EQ("vendored", vendored.name);
    EXPECT_EQ(0, vendored.int_prop("rank", -1));
    ASSERT_NE(vendored.prop("active"), nullptr);
    EXPECT_FALSE(vendored.prop("active")->bool_val);
    ASSERT_NE(vendored.child("dir"), nullptr);
    EXPECT_EQ("vendor", vendored.child("dir")->first_string());
}

// The lookahead that separates a property from a child node of the same name.
TEST(KdlTest, IdentWithoutEqualsStaysAChildNode) {
    auto nodes = parse_ok(R"(
        attr {
            rank 3
        }
    )");
    ASSERT_EQ(1u, nodes.size());
    EXPECT_EQ(-1, nodes[0].int_prop("rank", -1)) << "should be a child, not a prop";
    ASSERT_NE(nodes[0].child("rank"), nullptr);
    EXPECT_EQ(3, static_cast<int>(nodes[0].child("rank")->args[0].num_val));
}

TEST(KdlTest, PropertyValueIsRequired) {
    std::string error;
    kdl::parse("attr rank= { }", error);
    EXPECT_NE(error.find("rank"), std::string::npos) << error;
}

TEST(KdlTest, MissingPropertyReadsAsFallback) {
    auto nodes = parse_ok("attr other=1");
    ASSERT_EQ(1u, nodes.size());
    EXPECT_EQ(42, nodes[0].int_prop("rank", 42));
    EXPECT_EQ(nodes[0].prop("rank"), nullptr);
}

// -- Errors --------------------------------------------------------------------

// KDL v2 spells booleans '#true'. lci does not accept them, and the error
// has to name the token so the writer can see what to change.
TEST(KdlTest, RejectsKdlV2BoolsByName) {
    std::string error;
    kdl::parse("attr enabled #true", error);
    EXPECT_NE(error.find("#true"), std::string::npos) << error;
}

TEST(KdlTest, ReportsUnclosedBlockWithItsOpeningLine) {
    std::string error;
    kdl::parse("a {\n  b \"x\"\n", error);
    EXPECT_NE(error.find("unclosed block"), std::string::npos) << error;
}

TEST(KdlTest, ReportsUnterminatedString) {
    std::string error;
    kdl::parse("a \"unterminated", error);
    EXPECT_FALSE(error.empty());
}

TEST(KdlTest, SkipsLineAndBlockComments) {
    auto nodes = parse_ok(R"(
        // leading comment
        a "one"   // trailing
        /* block
           spanning lines */
        b "two"
    )");
    ASSERT_EQ(2u, nodes.size());
    EXPECT_EQ("a", nodes[0].name);
    EXPECT_EQ("two", nodes[1].first_string());
}

TEST(KdlTest, CollectsEveryStringArgument) {
    auto nodes = parse_ok(R"(dir "vendor" "node_modules" "third_party")");
    ASSERT_EQ(1u, nodes.size());
    EXPECT_EQ(std::vector<std::string>({"vendor", "node_modules", "third_party"}),
              nodes[0].string_args());
}

}  // namespace
}  // namespace lci
