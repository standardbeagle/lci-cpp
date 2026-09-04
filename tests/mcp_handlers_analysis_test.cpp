#include <gtest/gtest.h>

#include <lci/analysis/codebase_intelligence.h>
#include <lci/analysis/side_effect_analyzer.h>
#include <lci/config.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/semantic_annotator.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/handlers_side_effects.h>
#include <lci/mcp/server.h>

#include <nlohmann/json.hpp>

#include "portable_env.h"
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
        sym1.line = 4;
        sym1.end_line = 20;
        symbols_.push_back(sym1);

        Symbol sym2;
        sym2.name = "processData";
        sym2.type = SymbolType::Function;
        sym2.file_id = FileID{1};
        sym2.line = 26;
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
        // The error-handling report is beta and ships dark; these fixtures
        // exercise analysis handlers, so flip the gate on.
        config.insight.error_report = "on";
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
// stay loud — a bare empty result reads as "pure" — via an explicit
// available=false + reason (not isError, which reads as a code failure), and
// the reason must name the file root-relative ("main.go", not the temp dir).
TEST_F(CodeInsightTest, SideEffectsSymbolFoundButNoRecordSaysWhy) {
    SideEffectAnalyzer empty_analyzer("go");
    nlohmann::json params;
    params["mode"] = "symbol";
    params["symbol_name"] = "main";
    auto result = handle_side_effects(params, empty_analyzer, indexer_.get());
    EXPECT_FALSE(result.is_error);
    auto na = nlohmann::json::parse(result.text);
    EXPECT_EQ(na.value("available", true), false);
    EXPECT_NE(na.value("reason", std::string()).find("no side-effect record"),
              std::string::npos);
    auto reason = na.value("reason", std::string());
    EXPECT_NE(reason.find("main"), std::string::npos);
    EXPECT_EQ(reason.find(temp_dir_.string()), std::string::npos)
        << "reason leaked absolute path: " << reason;
}

