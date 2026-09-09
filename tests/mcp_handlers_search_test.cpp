#include <gtest/gtest.h>

#include <algorithm>

#include <lci/mcp/handlers_core_shared.h>

namespace lci {
namespace mcp {
namespace {

bool has_ext(const std::vector<std::string>& exts, const std::string& ext) {
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

// language_ext_table() is derived from kLangMap (include/lci/language_map.h)
// rather than an independent hard-coded list, so it cannot drift from the
// canonical classification. kLangMap deliberately resolves ".h" to C++ (the
// permissive header convention documented there), not to plain C.
TEST(McpHandlersSearchTest, HeaderExtensionResolvesToCppNotC) {
    const auto& table = language_ext_table();

    auto cpp_it = table.find("cpp");
    ASSERT_NE(cpp_it, table.end());
    EXPECT_TRUE(has_ext(cpp_it->second, "h"));

    auto c_it = table.find("c");
    ASSERT_NE(c_it, table.end());
    EXPECT_FALSE(has_ext(c_it->second, "h"));
}

TEST(McpHandlersSearchTest, CppAliasMatchesCanonicalCppEntry) {
    const auto& table = language_ext_table();
    auto cpp_it = table.find("cpp");
    auto plusplus_it = table.find("c++");
    ASSERT_NE(cpp_it, table.end());
    ASSERT_NE(plusplus_it, table.end());
    EXPECT_EQ(cpp_it->second, plusplus_it->second);
}

// kLangMap carries .pyx/.pxd for Python (Cython sources); the previous
// hand-maintained alias table omitted both.
TEST(McpHandlersSearchTest, PythonIncludesCythonExtensions) {
    const auto& table = language_ext_table();
    auto it = table.find("python");
    ASSERT_NE(it, table.end());
    EXPECT_TRUE(has_ext(it->second, "pyx"));
    EXPECT_TRUE(has_ext(it->second, "pxd"));
}

}  // namespace
}  // namespace mcp
}  // namespace lci
