#include <gtest/gtest.h>

#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/config.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/semantic_annotator.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/server.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace lci {
namespace mcp {
namespace {

// =============================================================================
// Semantic annotations handler tests
// =============================================================================

class SemanticAnnotationsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        annotator_ = std::make_unique<SemanticAnnotator>();

        // Create symbols with @lci annotations
        Symbol sym1;
        sym1.name = "handleRequest";
        sym1.type = SymbolType::Function;
        sym1.file_id = FileID{1};
        sym1.line = 5;
        sym1.end_line = 20;
        symbols_.push_back(sym1);

        Symbol sym2;
        sym2.name = "processData";
        sym2.type = SymbolType::Function;
        sym2.file_id = FileID{1};
        sym2.line = 25;
        sym2.end_line = 40;
        symbols_.push_back(sym2);

        std::string content =
            "// @lci:labels[api,public]\n"
            "// @lci:category[endpoint]\n"
            "// @lci:tags[team=backend]\n"
            "func handleRequest() {}\n"
            "\n"
            "// some other code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// more code\n"
            "// @lci:labels[internal,worker]\n"
            "// @lci:category[processing]\n"
            "func processData() {}\n";

        annotator_->extract_annotations(FileID{1}, "handler.go", content,
                                        symbols_);
    }

    std::unique_ptr<SemanticAnnotator> annotator_;
    std::vector<Symbol> symbols_;
};

TEST_F(SemanticAnnotationsTest, RequiresLabelOrCategory) {
    nlohmann::json params;
    auto result = handle_semantic_annotations(params, *annotator_, nullptr);
    EXPECT_TRUE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json["error"].get<std::string>().find("label") !=
                std::string::npos);
}

TEST_F(SemanticAnnotationsTest, QueryByLabel) {
    nlohmann::json params;
    params["label"] = "api";
    auto result = handle_semantic_annotations(params, *annotator_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("annotations"));
    EXPECT_TRUE(json.contains("total_count"));
}

