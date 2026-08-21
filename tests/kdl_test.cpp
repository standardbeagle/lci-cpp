// KDL reader tests. One grammar serves .lci.kdl and the user defaults file,
// so a token that parses here parses in both.

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