// side_effects symbol mode with a name the index does not hold: a lookup
// miss is a definitive negative answer, not a tool error.
TEST_F(CodeInsightTest, SideEffectsSymbolNotFoundIsNotAnError) {
    SideEffectAnalyzer analyzer("go");
    nlohmann::json params;
    params["mode"] = "symbol";
    params["symbol_name"] = "NoSuchSymbol99";
    auto result = handle_side_effects(params, analyzer, indexer_.get());
    EXPECT_FALSE(result.is_error);
    auto na = nlohmann::json::parse(result.text);
    EXPECT_EQ(na.value("available", true), false);
    EXPECT_NE(na.value("reason", std::string()).find("symbol not found"),
              std::string::npos);
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

// fallow-class circular dependency detection: mutual package IMPORTS couple
// whole packages even when no call edge closes a loop — the call-graph CYCLES
// section cannot see them.
TEST(CodeInsightImportCycles, ReportsMutualPackageImports) {
    auto dir = lci::test::unique_temp_dir("lci_import_cycles_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "alpha");
    std::filesystem::create_directories(dir / "beta");
    {
        std::ofstream f(dir / "go.mod");
        f << "module example.com/app\n";
    }
    {
        std::ofstream f(dir / "alpha" / "a.go");
        f << "package alpha\n\nimport \"example.com/app/beta\"\n\n"
             "func A() { beta.B() }\n";
    }
    {
        std::ofstream f(dir / "beta" / "b.go");
        f << "package beta\n\nimport \"example.com/app/alpha\"\n\n"
             "func B() {}\nfunc C() { alpha.A() }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    params["mode"] = "unified";
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("import_cycles=1"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("alpha <-> beta"), std::string::npos)
        << result.text;

    std::filesystem::remove_all(dir);
}

// Dead-code is an @lci:-annotation-curated flow (2026-08-31): static
// reachability surfaces candidates; annotations resolve the ambiguous ones
// (dynamic dispatch, function values, public API), and flow=true drives an
// agent through what needs marking.
TEST(CodeInsightDeadCode, UnusedTypesSurfaced) {
    auto dir = lci::test::unique_temp_dir("lci_deadcode_types_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "lib.hpp");
        f << "#pragma once\n"
             "struct UsedThing { int x; };\n"
             "struct GhostThing { int y; };\n"
             "UsedThing make();\n";
    }
    {
        std::ofstream f(dir / "lib.cpp");
        f << "#include \"lib.hpp\"\n"
             "UsedThing make() { UsedThing t; return t; }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "deadcode";
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("== UNUSED TYPES =="), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("GhostThing"), std::string::npos)
        << result.text;
    EXPECT_EQ(result.text.find("struct UsedThing "), std::string::npos)
        << result.text;

    std::filesystem::remove_all(dir);
}

// @lci:exclude[deadcode] and used-labels suppress a candidate; the flow view
// lists what still needs a decision.
TEST(CodeInsightDeadCode, AnnotationSuppressesAndFlowLists) {
    auto dir = lci::test::unique_temp_dir("lci_deadcode_annot_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "main.go");
        f << "package main\n\n"
             "func main() { register() }\n"
             "func register() {}\n"
             "// @lci:exclude[deadcode]\n"
             "func handler() {}\n"   // annotated used -> suppressed
             "func rawHandler() {}\n";  // truly unreferenced -> candidate
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;
    auto annotator = std::make_unique<SemanticAnnotator>();
    annotator->populate_from_index(indexer);

    // Plain view with the annotator: handler suppressed, rawHandler listed.
    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "deadcode";
    auto plain = handle_code_insight(params, engine, indexer, nullptr,
                                     nullptr, annotator.get());
    ASSERT_FALSE(plain.is_error) << plain.text;
    EXPECT_NE(plain.text.find("== UNUSED PRIVATE =="), std::string::npos)
        << plain.text;
    EXPECT_NE(plain.text.find("rawHandler"), std::string::npos) << plain.text;
    EXPECT_EQ(plain.text.find("handler ("), std::string::npos)
        << "annotated handler must be suppressed: " << plain.text;

    // Flow view: worklist naming the pending candidate and the markers.
    params["flow"] = true;
    auto flow = handle_code_insight(params, engine, indexer, nullptr, nullptr,
                                    annotator.get());
    ASSERT_FALSE(flow.is_error) << flow.text;
    EXPECT_NE(flow.text.find("== ANNOTATION FLOW =="), std::string::npos)
        << flow.text;
    EXPECT_NE(flow.text.find("rawHandler"), std::string::npos) << flow.text;
    EXPECT_NE(flow.text.find("@lci:exclude[deadcode]"), std::string::npos)
        << flow.text;
    EXPECT_NE(flow.text.find("@lci:labels[dead]"), std::string::npos)
        << flow.text;
    // The annotated one is not pending.
    EXPECT_EQ(flow.text.find("UNUSED PRIVATE handler ("), std::string::npos)
        << flow.text;

    std::filesystem::remove_all(dir);
}

// @lci:labels[dead] confirms a symbol that would otherwise look referenced:
// a confirmed-dead symbol is always listed.
TEST(CodeInsightDeadCode, LabelDeadConfirms) {
    auto dir = lci::test::unique_temp_dir("lci_deadcode_confirm_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "lib.hpp");
        f << "#pragma once\n"
             "// @lci:labels[dead]\n"
             "struct Doomed { int x; };\n"
             "Doomed useDoomed();\n";  // Doomed IS referenced, but marked dead
    }
    {
        std::ofstream f(dir / "lib.cpp");
        f << "#include \"lib.hpp\"\n"
             "Doomed useDoomed() { Doomed d; return d; }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;
    auto annotator = std::make_unique<SemanticAnnotator>();
    annotator->populate_from_index(indexer);

    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "deadcode";
    auto result = handle_code_insight(params, engine, indexer, nullptr,
                                      nullptr, annotator.get());
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("Doomed"), std::string::npos)
        << "label[dead] must force-list even a referenced symbol: "
        << result.text;

    std::filesystem::remove_all(dir);
}

// The annotation path driver (analysis=annotate) walks an agent through
// every @lci: annotation LCI supports: entry points, community domains,
// hot-path/loop memory hints, and dead code. Each dimension lists the
// elements that would benefit and the exact marker to add.
TEST(CodeInsightAnnotate, DrivesAllDimensions) {
    auto dir = lci::test::unique_temp_dir("lci_annotate_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        // A small call graph: Serve (exported entry-ish) -> handle -> work;
        // a recursive walk; a dead helper.
        std::ofstream f(dir / "srv.go");
        f << "package srv\n\n"
             "func Serve() { handle() }\n"
             "func handle() { work(); walk(3) }\n"
             "func work() {}\n"
             "func walk(n int) { if n > 0 { walk(n-1) } }\n"
             "func deadHelper() {}\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;
    auto annotator = std::make_unique<SemanticAnnotator>();
    annotator->populate_from_index(indexer);

    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "annotate";
    auto result = handle_code_insight(params, engine, indexer, nullptr,
                                      nullptr, annotator.get());
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("== ANNOTATION PATH =="), std::string::npos)
        << result.text;
    // Every dimension's marker vocabulary is documented.
    EXPECT_NE(result.text.find("@lci:labels[entry]"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("@lci:call-frequency[hot-path]"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("@lci:loop-bounded"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("@lci:exclude[deadcode]"), std::string::npos)
        << result.text;
    // walk is recursive -> a loop-bound candidate.
    EXPECT_NE(result.text.find("walk"), std::string::npos) << result.text;

    std::filesystem::remove_all(dir);
}

// A target filter narrows the driver to one dimension.
TEST(CodeInsightAnnotate, TargetFilterSelectsDimension) {
    auto dir = lci::test::unique_temp_dir("lci_annotate_target_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "srv.go");
        f << "package srv\n\n"
             "func Serve() { handle() }\n"
             "func handle() { work() }\n"
             "func work() {}\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;
    auto annotator = std::make_unique<SemanticAnnotator>();
    annotator->populate_from_index(indexer);

    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "annotate";
    params["target"] = "entry";
    auto result = handle_code_insight(params, engine, indexer, nullptr,
                                      nullptr, annotator.get());
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("-- ENTRY"), std::string::npos) << result.text;
    // Other dimensions' section headers are absent under a target filter.
    EXPECT_EQ(result.text.find("-- DEADCODE"), std::string::npos)
        << result.text;

    std::filesystem::remove_all(dir);
}

// An @lci annotation removes an element from its dimension's worklist.
TEST(CodeInsightAnnotate, AnnotatedElementLeavesWorklist) {
    auto dir = lci::test::unique_temp_dir("lci_annotate_supp_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "srv.go");
        f << "package srv\n\n"
             "// @lci:labels[entry]\n"
             "func Serve() { handle() }\n"
             "func handle() {}\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;
    auto annotator = std::make_unique<SemanticAnnotator>();
    annotator->populate_from_index(indexer);

    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "annotate";
    params["target"] = "entry";
    auto result = handle_code_insight(params, engine, indexer, nullptr,
                                      nullptr, annotator.get());
    ASSERT_FALSE(result.is_error) << result.text;
    // Serve is already labeled entry -> not pending under the entry target.
    EXPECT_EQ(result.text.find("Serve ("), std::string::npos) << result.text;

    std::filesystem::remove_all(dir);
}

// Dead-code candidates whose name is dynamically dispatched are MARKED (not
// suppressed — the by-name count is a lower bound): the flow hands the agent
// the exact tag (@lci:labels[dynamic]) to resolve the ambiguity.
TEST(CodeInsightDeadCode, MarksDynamicallyDispatchedCandidates) {
    auto dir = lci::test::unique_temp_dir("lci_dc_dyn_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "srv.go");
        f << "package srv\n\n"
             "type impl struct{}\n"
             "func (i *impl) serve() {}\n"   // unexported, 0 static callers
             "func run(xs []interface{ serve() }) {\n"
             "    for _, x := range xs { x.serve() }\n"  // dynamic dispatch
             "}\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "deadcode";
    params["flow"] = true;
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    // serve is a candidate whose name is dynamically dispatched -> the flow
    // carries the dynamic evidence and the resolving tag.
    if (result.text.find("serve") != std::string::npos) {
        EXPECT_NE(result.text.find("dynamically dispatched"),
                  std::string::npos)
            << result.text;
        EXPECT_NE(result.text.find("@lci:labels[dynamic]"),
                  std::string::npos)
            << result.text;
    }

    std::filesystem::remove_all(dir);
}

// The DYNAMIC section maps where the code opts OUT of static analysis:
// dynamic-dispatch call sites (through unknown receivers), the hubs that make
// them, and symbols reachable only dynamically. Using dynamic features
// explicitly limits what static analysis can see — that map is the value.
TEST(CodeInsightDynamic, ReportsDynamicDispatch) {
    auto dir = lci::test::unique_temp_dir("lci_dynamic_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        // handler dispatches through an interface value (dynamic); target is
        // reachable only through that dynamic call.
        std::ofstream f(dir / "srv.go");
        f << "package srv\n\n"
             "type Handler interface { Serve() }\n"
             "func run(h Handler) { h.Serve() }\n"
             "type Impl struct{}\n"
             "func (i *Impl) Serve() {}\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    params["mode"] = "unified";
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("== DYNAMIC =="), std::string::npos)
        << result.text;
    // h.Serve() is a dynamic-dispatch call site.
    EXPECT_NE(result.text.find("dynamic_call_sites="), std::string::npos)
        << result.text;

    std::filesystem::remove_all(dir);
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

// D1 enforcement: attribute-tagged files (test/example/vendored/...) are
// excluded from EVERY code_insight analysis section — entry points, health,
// load-bearing, modules, vocabulary — and the exclusion is labeled in
// == SUMMARY ==, never silent. structure mode counts the same classifier's
// buckets. Fixture mirrors the verified defects: chi's _examples main.go
// entry points, guzzle's *Test.php production classification.
class CodeInsightAttrTest : public CodeInsightTest {
  protected:
    void SetUp() override {
        CodeInsightTest::SetUp();
        write_file(temp_dir_ / "_examples" / "demo" / "main.go",
                   "package main\n\nfunc main() {}\n\n"
                   "func ExampleWidget() {}\n");
        write_file(temp_dir_ / "tests" / "HandlerTest.php",
                   "<?php\nclass HandlerTest {\n"
                   "  public function testSend() {}\n}\n");
        indexer_->index_directory(temp_dir_.string());
    }
};

TEST_F(CodeInsightAttrTest, UnifiedExcludesTaggedFilesAndLabelsIt) {
    nlohmann::json params;
    params["mode"] = "unified";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    ASSERT_FALSE(result.is_error);
    // No example/test symbols in any section.
    EXPECT_EQ(result.text.find("ExampleWidget"), std::string::npos)
        << result.text;
    EXPECT_EQ(result.text.find("HandlerTest"), std::string::npos);
    EXPECT_EQ(result.text.find("testSend"), std::string::npos);
    // Production entry point still present.
    EXPECT_NE(result.text.find("main.go"), std::string::npos);
    // The exclusion is labeled with its attribute AND the directory the files
    // came from — a bare count says something was left out but not what.
    EXPECT_NE(result.text.find("excluded_from_analysis:"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("test=1 (tests/)"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("example=1 (_examples/)"), std::string::npos)
        << result.text;
}

// The attributes parameter: the default covers shipping code, and a caller can
// ask for the excluded trees explicitly instead of being told a number they
// cannot re-aim. The header always says which set the numbers cover.
TEST_F(CodeInsightAttrTest, DefaultIsShippingAndIsStatedInTheHeader) {
    nlohmann::json params;
    params["mode"] = "unified";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    ASSERT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("attributes=shipping"), std::string::npos)
        << result.text;
}

TEST_F(CodeInsightAttrTest, AttributesAllAnalyzesTheExcludedTreesToo) {
    nlohmann::json params;
    params["mode"] = "unified";
    params["attributes"] = "all";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    ASSERT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("attributes=all"), std::string::npos) << result.text;
    // Nothing is excluded any more, so the exclusion block is gone.
    EXPECT_EQ(result.text.find("excluded_from_analysis:"), std::string::npos)
        << result.text;
}

TEST_F(CodeInsightAttrTest, AttributesTakesAListOfNames) {
    nlohmann::json params;
    params["mode"] = "unified";
    params["attributes"] = nlohmann::json::array({"test", "example"});
    auto result = handle_code_insight(params, *engine_, *indexer_);
    ASSERT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("attributes=test,example"), std::string::npos)
        << result.text;
    // Production is now the excluded set.
    EXPECT_NE(result.text.find("production="), std::string::npos)
        << result.text;
}

// An attribute name this project does not have is an error, not a silent
// empty analysis: analyzing a different set than the caller asked for is the
// worse failure.
TEST_F(CodeInsightAttrTest, UnknownAttributeNameIsAnError) {
    nlohmann::json params;
    params["mode"] = "unified";
    params["attributes"] = "benchmarks";  // the attribute is "benchmark"
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.text.find("benchmarks"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("benchmark"), std::string::npos) << result.text;
}

TEST_F(CodeInsightAttrTest, StructureCountsViaClassifier) {
    nlohmann::json params;
    params["mode"] = "structure";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    ASSERT_FALSE(result.is_error);
    // tests= counts the classifier's Test files (the PHP *Test.php that the
    // old extension-only categorizer scored as tests=0 on guzzle);
    // example= surfaces the tagged _examples file.
    EXPECT_NE(result.text.find("tests=1"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("example=1"), std::string::npos);
}

// The base fixture's temp_dir_ is not a git repo. A well-formed request whose
// environmental precondition is absent is NOT a tool error (isError would read
// as a code failure to agent callers): both git modes return a successful,
// self-describing not-applicable block — explicit available=false + reason,
// never a fake zero-STATISTICS block.
TEST_F(CodeInsightTest, GitAnalyzeReportsUnavailableOnNonGitDir) {
    nlohmann::json params;
    params["mode"] = "git_analyze";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("available=false"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("not a git repository"), std::string::npos);
    // No fabricated analysis data alongside the not-applicable marker.
    EXPECT_EQ(result.text.find("files_changed"), std::string::npos);
}

TEST_F(CodeInsightTest, GitHotspotsReportsUnavailableOnNonGitDir) {
    nlohmann::json params;
    params["mode"] = "git_hotspots";
    auto result = handle_code_insight(params, *engine_, *indexer_);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("available=false"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("not a git repository"), std::string::npos);
    EXPECT_EQ(result.text.find("files_analyzed"), std::string::npos);
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

// Direct recursion is a property of one function, not an architectural
// cycle: it goes to the compact recursion= line, never a "x -> x" row.
TEST(CodeInsightGraphSignals, RecursionIsReportedApartFromCycles) {
    auto dir = lci::test::unique_temp_dir("lci_recursion_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "g.go");
        f << "package main\n\n"
             "func fact(n int) int { if n <= 1 { return 1 }; "
             "return fact(n-1) }\n"
             "func a1() int { return a2() }\n"
             "func a2() int { return a1() }\n"
             "func main() { _ = fact(3) + a1() }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    auto cpos = result.text.find("== CYCLES ==");
    ASSERT_NE(cpos, std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("fact -> fact"), std::string::npos)
        << result.text;
    auto rpos = result.text.find("recursion=", cpos);
    ASSERT_NE(rpos, std::string::npos) << result.text;
    EXPECT_NE(result.text.find("fact", rpos), std::string::npos);
    // The genuine multi-node cycle still shows.
    EXPECT_NE(result.text.find("a1 -> a2 -> a1", cpos), std::string::npos);

    std::filesystem::remove_all(dir);
}

// The `size -> size` false-cycle class: a method calling a same-named method
// through a member of an unindexed type must produce neither a cycle nor a
// recursion entry (discrimination test for the foreign_receiver gate).
TEST(CodeInsightGraphSignals, SameNamedMethodThroughMemberIsNotACycle) {
    auto dir = lci::test::unique_temp_dir("lci_selfloop_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "t.go");
        // tracker.files is an external set type; files.size() resolves by
        // bare name and used to link back to Tracker.size itself.
        f << "package main\n\n"
             "type Tracker struct { files ExternalSet }\n"
             "func (t Tracker) size() int { return t.files.size() }\n"
             "func main() { var t Tracker; _ = t.size() }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_EQ(result.text.find("size -> size"), std::string::npos)
        << result.text;
    // Not recursion either — the call goes through a foreign member.
    auto rpos = result.text.find("recursion=");
    if (rpos != std::string::npos) {
        EXPECT_EQ(result.text.find("size", rpos), std::string::npos)
            << result.text;
    }

    std::filesystem::remove_all(dir);
}

// -- Entry-point pins ---------------------------------------------------------
// ENTRY POINTS is authoritative only when grounded: author annotations or a
// framework signature seat the real front door first; without either the
// section labels itself heuristic and asks for annotations.

namespace {
struct InsightCorpus {
    std::filesystem::path dir;
    explicit InsightCorpus(const char* tag) {
        dir = lci::test::unique_temp_dir(tag);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }
    ~InsightCorpus() { std::filesystem::remove_all(dir); }
    void write(const char* name, const std::string& content) {
        std::ofstream f(dir / name);
        f << content;
    }
};
}  // namespace

TEST(CodeInsightEntryPins, HeuristicListSaysSoAndAsksForAnnotations) {
    InsightCorpus c("lci_entry_heuristic_");
    c.write("g.go",
            "package main\n\n"
            "func Frobnicate() int { return 1 }\n"
            "func main() { _ = Frobnicate() }\n");

    Config config;
    config.project.root = c.dir.string();
    MasterIndex indexer(config);
    ASSERT_TRUE(indexer.index_directory(c.dir.string()));
    CodebaseIntelligenceEngine engine;

    auto result = handle_code_insight({}, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("confidence=heuristic"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("@lci:entry"), std::string::npos);
}

TEST(CodeInsightEntryPins, ConfigPinsSeatFirstWithAnnotatedConfidence) {
    InsightCorpus c("lci_entry_config_");
    c.write("g.go",
            "package main\n\n"
            "func Alpha() int { return 1 }\n"
            "func Beta() int { return Alpha() }\n"
            "func Zulu() int { return Beta() }\n"
            "func main() { _ = Zulu() }\n");

    Config config;
    config.project.root = c.dir.string();
    config.insight.entry_points = {"Zulu"};
    MasterIndex indexer(config);
    ASSERT_TRUE(indexer.index_directory(c.dir.string()));
    CodebaseIntelligenceEngine engine;

    auto result = handle_code_insight({}, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("confidence=annotated"), std::string::npos)
        << result.text;
    EXPECT_EQ(result.text.find("hint=ranked exports"), std::string::npos);
    // The pinned symbol leads the api list.
    auto ep = result.text.find("== ENTRY POINTS ==");
    ASSERT_NE(ep, std::string::npos);
    auto first_api = result.text.find("api: ", ep);
    ASSERT_NE(first_api, std::string::npos);
    EXPECT_EQ(result.text.substr(first_api + 5, 4), "Zulu") << result.text;
}

TEST(CodeInsightEntryPins, FrameworkSignatureMatchesGoModuleIdentity) {
    InsightCorpus c("lci_entry_fw_");
    c.write("go.mod", "module github.com/go-chi/chi/v5\n\ngo 1.21\n");
    c.write("g.go",
            "package chi\n\n"
            "func NewRouter() int { return 1 }\n"
            "func Aardvark() int { return NewRouter() }\n");

    Config config;
    config.project.root = c.dir.string();
    MasterIndex indexer(config);
    ASSERT_TRUE(indexer.index_directory(c.dir.string()));
    CodebaseIntelligenceEngine engine;

    auto result = handle_code_insight({}, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("confidence=framework"), std::string::npos)
        << result.text;
    auto ep = result.text.find("== ENTRY POINTS ==");
    ASSERT_NE(ep, std::string::npos);
    auto first_api = result.text.find("api: ", ep);
    ASSERT_NE(first_api, std::string::npos);
    EXPECT_EQ(result.text.substr(first_api + 5, 9), "NewRouter") << result.text;
}

// Pinned trivially-named symbols keep their seats (guzzle's get/post verbs
// ARE its front door) while unpinned trivial names still demote.
TEST(CodeInsightEntryPins, PinnedTrivialNamesAreNotDemoted) {
    InsightCorpus c("lci_entry_trivial_");
    c.write("g.go",
            "package main\n\n"
            "func Get() int { return 1 }\n"
            "func Weird() int { return Get() }\n"
            "func main() { _ = Weird() }\n");

    Config config;
    config.project.root = c.dir.string();
    config.insight.entry_points = {"Get"};
    MasterIndex indexer(config);
    ASSERT_TRUE(indexer.index_directory(c.dir.string()));
    CodebaseIntelligenceEngine engine;

    auto result = handle_code_insight({}, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    auto ep = result.text.find("== ENTRY POINTS ==");
    ASSERT_NE(ep, std::string::npos);
    auto first_api = result.text.find("api: ", ep);
    ASSERT_NE(first_api, std::string::npos);
    EXPECT_EQ(result.text.substr(first_api + 5, 3), "Get") << result.text;
}

// Layer violation: a Data-layer function calling a Presentation-layer function
// is an upward call against the architecture and must be flagged.
TEST(CodeInsightLayers, FlagsUpwardCall) {
    auto dir = lci::test::unique_temp_dir("lci_layers_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "g.go");
        // saveRecord -> Data Layer (save*). render* -> Presentation.
        // The flow-evidence gate (2026-08-31) reports an upward edge only
        // where downward flow dominates the layer pair (>=4 edges, 3:1), so
        // the fixture establishes real Presentation->Data layering first and
        // then inverts one edge.
        f << "package main\n\n"
             "func saveA() int { return 1 }\n"
             "func saveB() int { return 1 }\n"
             "func saveC() int { return 1 }\n"
             "func saveD() int { return 1 }\n"
             "func renderView() int { return saveA() + saveB() + saveC() + saveD() }\n"
             "func saveRecord() int { return renderView() }\n"
             "func main() { _ = saveRecord() + renderView() }\n";
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

// The complement: with NO established downward flow between two labels, a
// single upward edge is more likely a mislabeled helper than architecture —
// the audits measured 3/6 reported violations as fabricated. Withhold it.
TEST(CodeInsightLayers, WithholdsUpwardCallWithoutFlowEvidence) {
    auto dir = lci::test::unique_temp_dir("lci_layers_noev_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "g.go");
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
    EXPECT_EQ(result.text.find("saveRecord [Data Layer]"), std::string::npos)
        << "upward edge without dominance evidence must be withheld";

    std::filesystem::remove_all(dir);
}

// Brokers must never print a degenerate all-0.00 leaderboard: scores are
// emitted relative to the top broker, so the first row is always 1.00.
TEST(CodeInsightGraphSignals, BrokerScoresAreRelativeNotDegenerate) {
    auto dir = lci::test::unique_temp_dir("lci_broker_scale_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "g.go");
        // Two mutually-recursive groups bridged by a2: a2 is the top broker.
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
    auto bpos = result.text.find("brokers:");
    ASSERT_NE(bpos, std::string::npos) << result.text;
    // Top row is the reference point: relative score 1.00.
    EXPECT_NE(result.text.find("betweenness=1.00", bpos), std::string::npos)
        << result.text;
    // A leaderboard where every row rounds to zero is a false signal.
    auto line_end = result.text.find('\n', bpos + 9);
    auto first_row_end = result.text.find('\n', line_end + 1);
    std::string first_row =
        result.text.substr(line_end + 1, first_row_end - line_end - 1);
    EXPECT_EQ(first_row.find("betweenness=0.00"), std::string::npos)
        << first_row;

    std::filesystem::remove_all(dir);
}

// Cycles must be actionable: member chain with an explicit loop-back plus the
// file the cycle lives in, not a bare name soup.
TEST(CodeInsightGraphSignals, CyclesEmitMemberChainAndFile) {
    auto dir = lci::test::unique_temp_dir("lci_cycle_emit_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "ring.go");
        f << "package main\n\n"
             "func a1() int { return a2() }\n"
             "func a2() int { return a1() }\n"
             "func main() { _ = a1() }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    auto cpos = result.text.find("== CYCLES ==");
    ASSERT_NE(cpos, std::string::npos) << result.text;
    EXPECT_NE(result.text.find("a1 -> a2 -> a1 (ring.go)", cpos),
              std::string::npos)
        << result.text;

    std::filesystem::remove_all(dir);
}

// A middleware calling the next handler in the chain is the definitional shape
// of the pattern, not an architecture violation — whitelist it.
TEST(CodeInsightLayers, MiddlewareChainCallsAreNotViolations) {
    auto dir = lci::test::unique_temp_dir("lci_layers_mw_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "mw.go");
        // saveMiddleware -> Data Layer (save*); renderView -> Presentation.
        // Upward by layer depth, but the caller is a middleware: exempt.
        // saveRecord -> renderNext: callee is a next-handler dispatch: exempt.
        f << "package main\n\n"
             "func renderView() int { return 1 }\n"
             "func saveMiddleware() int { return renderView() }\n"
             "func renderNext() int { return 2 }\n"
             "func saveRecord() int { return renderNext() }\n"
             "func main() { _ = saveMiddleware() + saveRecord() }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_EQ(result.text.find("== LAYER VIOLATIONS =="), std::string::npos)
        << result.text;

    std::filesystem::remove_all(dir);
}

// Library-shaped corpora: the exported public surface (factories first, root
// package first) leads ENTRY POINTS; main() binaries move to a `binaries:`
// sub-line and stop eating api slots.
TEST(CodeInsightEntryPoints, LibraryApiLeadsAndBinariesAreSubLine) {
    auto dir = lci::test::unique_temp_dir("lci_entrypoints_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "cmd" / "server");
    {
        std::ofstream f(dir / "router.go");
        f << "package chi\n\n"
             "func NewRouter() int { return 0 }\n"
             "func Handle() int { return NewRouter() }\n";
    }
    {
        std::ofstream f(dir / "cmd" / "server" / "main.go");
        f << "package main\n\n"
             "func main() { }\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    CodebaseIntelligenceEngine engine;

    nlohmann::json params;
    auto result = handle_code_insight(params, engine, indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    auto epos = result.text.find("== ENTRY POINTS ==");
    ASSERT_NE(epos, std::string::npos) << result.text;
    auto api_pos = result.text.find("api: NewRouter", epos);
    auto bin_pos = result.text.find("binaries:", epos);
    ASSERT_NE(api_pos, std::string::npos) << result.text;
    ASSERT_NE(bin_pos, std::string::npos) << result.text;
    // The public surface leads; binaries are a trailing sub-line.
    EXPECT_LT(api_pos, bin_pos) << result.text;
    // main() no longer occupies an api slot.
    EXPECT_EQ(result.text.find("main: main", epos), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("cmd/server/main.go", epos), std::string::npos)
        << result.text;

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
    auto snapshot = indexer.ref_tracker().pin();
    for (const char* name : {"a1", "a2", "b1", "b2"}) {
        for (const auto& es : snapshot->find_symbols_by_name(name))
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
// Error-handling / resource sections (== ERROR HANDLING == etc.)
// =============================================================================

// Real corpus + hand-driven analyzer records keyed to the indexed symbols'
// file:line (the extractor-side detection is covered by
// side_effect_extraction_test.cpp; these tests lock rollup + emission).
class ErrorHandlingSectionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_eh_section_test_");
        std::filesystem::create_directories(temp_dir_);

        write_file(temp_dir_ / "main.go",
                   "package main\n"
                   "\n"
                   "func swallowIt() {\n"
                   "}\n"
                   "\n"
                   "func leakIt() {\n"
                   "}\n"
                   "\n"
                   "func funnelIt() {\n"
                   "}\n"
                   "\n"
                   "func PublicEntry() {\n"
                   "\tswallowIt()\n"
                   "\tfunnelIt()\n"
                   "}\n");
        // Same finding in a test path: must never score (production-only).
        write_file(temp_dir_ / "util_test.go",
                   "package main\n"
                   "\n"
                   "func helperSwallow() {\n"
                   "}\n");

        Config config;
        config.project.root = temp_dir_.string();
        // The error-handling report is beta and ships dark; these fixtures
        // exercise analysis handlers, so flip the gate on.
        config.insight.error_report = "on";
        indexer_ = std::make_unique<MasterIndex>(config);
        ASSERT_TRUE(indexer_->index_directory(temp_dir_.string()));
        engine_ = std::make_unique<CodebaseIntelligenceEngine>();

        analyzer_ = std::make_unique<SideEffectAnalyzer>("go");
        // generic_string(), not string(): the extractor feeds the analyzer
        // the index's generic spelling, so a native path here would key the
        // findings under a name the report can never join back.
        std::string main_path = (temp_dir_ / "main.go").generic_string();
        std::string test_path = (temp_dir_ / "util_test.go").generic_string();

        analyzer_->begin_function("swallowIt", main_path, 3, 4);
        CatchSiteInfo site;
        site.line = 3;
        site.body_empty = true;
        analyzer_->record_catch(site);
        analyzer_->end_function();

        analyzer_->begin_function("leakIt", main_path, 6, 7);
        analyzer_->record_call_site_resources("Open", 6, false);
        analyzer_->end_function();

        // A cause-loss funnel: rethrows, but the new error never chains the
        // cause. Seeds the api-reaches-cause-loss exposure via PublicEntry.
        analyzer_->begin_function("funnelIt", main_path, 9, 10);
        CatchSiteInfo funnel;
        funnel.line = 9;
        funnel.has_rethrow = true;
        funnel.has_other_call = true;
        analyzer_->record_catch(funnel);
        analyzer_->end_function();

        // The caller must be a scored unit for exposure to name it.
        analyzer_->begin_function("PublicEntry", main_path, 12, 15);
        analyzer_->end_function();

        analyzer_->begin_function("helperSwallow", test_path, 3, 4);
        analyzer_->record_catch(site);
        analyzer_->end_function();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    static void write_file(const std::filesystem::path& path,
                           const std::string& content) {
        std::ofstream out(path);
        out << content;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<CodebaseIntelligenceEngine> engine_;
    std::unique_ptr<SideEffectAnalyzer> analyzer_;
};

TEST_F(ErrorHandlingSectionTest, OverviewEmitsBothSectionsAfterHealth) {
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    auto health = result.text.find("== HEALTH ==");
    auto eh = result.text.find("== ERROR HANDLING ==");
    auto res = result.text.find("== RESOURCE MANAGEMENT ==");
    ASSERT_NE(health, std::string::npos);
    ASSERT_NE(eh, std::string::npos);
    ASSERT_NE(res, std::string::npos);
    EXPECT_LT(health, eh);
    EXPECT_LT(eh, res);
    // LOAD BEARING (when present) comes after both.
    auto lb = result.text.find("== LOAD BEARING ==");
    if (lb != std::string::npos) EXPECT_LT(res, lb);
}

TEST_F(ErrorHandlingSectionTest, ZeroSignalRendersNAInsteadOfPerfectScore) {
    // The pgvector defect: the C classifier saw no ereport/elog error flow,
    // so every counter was zero — and the rollup awarded a perfect 10.00
    // over nothing. Zero observed signal must render as n/a, not as the
    // best possible score.
    SideEffectAnalyzer plain("go");
    // generic_string() for the same reason as the fixture's paths: the
    // report joins findings back through the index's generic spelling.
    std::string main_path = (temp_dir_ / "main.go").generic_string();
    plain.begin_function("PublicEntry", main_path, 12, 15);
    plain.end_function();

    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_, &plain);
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find(
                  "score=n/a signal=none (no error-flow constructs"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find(
                  "score=n/a signal=none (no resource acquisitions"),
              std::string::npos)
        << result.text;
    // Neither section may carry a numeric score when there is no signal
    // (HEALTH's own score=10.00/10 line is a different metric and fine).
    auto eh = result.text.find("== ERROR HANDLING ==");
    ASSERT_NE(eh, std::string::npos);
    EXPECT_EQ(result.text.find("score=10.00", eh), std::string::npos)
        << result.text;
}

TEST_F(ErrorHandlingSectionTest, SummaryLineCarriesBothScores) {
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("error_handling="), std::string::npos);
    EXPECT_NE(result.text.find(" resources="), std::string::npos);
}

TEST_F(ErrorHandlingSectionTest, FindingsCarryFileLineAndSignal) {
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("empty-catch: swallowIt (main.go:3)"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("leak-no-release: leakIt (main.go:6)"),
              std::string::npos)
        << result.text;
}

TEST_F(ErrorHandlingSectionTest, TestPathFindingsNeverScore) {
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error);
    EXPECT_EQ(result.text.find("helperSwallow"), std::string::npos)
        << result.text;
}

TEST_F(ErrorHandlingSectionTest, SectionsSkippedWhenAnalyzerEmpty) {
    SideEffectAnalyzer empty("go");
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_, &empty);
    ASSERT_FALSE(result.is_error);
    EXPECT_EQ(result.text.find("== ERROR HANDLING =="), std::string::npos);
    EXPECT_EQ(result.text.find("== RESOURCE MANAGEMENT =="),
              std::string::npos);
}

TEST_F(ErrorHandlingSectionTest, DetailedErrorsListsAllFindings) {
    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "errors";
    auto result = handle_code_insight(params, *engine_, *indexer_,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("== ERROR HANDLING =="), std::string::npos);
    EXPECT_NE(result.text.find("empty-catch"), std::string::npos);
}

TEST_F(ErrorHandlingSectionTest, DetailedResourcesListsAllFindings) {
    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "resources";
    auto result = handle_code_insight(params, *engine_, *indexer_,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("== RESOURCE MANAGEMENT =="),
              std::string::npos);
    EXPECT_NE(result.text.find("leak-no-release"), std::string::npos);
}

// Purity must count only the analysis-set files — it previously tallied
// EVERY indexed file (tests included), so its total disagreed with every
// other section's population by up to 8x (fastapi: 4589 vs symbols=562).
TEST_F(ErrorHandlingSectionTest, PurityCountsOnlyAnalysisScope) {
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    // 4 production records (swallowIt/leakIt/funnelIt/PublicEntry); the
    // util_test.go record (helperSwallow) must not count.
    EXPECT_NE(result.text.find("total=4 "), std::string::npos) << result.text;
}

TEST_F(ErrorHandlingSectionTest, DetailedErrorsSaysWhyWithoutRecords) {
    // Unpopulated analyzer: stay loud (an empty section would read as "no
    // findings") via available=false + reason, without the error flag.
    SideEffectAnalyzer empty("go");
    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "errors";
    auto result = handle_code_insight(params, *engine_, *indexer_, &empty);
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("available=false"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("unpopulated"), std::string::npos);
    EXPECT_EQ(result.text.find("== ERROR HANDLING =="), std::string::npos);
}

TEST_F(ErrorHandlingSectionTest, SideEffectsSummaryCarriesJsonTwin) {
    nlohmann::json params;
    params["mode"] = "summary";
    auto result = handle_side_effects(params, *analyzer_, indexer_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("error_handling")) << result.text;
    ASSERT_TRUE(json.contains("resources"));
    EXPECT_EQ(json["error_handling"]["swallow_sites"].get<int>(), 1);
    EXPECT_GE(json["error_handling"]["findings"].size(), 1u);
    EXPECT_EQ(json["resources"]["acquisitions"].get<int>(), 1);
    // Location always carries the file name (D6 lesson).
    auto loc = json["error_handling"]["findings"][0]["location"]
                   .get<std::string>();
    EXPECT_NE(loc.find("main.go:"), std::string::npos);
}

TEST_F(ErrorHandlingSectionTest, ExposureCarriesLogAnnotationAndFunnels) {
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    // PublicEntry -> swallowIt: an empty catch logs nothing.
    EXPECT_NE(result.text.find("api-reaches-swallow: PublicEntry"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("log=none"), std::string::npos) << result.text;
    // PublicEntry -> funnelIt: the error surfaces renamed and chainless.
    EXPECT_NE(result.text.find("api-reaches-cause-loss: PublicEntry"),
              std::string::npos)
        << result.text;
}

TEST_F(ErrorHandlingSectionTest, DensityLineDeSaturatesTheScore) {
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, *indexer_,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error);
    EXPECT_NE(result.text.find("density: findings="), std::string::npos)
        << result.text;
    // 10.00 is reserved for zero findings; this corpus has findings.
    auto eh = result.text.find("== ERROR HANDLING ==");
    auto res = result.text.find("== RESOURCE MANAGEMENT ==");
    ASSERT_NE(eh, std::string::npos);
    ASSERT_NE(res, std::string::npos);
    std::string eh_section = result.text.substr(eh, res - eh);
    EXPECT_EQ(eh_section.find("score=10.00"), std::string::npos)
        << eh_section;
}

// -- The beta gate ------------------------------------------------------------

TEST_F(ErrorHandlingSectionTest, BetaGateOffHidesEverySurface) {
    Config off_cfg;
    off_cfg.project.root = temp_dir_.string();  // error_report stays "off"
    MasterIndex off_index(off_cfg);
    ASSERT_TRUE(off_index.index_directory(temp_dir_.string()));

    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, off_index,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_EQ(result.text.find("== ERROR HANDLING =="), std::string::npos);
    EXPECT_EQ(result.text.find("== RESOURCE MANAGEMENT =="),
              std::string::npos);
    EXPECT_EQ(result.text.find("error_handling="), std::string::npos);

    // The JSON twin goes dark too.
    nlohmann::json sp;
    sp["mode"] = "summary";
    auto summary = handle_side_effects(sp, *analyzer_, &off_index);
    ASSERT_FALSE(summary.is_error);
    auto json = nlohmann::json::parse(summary.text);
    EXPECT_FALSE(json.contains("error_handling")) << summary.text;
}

TEST_F(ErrorHandlingSectionTest, BetaGateOffRefusesDetailedErrors) {
    Config off_cfg;
    off_cfg.project.root = temp_dir_.string();
    MasterIndex off_index(off_cfg);
    ASSERT_TRUE(off_index.index_directory(temp_dir_.string()));

    nlohmann::json params;
    params["mode"] = "detailed";
    params["analysis"] = "errors";
    auto result = handle_code_insight(params, *engine_, off_index,
                                      analyzer_.get());
    // Config-gated-off is a definitive not-available answer, not an error:
    // a successful LCF block that still names the gate and how to open it.
    ASSERT_FALSE(result.is_error) << result.text;
    EXPECT_NE(result.text.find("available=false"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("BETA"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("error_report"), std::string::npos);
    // No report data alongside the not-applicable marker.
    EXPECT_EQ(result.text.find("== ERROR HANDLING =="), std::string::npos);
}

TEST_F(ErrorHandlingSectionTest, CaptureModeWritesTheFullReportToStateDir) {
    auto state_dir = temp_dir_ / "state";
    ::setenv("XDG_STATE_HOME", state_dir.string().c_str(), 1);

    Config cap_cfg;
    cap_cfg.project.root = temp_dir_.string();
    cap_cfg.insight.error_report = "capture";
    MasterIndex cap_index(cap_cfg);
    ASSERT_TRUE(cap_index.index_directory(temp_dir_.string()));

    // Capture publishes nothing in-band...
    nlohmann::json params;
    auto result = handle_code_insight(params, *engine_, cap_index,
                                      analyzer_.get());
    ASSERT_FALSE(result.is_error);
    EXPECT_EQ(result.text.find("== ERROR HANDLING =="), std::string::npos);

    // ...and writes the full untruncated report to the state dir.
    std::string path =
        write_error_report_capture(cap_index, analyzer_.get());
    ::unsetenv("XDG_STATE_HOME");
    ASSERT_FALSE(path.empty());
    std::ifstream f(path);
    ASSERT_TRUE(f.good()) << path;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string report = ss.str();
    EXPECT_NE(report.find("beta capture"), std::string::npos);
    EXPECT_NE(report.find("== ERROR HANDLING =="), std::string::npos);
    EXPECT_NE(report.find("== RESOURCE MANAGEMENT =="), std::string::npos);
    EXPECT_NE(report.find("empty-catch: swallowIt"), std::string::npos);

    // "on" and "off" never write.
    EXPECT_TRUE(write_error_report_capture(*indexer_, analyzer_.get())
                    .empty());
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