TEST_F(SemanticAnnotationsTest, QueryByLabelFindsMatches) {
    nlohmann::json params;
    params["label"] = "api";
    auto result = handle_semantic_annotations(params, *annotator_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    int total = json["total_count"].get<int>();
    EXPECT_GE(total, 1);
    if (total > 0) {
        auto& first = json["annotations"][0];
        EXPECT_EQ(first["symbol_name"].get<std::string>(), "handleRequest");
    }
}

TEST_F(SemanticAnnotationsTest, QueryByNonexistentLabelReturnsEmpty) {
    nlohmann::json params;
    params["label"] = "nonexistent_label_xyz";
    auto result = handle_semantic_annotations(params, *annotator_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_count"].get<int>(), 0);
}

TEST_F(SemanticAnnotationsTest, MaxResultsClamps) {
    nlohmann::json params;
    params["label"] = "api";
    params["max_results"] = 1;
    auto result = handle_semantic_annotations(params, *annotator_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_LE(json["total_count"].get<int>(), 1);
}

TEST_F(SemanticAnnotationsTest, CategoryAloneIsValid) {
    nlohmann::json params;
    params["category"] = "endpoint";
    auto result = handle_semantic_annotations(params, *annotator_, nullptr);
    EXPECT_FALSE(result.is_error);
}

TEST_F(SemanticAnnotationsTest, CategoryQueryFindsMatches) {
    // Verify the category branch actually returns annotated symbols
    // (was previously a comment-only stub).
    nlohmann::json params;
    params["category"] = "endpoint";
    auto result = handle_semantic_annotations(params, *annotator_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    int total = json["total_count"].get<int>();
    EXPECT_GE(total, 1);
    bool found = false;
    for (const auto& a : json["annotations"]) {
        if (a.value("symbol_name", "") == "handleRequest") {
            EXPECT_EQ(a.value("category", ""), "endpoint");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SemanticAnnotationsTest, GetSymbolsByCategoryDirect) {
    auto syms = annotator_->get_symbols_by_category("processing");
    ASSERT_EQ(syms.size(), 1u);
    EXPECT_EQ(syms[0]->name, "processData");
    EXPECT_EQ(syms[0]->annotation.category, "processing");
}

TEST_F(SemanticAnnotationsTest, GetSymbolsByCategoryUnknownReturnsEmpty) {
    auto syms = annotator_->get_symbols_by_category("does-not-exist");
    EXPECT_TRUE(syms.empty());
}

// PopulateFromIndex test: build a real MasterIndex on a tiny temp project
// with @lci: annotations, run populate, assert annotations land.
TEST(SemanticAnnotatorIntegration, PopulateFromIndexLoadsAnnotations) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() /
               ("lci_populate_test_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(tmp);

    // Cleanup at scope end
    struct Cleanup {
        fs::path p;
        ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); }
    } _{tmp};

    {
        std::ofstream f(tmp / "main.go");
        f << "// @lci:labels[handler,api]\n"
          << "// @lci:category[mcp-api]\n"
          << "func handleRequest() {}\n"
          << "\n"
          << "func main() {}\n";
    }

    Config cfg;
    cfg.project.root = tmp.string();
    MasterIndex idx(cfg);
    ASSERT_TRUE(idx.index_directory(tmp.string()));

    SemanticAnnotator ann;
    int processed = ann.populate_from_index(idx);
    EXPECT_GE(processed, 1);

    auto by_label = ann.get_symbols_by_label("handler");
    ASSERT_FALSE(by_label.empty());
    EXPECT_EQ(by_label[0]->name, "handleRequest");

    auto by_cat = ann.get_symbols_by_category("mcp-api");
    ASSERT_FALSE(by_cat.empty());
    EXPECT_EQ(by_cat[0]->name, "handleRequest");
    EXPECT_EQ(by_cat[0]->annotation.category, "mcp-api");
}

// =============================================================================
// Side effects handler tests
// =============================================================================

class SideEffectsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        analyzer_ = std::make_unique<SideEffectAnalyzer>("go");

        // Create a pure function
        analyzer_->begin_function("pureFunc", "math.go", 1, 10);
        analyzer_->add_parameter("x", 0);
        analyzer_->record_access("x", {}, AccessType::Read, 2, 1);
        pure_info_ = analyzer_->end_function();

        // Create an impure function with global write
        analyzer_->begin_function("impureFunc", "state.go", 1, 15);
        analyzer_->record_access("globalVar", {}, AccessType::Write, 5, 1);
        impure_info_ = analyzer_->end_function();

        // Create a function with IO
        analyzer_->begin_function("ioFunc", "io.go", 1, 20);
        analyzer_->record_function_call("fmt.Println", "fmt", false, 5, 1);
        io_info_ = analyzer_->end_function();
    }

    std::unique_ptr<SideEffectAnalyzer> analyzer_;
    SideEffectInfo pure_info_;
    SideEffectInfo impure_info_;
    SideEffectInfo io_info_;
};

TEST_F(SideEffectsTest, DefaultModeIsSummary) {
    nlohmann::json params;
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["mode"].get<std::string>(), "summary");
    EXPECT_TRUE(json.contains("summary"));
}

TEST_F(SideEffectsTest, SummaryShowsCounts) {
    nlohmann::json params;
    params["mode"] = "summary";
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    auto& summary = json["summary"];
    EXPECT_GE(summary["total_functions"].get<int>(), 3);
}

TEST_F(SideEffectsTest, PureModeFiltersPure) {
    nlohmann::json params;
    params["mode"] = "pure";
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["mode"].get<std::string>(), "pure");
    for (const auto& r : json["results"]) {
        EXPECT_TRUE(r["is_pure"].get<bool>());
    }
}

TEST_F(SideEffectsTest, ImpureModeFiltersImpure) {
    nlohmann::json params;
    params["mode"] = "impure";
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["mode"].get<std::string>(), "impure");
    for (const auto& r : json["results"]) {
        EXPECT_FALSE(r["is_pure"].get<bool>());
    }
}

TEST_F(SideEffectsTest, FileModeRequiresPath) {
    nlohmann::json params;
    params["mode"] = "file";
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_TRUE(result.is_error);
}

TEST_F(SideEffectsTest, FileModeFilters) {
    nlohmann::json params;
    params["mode"] = "file";
    params["file_path"] = "math.go";
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["mode"].get<std::string>(), "file");
    for (const auto& r : json["results"]) {
        EXPECT_EQ(r["file_path"].get<std::string>(), "math.go");
    }
}

TEST_F(SideEffectsTest, CategoryModeRequiresCategory) {
    nlohmann::json params;
    params["mode"] = "category";
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_TRUE(result.is_error);
}

TEST_F(SideEffectsTest, UnknownModeReturnsError) {
    nlohmann::json params;
    params["mode"] = "nonexistent";
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_TRUE(result.is_error);
}

TEST_F(SideEffectsTest, SymbolModeRequiresName) {
    nlohmann::json params;
    params["mode"] = "symbol";
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_TRUE(result.is_error);
}

