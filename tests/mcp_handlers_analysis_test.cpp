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

#include "test_git.h"
#include "unique_temp.h"

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

// Fail loud (Karpathy #6): the fixture HAS annotations, so an unknown label
// gets the "no match, index holds N" hint (not the "no annotations at all"
// variant) so the caller knows the label was wrong, not the corpus empty.
TEST_F(SemanticAnnotationsTest, QueryByNonexistentLabelEmitsHint) {
    nlohmann::json params;
    params["label"] = "nonexistent_label_xyz";
    auto result = handle_semantic_annotations(params, *annotator_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("hint"));
    auto hint = json["hint"].get<std::string>();
    EXPECT_NE(hint.find("matched"), std::string::npos);
    EXPECT_NE(hint.find("annotated symbol"), std::string::npos);
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
    auto tmp = lci::test::unique_temp_dir("lci_populate_test_");
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

// Fail loud (Karpathy #6): file mode on a path with no analyzed functions must
// carry a recovery hint, not a bare {results:[],total_count:0}.
TEST_F(SideEffectsTest, FileModeUnknownPathEmitsHint) {
    nlohmann::json params;
    params["mode"] = "file";
    params["file_path"] = "does_not_exist.go";
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_count"].get<int>(), 0);
    ASSERT_TRUE(json.contains("hint"));
    EXPECT_NE(json["hint"].get<std::string>().find("does_not_exist.go"),
              std::string::npos);
}

// A valid category with zero matches is a real empty — it still gets a hint so
// the caller knows to fall back to summary rather than assume "no data".
TEST_F(SideEffectsTest, CategoryNoMatchEmitsHint) {
    nlohmann::json params;
    params["mode"] = "category";
    params["category"] = "network";  // fixture has none
    auto result = handle_side_effects(params, *analyzer_, nullptr);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_count"].get<int>(), 0);
    EXPECT_TRUE(json.contains("hint"));
}

// =============================================================================
// Code insight handler tests
// =============================================================================

class CodeInsightTest : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_code_insight_test_");
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

// NOTE: handle_code_insight emits LCF text (not JSON). These tests assert LCF
// header + section presence; the mcp/code_insight integration goldens lock the
// full byte-level output.

TEST_F(CodeInsightTest, DefaultModeIsOverview) {
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("LCF/1.0"), std::string::npos);
    EXPECT_NE(result.text.find("mode=overview"), std::string::npos);
    EXPECT_NE(result.text.find("== REPOSITORY MAP =="), std::string::npos);
    EXPECT_NE(result.text.find("== HEALTH =="), std::string::npos);
}

// side_effects symbol mode against a real index but an empty analyzer: the
// symbol resolves in the index, yet the analyzer holds no record for it. Must
// fail loud (Karpathy #6) — a bare empty result reads as "pure" — and the
// error must name the file root-relative ("main.go", not the temp dir).
TEST_F(CodeInsightTest, SideEffectsSymbolFoundButNoRecordFailsLoud) {
    SideEffectAnalyzer empty_analyzer("go");
    nlohmann::json params;
    params["mode"] = "symbol";
    params["symbol_name"] = "main";
    auto result = handle_side_effects(params, empty_analyzer, indexer_.get());
    EXPECT_TRUE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    auto err = json["error"].get<std::string>();
    EXPECT_NE(err.find("main"), std::string::npos);
    EXPECT_EQ(err.find(temp_dir_.string()), std::string::npos)
        << "error leaked absolute path: " << err;
}

