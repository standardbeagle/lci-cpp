#include <gtest/gtest.h>

#include <algorithm>

#include <lci/config.h>
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/handlers_core_shared.h>
#include <lci/search/search_engine.h>

#include <nlohmann/json.hpp>

#include "unique_temp.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

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

// -- per-hit `callers` count (MCP-4) ------------------------------------------
//
// `callers` is the incoming-reference count MINUS the symbol's own
// definition-site reference. Go methods (and Python/JS/TS functions) emit a
// self `usage` reference at the definition line, so raw incoming_ref_count
// reports 1 for an uncalled method; the per-hit field must report 0. A method
// with two real call sites reports 2. The definition self-reference stays in
// the refs total (criterion 4) — only this per-hit count excludes it.
class SearchCallersFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_search_callers_");
        std::filesystem::create_directories(temp_dir_);
        write_file(temp_dir_ / "main.go",
                   "package main\n"
                   "\n"
                   "type T struct{}\n"
                   "\n"
                   "func (t *T) uncalled() int { return 0 }\n"
                   "\n"
                   "func (t *T) target() int { return 1 }\n"
                   "\n"
                   "func (t *T) callerA() int { return t.target() }\n"
                   "\n"
                   "func (t *T) callerB() int { return t.target() }\n");

        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());
        search_engine_ = std::make_unique<SearchEngine>(*indexer_);
    }

    void TearDown() override {
        search_engine_.reset();
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    static void write_file(const std::filesystem::path& path,
                           const std::string& content) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        out << content;
    }

    // Runs search and returns the `callers` value for the single hit whose
    // enclosing symbol is `sym_name`. The field follows the existing
    // omit-at-zero convention (docs/TOOLS.md), so its absence reports 0.
    int search_callers(const std::string& pattern,
                       const std::string& sym_name) {
        nlohmann::json params;
        params["pattern"] = pattern;
        auto result = handle_search(params, *indexer_, search_engine_.get());
        EXPECT_FALSE(result.is_error) << result.text;
        auto json = nlohmann::json::parse(result.text);
        for (const auto& group : json["results"]) {
            for (const auto& hit : group["hits"]) {
                if (hit.contains("sym") &&
                    hit["sym"].get<std::string>() == sym_name) {
                    if (!hit.contains("callers")) return 0;
                    return hit["callers"].get<int>();
                }
            }
        }
        ADD_FAILURE() << "no hit enclosed by symbol " << sym_name
                      << " for pattern " << pattern << ": " << result.text;
        return -1;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SearchEngine> search_engine_;
};

TEST_F(SearchCallersFixture, UncalledMethodReportsZeroCallers) {
    // At HEAD the definition self-reference makes this report 1.
    EXPECT_EQ(search_callers("uncalled", "uncalled"), 0);
}

TEST_F(SearchCallersFixture, MethodWithTwoCallSitesReportsTwoCallers) {
    EXPECT_EQ(search_callers("target", "target"), 2);
}

}  // namespace
}  // namespace mcp
}  // namespace lci