TEST_F(SideEffectsTest, IncludeReasonsFlag) {
    nlohmann::json params;
    params["mode"] = "impure";
    params["include_reasons"] = true;
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_FALSE(result.is_error);
}

TEST_F(SideEffectsTest, IncludeConfidenceFlag) {
    nlohmann::json params;
    params["mode"] = "pure";
    params["include_confidence"] = true;
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    for (const auto& r : json["results"]) {
        EXPECT_TRUE(r.contains("confidence"));
    }
}

// =============================================================================
// Code insight handler tests
// =============================================================================

class CodeInsightTest : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() /
                    "lci_code_insight_test";
        std::filesystem::create_directories(temp_dir_);

        write_file(temp_dir_ / "main.go",
                   "package main\n\nfunc main() {\n\tprintln(\"hello\")\n}\n");
        write_file(temp_dir_ / "handler.go",
                   "package main\n\nfunc handleRequest() {}\n");

        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());
        engine_ = std::make_unique<CodebaseIntelligenceEngine>();
    }

    void TearDown() override {
        engine_.reset();
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

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<CodebaseIntelligenceEngine> engine_;
};

// NOTE post-FIX-D.1.C: handle_code_insight emits LCF text (not JSON) to match
// Go's wire format. Tests assert LCF header + section presence instead of
// parsing JSON. The 6 mcp/code_insight/* parity descriptors lock byte-level
// agreement with Go.

TEST_F(CodeInsightTest, DefaultModeIsOverview) {
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("LCF/1.0"), std::string::npos);
    EXPECT_NE(result.text.find("mode=overview"), std::string::npos);
    EXPECT_NE(result.text.find("== REPOSITORY MAP =="), std::string::npos);
    EXPECT_NE(result.text.find("== HEALTH =="), std::string::npos);
}

TEST_F(CodeInsightTest, InvalidModeReturnsError) {
    nlohmann::json params;
    params["mode"] = "nonexistent_mode";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_TRUE(result.is_error);
}

TEST_F(CodeInsightTest, OverviewModeProducesResponse) {
    nlohmann::json params;
    params["mode"] = "overview";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("mode=overview"), std::string::npos);
    EXPECT_NE(result.text.find("tier=1"), std::string::npos);
}

TEST_F(CodeInsightTest, StatisticsModeWorks) {
    nlohmann::json params;
    params["mode"] = "statistics";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("mode=statistics"), std::string::npos);
    EXPECT_NE(result.text.find("== STATISTICS =="), std::string::npos);
}

TEST_F(CodeInsightTest, StructureModeWorks) {
    nlohmann::json params;
    params["mode"] = "structure";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("mode=structure"), std::string::npos);
    EXPECT_NE(result.text.find("== STRUCTURE =="), std::string::npos);
}

TEST_F(CodeInsightTest, DetailedModeWorks) {
    // detailed with default analysis (modules) now actually dispatches to
    // ModuleAnalyzer (was a silent overview fallback before).
    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "modules";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("mode=detailed"), std::string::npos);
    EXPECT_NE(result.text.find("sub=modules"), std::string::npos);
    EXPECT_NE(result.text.find("== MODULES =="), std::string::npos);
}

TEST_F(CodeInsightTest, DetailedLayersDispatches) {
    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "layers";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("sub=layers"), std::string::npos);
    EXPECT_NE(result.text.find("== LAYERS =="), std::string::npos);
}

TEST_F(CodeInsightTest, DetailedFeaturesDispatches) {
    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "features";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("sub=features"), std::string::npos);
    EXPECT_NE(result.text.find("== FEATURES =="), std::string::npos);
}

TEST_F(CodeInsightTest, DetailedTermsDispatches) {
    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "terms";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("sub=terms"), std::string::npos);
    EXPECT_NE(result.text.find("== TERMS =="), std::string::npos);
}

TEST_F(CodeInsightTest, DetailedInvalidSubReturnsError) {
    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "bogus";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_TRUE(result.is_error);
}

TEST_F(CodeInsightTest, UnifiedModeWorks) {
    nlohmann::json params;
    params["mode"] = "unified";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("mode=unified"), std::string::npos);
    EXPECT_NE(result.text.find("== REPOSITORY MAP =="), std::string::npos);
    EXPECT_NE(result.text.find("== HEALTH =="), std::string::npos);
    EXPECT_NE(result.text.find("== MODULES =="), std::string::npos);
    EXPECT_NE(result.text.find("== STATISTICS =="), std::string::npos);
}