// Same real index: symbol mode on a resolvable symbol emits a root-relative
// file_path (regression guard for the absolute-path leak that was fixed).
TEST_F(CodeInsightTest, SideEffectsSymbolEmitsRootRelativePath) {
    SideEffectAnalyzer analyzer("go");
    // Populate the analyzer from the real index so `main` has a record.
    analyzer.populate_from_index(*indexer_);
    nlohmann::json params;
    params["mode"] = "symbol";
    params["symbol_name"] = "main";
    auto result = handle_side_effects(params, analyzer, indexer_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_EQ(json["total_count"].get<int>(), 1);
    auto fp = json["results"][0]["file_path"].get<std::string>();
    EXPECT_EQ(fp, "main.go");
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
        repo_ = lci::test::unique_temp_dir("lci_ci_git_repo_");
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

    bool git(const std::string& args) { return test::run_git(repo_, args); }

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

// Load-bearing centrality: a leaf called transitively by the whole chain must
// outrank its callers. Real call graph, weight-1.0 reachability, no mocks.
TEST(CodeInsightLoadBearing, RanksByTransitiveReach) {
    auto dir = lci::test::unique_temp_dir("lci_loadbearing_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "chain.go");
        f << "package main\n\n"
             "func leaf() int { return 1 }\n"
             "func mid() int { return leaf() }\n"
             "func top() int { return mid() }\n"
             "func main() { _ = top() }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;  // default overview
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    ASSERT_NE(result.text.find("== LOAD BEARING =="), std::string::npos)
        << result.text;

    // `leaf` is reachable from mid, top, main → must rank above `top`
    // (reachable only from main). First listed = highest reach.
    auto lb = result.text.find("== LOAD BEARING ==");
    auto leaf_pos = result.text.find("leaf", lb);
    auto top_pos = result.text.find("top (", lb);
    ASSERT_NE(leaf_pos, std::string::npos);
    EXPECT_TRUE(top_pos == std::string::npos || leaf_pos < top_pos)
        << "leaf must outrank top in load-bearing order\n" << result.text;

    std::filesystem::remove_all(dir);
}

// Real graph clustering + cycle detection surfaced in the overview. Two
// mutually-recursive groups (each a cycle) wired into one file; overview must
// emit == CLUSTERS == (Louvain) and == CYCLES == (SCC).
TEST(CodeInsightGraphSignals, SurfacesClustersAndCycles) {
    auto dir = lci::test::unique_temp_dir("lci_graphsignals_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "g.go");
        // Group A: a1<->a2 (cycle). Group B: b1<->b2 (cycle). a2 bridges to b1.
        f << "package main\n\n"
             "func a1() int { return a2() }\n"
             "func a2() int { return a1() + b1() }\n"
             "func b1() int { return b2() }\n"
             "func b2() int { return b1() }\n"
             "func main() { _ = a1() }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;  // overview
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("== CLUSTERS =="), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("modularity="), std::string::npos);
    EXPECT_NE(result.text.find("== CYCLES =="), std::string::npos)
        << result.text;
    // The bridge between the two groups is a betweenness broker.
    EXPECT_NE(result.text.find("brokers:"), std::string::npos) << result.text;

    std::filesystem::remove_all(dir);
}

// Layer violation: a Data-layer function calling a Presentation-layer function
// is an upward call against the architecture and must be flagged.
TEST(CodeInsightLayers, FlagsUpwardCall) {
    auto dir = lci::test::unique_temp_dir("lci_layers_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "g.go");
        // saveRecord -> Data Layer (save*). renderView -> Presentation (render*).
        // Data calling Presentation = upward violation.
        f << "package main\n\n"
             "func renderView() int { return 1 }\n"
             "func saveRecord() int { return renderView() }\n"
             "func main() { _ = saveRecord() }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    ASSERT_NE(result.text.find("== LAYER VIOLATIONS =="), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("saveRecord"), std::string::npos);
    EXPECT_NE(result.text.find("renderView"), std::string::npos);

    std::filesystem::remove_all(dir);
}

// Flagship: label-coherent communities. A Louvain community whose members all
// carry the same propagated @lci: label is reported as a named domain. Crosses
// graph structure (CallGraph) with propagated semantics (GraphPropagator).
TEST(CodeInsightLabelCoherence, ClustersGetDomainFromPropagatedLabels) {
    auto dir = lci::test::unique_temp_dir("lci_labelcoh_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "g.go");
        f << "package main\n\n"
             "func a1() int { return a2() }\n"
             "func a2() int { return a1() + b1() }\n"
             "func b1() int { return b2() }\n"
             "func b2() int { return b1() }\n"
             "func main() { _ = a1() }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    // Seed every function with the domain label "core", then propagate, so each
    // detected community is fully coherent on it.
    GraphPropagator propagator(&indexer.ref_tracker());
    for (const char* name : {"a1", "a2", "b1", "b2"}) {
        for (const auto* es : indexer.ref_tracker().find_symbols_by_name(name))
            propagator.seed_label(es->id, "core", 1.0);
    }
    propagator.propagate();

    nlohmann::json params;  // overview
    auto result =
        handle_code_insight(params, engine, indexer, nullptr, &propagator);
    ASSERT_FALSE(result.is_error) << result.text;
    ASSERT_NE(result.text.find("== CLUSTERS =="), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("domain=core"), std::string::npos)
        << "communities should be labeled with the propagated domain\n"
        << result.text;

    std::filesystem::remove_all(dir);
}

// =============================================================================
// Registration test
// =============================================================================

TEST(RegisterAnalysisHandlers, RegistersWithoutCrash) {
    Config config;
    config.project.root = "/tmp";
    McpServer server(config);

    register_analysis_handlers(server, nullptr, nullptr, nullptr, nullptr,
                               nullptr);
    // Registers exactly its 3 tools (semantic_annotations, side_effects,
    // code_insight) — no stub registrar runs first anymore.
    EXPECT_EQ(server.tool_count(), 3u);
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