// The base fixture's temp_dir_ is not a git repo. Both git modes must now
// FAIL FAST (real git wiring) instead of emitting a fake zero-STATISTICS block.
TEST_F(CodeInsightTest, GitAnalyzeFailsFastOnNonGitDir) {
    nlohmann::json params;
    params["mode"] = "git_analyze";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.text.find("git"), std::string::npos);
}

TEST_F(CodeInsightTest, GitHotspotsFailsFastOnNonGitDir) {
    nlohmann::json params;
    params["mode"] = "git_hotspots";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.text.find("git"), std::string::npos);
}

// Real-repo fixture: git-init a throwaway repo with several commits so both git
// modes have history to analyze. Asserts the real LCF sections appear (no mocks,
// real git::Analyzer + git::FrequencyAnalyzer).
class CodeInsightGitTest : public ::testing::Test {
  protected:
    void SetUp() override {
        repo_ = std::filesystem::temp_directory_path() / "lci_ci_git_repo";
        std::filesystem::remove_all(repo_);
        std::filesystem::create_directories(repo_);
        ASSERT_TRUE(git("init"));
        git("config user.email t@t.com");
        git("config user.name t");

        // Three commits touching churn.go so it becomes a hotspot.
        for (int rev = 0; rev < 3; ++rev) {
            std::ofstream f(repo_ / "churn.go");
            f << "package main\n\nfunc Churn() int { return " << rev << "; }\n";
            f.close();
            ASSERT_TRUE(git("add ."));
            ASSERT_TRUE(git("commit -m rev" + std::to_string(rev)));
        }

        // A staged long function (>100 lines) so git_analyze (default scope=
        // staged) surfaces a metrics finding deterministically.
        {
            std::ofstream f(repo_ / "huge.go");
            f << "package main\n\nfunc Huge() int {\n\tx := 0\n";
            for (int i = 0; i < 130; ++i) f << "\tx += " << i << "\n";
            f << "\treturn x\n}\n";
        }
        ASSERT_TRUE(git("add huge.go"));

        Config config;
        config.project.root = repo_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(repo_.string());
        engine_ = std::make_unique<CodebaseIntelligenceEngine>();
    }

    void TearDown() override {
        engine_.reset();
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(repo_, ec);
    }

    bool git(const std::string& args) {
        std::string cmd =
            "git -C \"" + repo_.string() + "\" " + args + " >/dev/null 2>&1";
        return std::system(cmd.c_str()) == 0;
    }

    std::filesystem::path repo_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<CodebaseIntelligenceEngine> engine_;
};

TEST_F(CodeInsightGitTest, GitAnalyzeSurfacesRealChanges) {
    nlohmann::json params;
    params["mode"] = "git_analyze";
    params["scope"] = "staged";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("mode=git_analyze"), std::string::npos);
    EXPECT_NE(result.text.find("== GIT CHANGES =="), std::string::npos);
    // The staged long function must surface as a metrics issue.
    EXPECT_NE(result.text.find("metrics_issues:"), std::string::npos);
    EXPECT_NE(result.text.find("Huge"), std::string::npos);
}

TEST_F(CodeInsightGitTest, GitHotspotsSurfacesRealChurn) {
    nlohmann::json params;
    params["mode"] = "git_hotspots";
    params["time_window"] = "1y";  // wide window so the 3 commits are in range
    auto result = handle_code_insight(params, *engine_, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("mode=git_hotspots"), std::string::npos);
    EXPECT_NE(result.text.find("== GIT HOTSPOTS =="), std::string::npos);
    EXPECT_NE(result.text.find("window=1y"), std::string::npos);
}

// =============================================================================
// Registration test
// =============================================================================

TEST(RegisterAnalysisHandlers, RegistersWithoutCrash) {
    Config config;
    config.project.root = "/tmp";
    McpServer server(config);
    server.register_tools();
    size_t before = server.tool_count();

    register_analysis_handlers(server, nullptr, nullptr, nullptr, nullptr,
                               nullptr);
    // Should have added 3 tools (replacing stubs)
    EXPECT_EQ(server.tool_count(), before + 3);
}

TEST(RegisterAnalysisHandlers, NullAnnotatorReturnsError) {
    Config config;
    config.project.root = "/tmp";
    McpServer server(config);
    register_analysis_handlers(server, nullptr, nullptr, nullptr, nullptr,
                               nullptr);

    // The last 3 tools should be our handlers
    size_t count = server.tool_count();
    ASSERT_GE(count, 3u);
}

}  // namespace
}  // namespace mcp
}  // namespace lci
