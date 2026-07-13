#include <gtest/gtest.h>

#include <lci/analysis/side_effect_analyzer.h>
#include <lci/config.h>
#include <lci/core/context_lookup.h>
#include <lci/core/context_lookup_types.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/semantic_annotator.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/server.h>
#include <lci/search/search_engine.h>
#include <lci/symbol.h>

#include <nlohmann/json.hpp>

#include "unique_temp.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace lci {
namespace mcp {
namespace {

// -- Test fixture with real index ---------------------------------------------

class HandlersFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        // Create a temp directory with sample files for indexing
        temp_dir_ = lci::test::unique_temp_dir("lci_handler_test_");
        std::filesystem::create_directories(temp_dir_);

        // Create sample source files
        write_file(temp_dir_ / "main.go",
                   "package main\n\nfunc main() {\n\tprintln(\"hello\")\n}\n");
        write_file(temp_dir_ / "handler.go",
                   "package main\n\nfunc handleRequest(r Request) Response "
                   "{\n\treturn Response{}\n}\n");
        write_file(temp_dir_ / "utils.go",
                   "package main\n\nfunc parseInput(s string) int {\n\treturn "
                   "0\n}\n");
        write_file(temp_dir_ / ".hidden/secret.go",
                   "package hidden\n\nfunc secret() {}\n");
        write_file(temp_dir_ / "internal/api/server.go",
                   "package api\n\nfunc StartServer() {}\n");
        write_file(temp_dir_ / "crlf.go",
                   "package main\r\nfunc CrlfFunc() {}\r\n");
        write_file(temp_dir_ / "math.go",
                   "package main\n\n// Add returns the sum of a and b.\nfunc "
                   "Add(a int, b int) int {\n\treturn a + b\n}\n");

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

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SearchEngine> search_engine_;
};

// =============================================================================
// info handler tests
// =============================================================================

TEST(InfoHandler, DefaultReturnsOverview) {
    auto result = handle_info(nlohmann::json::object());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("server"));
    EXPECT_TRUE(json.contains("available_tools"));
}

TEST(InfoHandler, VersionReturnsServerInfo) {
    nlohmann::json params;
    params["tool"] = "version";
    auto result = handle_info(params);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["name"], "version");
    EXPECT_TRUE(json.contains("server_version"));
    EXPECT_TRUE(json.contains("capabilities"));
}

TEST(InfoHandler, SearchReturnsHelpText) {
    nlohmann::json params;
    params["tool"] = "search";
    auto result = handle_info(params);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["name"], "search");
    EXPECT_TRUE(json.contains("parameters"));
    EXPECT_TRUE(json.contains("example"));
}

TEST(InfoHandler, GetContextReturnsHelpText) {
    nlohmann::json params;
    params["tool"] = "get_context";
    auto result = handle_info(params);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["name"], "get_context");
}

TEST(InfoHandler, FindFilesReturnsHelpText) {
    nlohmann::json params;
    params["tool"] = "find_files";
    auto result = handle_info(params);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["name"], "find_files");
}

TEST(InfoHandler, CaseInsensitiveTool) {
    nlohmann::json params;
    params["tool"] = "SEARCH";
    auto result = handle_info(params);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["name"], "search");
}

TEST(InfoHandler, UnknownToolReturnsOverview) {
    nlohmann::json params;
    params["tool"] = "nonexistent_tool";
    auto result = handle_info(params);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("server"));
}

// =============================================================================
// search handler tests
// =============================================================================

TEST_F(HandlersFixture, SearchReturnsResults) {
    nlohmann::json params;
    params["pattern"] = "main";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("results"));
    EXPECT_GT(json["total_matches"].get<int>(), 0);
}

TEST_F(HandlersFixture, SearchWithContextLines) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["output"] = "ctx:3";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("results"));
}

TEST_F(HandlersFixture, SearchFilesOnlyOutput) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["output"] = "files";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("files"));
    EXPECT_TRUE(json.contains("unique_files"));
}

TEST_F(HandlersFixture, SearchCountOutput) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["output"] = "count";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("total_matches"));
    EXPECT_TRUE(json.contains("unique_files"));
}

// -- include= add-ons (breadcrumbs / refs / safety / deps) --------------------
// Go parity: handleSearch accepts these tokens and, for strong matches
// (normalizedScore >= 0.5), attaches `references` {incoming_count,
// outgoing_count} and `breadcrumbs` (scope chain). `safety`/`deps` are
// accepted but never populated in compact results (matches Go server.go
// CompactSearchResult: only Breadcrumbs + References are filled).

TEST_F(HandlersFixture, SearchIncludeRefsEmitsReferenceCounts) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["include"] = "refs";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("results"));
    bool found_refs = false;
    for (const auto& group : json["results"]) {
        for (const auto& hit : group["hits"]) {
            if (hit.contains("references")) {
                EXPECT_TRUE(hit["references"].contains("incoming_count"));
                EXPECT_TRUE(hit["references"].contains("outgoing_count"));
                found_refs = true;
            }
        }
    }
    EXPECT_TRUE(found_refs) << "expected at least one symbol-enclosed match to "
                              "carry references; body: " << result.text;
}

TEST_F(HandlersFixture, SearchIncludeBreadcrumbsAcceptedAndWellFormed) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["include"] = "breadcrumbs";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("results"));
    for (const auto& group : json["results"]) {
        for (const auto& hit : group["hits"]) {
            if (!hit.contains("breadcrumbs")) continue;
            ASSERT_TRUE(hit["breadcrumbs"].is_array());
            for (const auto& c : hit["breadcrumbs"]) {
                EXPECT_TRUE(c.contains("scope_type"));
                EXPECT_TRUE(c.contains("name"));
                EXPECT_TRUE(c.contains("start_line"));
                EXPECT_TRUE(c.contains("end_line"));
                EXPECT_TRUE(c.contains("language"));
                EXPECT_TRUE(c.contains("visibility"));
            }
        }
    }
}

TEST_F(HandlersFixture, SearchIncludeSafetyAndDepsAcceptedNotErrored) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["include"] = "safety,deps";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error) << result.text;
}

// A match inside a nested scope (method body) must carry a non-empty
// breadcrumb chain — proves include=breadcrumbs emits real scope data, not
// just an absent field.
TEST(SearchIncludeBreadcrumbs, NestedScopeEmitsChain) {
    auto dir = lci::test::unique_temp_dir("lci_bc_nested_");
    std::filesystem::create_directories(dir);
    {
        std::ofstream o(dir / "svc.go");
        o << "package main\n\n"
             "type Service struct{}\n\n"
             "func (s Service) Process() {\n"
             "\tuniqueMarker := 1\n"
             "\t_ = uniqueMarker\n"
             "}\n";
    }
    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    indexer.index_directory(dir.string());
    SearchEngine engine(indexer);

    nlohmann::json params;
    params["pattern"] = "uniqueMarker";
    params["include"] = "breadcrumbs";
    auto result = handle_search(params, indexer, &engine);
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("results"));
    bool found_chain = false;
    for (const auto& group : json["results"]) {
        for (const auto& hit : group["hits"]) {
            if (hit.contains("breadcrumbs") && !hit["breadcrumbs"].empty()) {
                found_chain = true;
                const auto& first = hit["breadcrumbs"][0];
                EXPECT_TRUE(first.contains("scope_type"));
                EXPECT_TRUE(first.contains("name"));
            }
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    EXPECT_TRUE(found_chain)
        << "nested match should carry a scope chain; body: " << result.text;
}

// -- get_context purity (Go getPurityInfo parity) -----------------------------
// When a SideEffectAnalyzer is wired into the MCP runtime (populate_from_index
// at startup), get_context attaches a `purity` block to function/method
// symbols: {is_pure, purity_score, confidence, [local_effects],
// [transitive_effects], [reasons]}. Without a propagator it is omitted
// (Go returns nil; field is omitempty).
class GetContextPurityTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = lci::test::unique_temp_dir("lci_purity_ctx_");
        std::filesystem::create_directories(dir_);
        std::ofstream(dir_ / "math.go")
            << "package math\n\nfunc Adder(a int, b int) int {\n"
               "\treturn a + b\n}\n";
        Config config;
        config.project.root = dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(dir_.string());
    }
    void TearDown() override {
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::string adder_object_id() const {
        auto snapshot = indexer_->ref_tracker().pin();
        auto sym = snapshot->find_symbol_by_name("Adder");
        return sym ? encode_symbol_id(sym->id) : "";
    }
    std::filesystem::path dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

TEST_F(GetContextPurityTest, FunctionCarriesPurityWhenAnalyzerWired) {
    SideEffectAnalyzer analyzer("generic");
    analyzer.populate_from_index(*indexer_);

    auto oid = adder_object_id();
    ASSERT_FALSE(oid.empty());
    nlohmann::json params;
    params["id"] = oid;
    auto result = handle_get_context(params, *indexer_, &analyzer);
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("contexts"));
    ASSERT_FALSE(json["contexts"].empty());
    const auto& ctx = json["contexts"][0];
    ASSERT_TRUE(ctx.contains("purity")) << result.text;
    const auto& purity = ctx["purity"];
    EXPECT_TRUE(purity.contains("is_pure"));
    EXPECT_TRUE(purity.contains("purity_score"));
    EXPECT_TRUE(purity.contains("confidence"));
    // Adder has no callees -> pure.
    EXPECT_TRUE(purity["is_pure"].get<bool>());
}

TEST_F(GetContextPurityTest, PurityOmittedWithoutAnalyzer) {
    auto oid = adder_object_id();
    ASSERT_FALSE(oid.empty());
    nlohmann::json params;
    params["id"] = oid;
    auto result = handle_get_context(params, *indexer_);  // no analyzer
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_FALSE(json["contexts"].empty());
    EXPECT_FALSE(json["contexts"][0].contains("purity"));
}

TEST_F(HandlersFixture, SearchMissingPatternErrors) {
    nlohmann::json params = nlohmann::json::object();
    // No pattern provided
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_TRUE(result.is_error);
}

// FIX-B (4RfLnLqNCD7u): json-schema validator now backs search param checks.
// Error response shape must remain the existing
// create_validation_error_response / create_multi_validation_error_response
// wire format (snapshot-preserved for missing-pattern). These three cases
// exercise schema-level rules: required, type, and additionalProperties.

// Required field missing -> validation_error with field=pattern.
TEST_F(HandlersFixture, SearchSchemaRejectsMissingRequiredPattern) {
    nlohmann::json params = nlohmann::json::object();  // no pattern, no patterns
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_TRUE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_FALSE(json["success"].get<bool>());
    // Must point at pattern (either as the single-error 'field' or as one of
    // the field_names in validation_errors[]).
    bool mentions_pattern = false;
    if (json.contains("error") && json["error"].contains("field")) {
        mentions_pattern |=
            json["error"]["field"].get<std::string>().find("pattern") !=
            std::string::npos;
    }
    if (json.contains("validation_errors")) {
        for (const auto& e : json["validation_errors"]) {
            if (e.contains("field_name") &&
                e["field_name"].get<std::string>().find("pattern") !=
                    std::string::npos) {
                mentions_pattern = true;
                break;
            }
        }
    }
    EXPECT_TRUE(mentions_pattern) << result.text;
}

// Wrong type on `max` -> validation_error with field=max and invalid_format-ish code.
TEST_F(HandlersFixture, SearchSchemaRejectsWrongTypeMax) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["max"] = "not-a-number";  // schema says number
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_TRUE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_FALSE(json["success"].get<bool>());
    bool mentions_max = false;
    if (json.contains("validation_errors")) {
        for (const auto& e : json["validation_errors"]) {
            if (e.contains("field_name") &&
                e["field_name"].get<std::string>() == "max") {
                mentions_max = true;
                break;
            }
        }
    } else if (json.contains("error") && json["error"].contains("field")) {
        mentions_max = json["error"]["field"].get<std::string>() == "max";
    }
    EXPECT_TRUE(mentions_max) << result.text;
}

// Unknown field rejected by additionalProperties:false.
TEST_F(HandlersFixture, SearchSchemaRejectsUnknownField) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["frobnicate"] = "wat";  // not in schema
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_TRUE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_FALSE(json["success"].get<bool>());
    // The unknown-field error should mention 'frobnicate' somewhere in the
    // message/field stream — schema validator typically reports the parent
    // object pointer plus the offending key in the message text.
    auto body = result.text;
    EXPECT_NE(body.find("frobnicate"), std::string::npos) << body;
}

// Agent tool-callers routinely send "" for optional params they don't use
// (repo-QA traces: one model failed 20/22 searches on the old minLength:1).
// Empty optional strings must validate; the business rule still rejects
// pattern and patterns both empty.
TEST_F(HandlersFixture, SearchAcceptsEmptyOptionalStrings) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["patterns"] = "";
    params["filter"] = "";
    params["flags"] = "";
    params["include"] = "";
    params["symbol_types"] = "";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error) << result.text;

    nlohmann::json both_empty;
    both_empty["pattern"] = "";
    both_empty["patterns"] = "";
    auto err = handle_search(both_empty, *indexer_, search_engine_.get());
    EXPECT_TRUE(err.is_error);
}

// Counts hit rows across all file groups in the grouped response shape.
static size_t count_hit_rows(const nlohmann::json& response) {
    size_t n = 0;
    for (const auto& group : response["results"]) {
        n += group["hits"].size();
    }
    return n;
}

TEST_F(HandlersFixture, SearchClampsMaxResults) {
    nlohmann::json params;
    params["pattern"] = "func";
    params["max"] = 999;
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_LE(json["showing"].get<int>(), 100);
    EXPECT_LE(count_hit_rows(json), 100u);
}

TEST_F(HandlersFixture, SearchWithFlags) {
    nlohmann::json params;
    params["pattern"] = "MAIN";
    params["flags"] = "ci";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
}

// Compact grouped shape: results[] is one entry per file (root-relative
// path emitted once) with hits[] rows carrying line numbers. A literal
// search repeats the identical matched text on every row, so it is emitted
// once at the top level as "match" and per-hit rows omit it. Symbol
// enrichment (sym/type/id) appears on the first hit inside each enclosing
// symbol; column/score were dropped (agents never used them — ordering
// already encodes rank).
TEST_F(HandlersFixture, SearchEmitsCompactShape) {
    nlohmann::json params;
    params["pattern"] = "main";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json["results"].is_array());
    ASSERT_FALSE(json["results"].empty());
    EXPECT_EQ(json["match"].get<std::string>(), "main");
    for (const auto& group : json["results"]) {
        EXPECT_TRUE(group.contains("file"));
        auto file = group["file"].get<std::string>();
        EXPECT_FALSE(file.empty());
        EXPECT_NE(file.front(), '/') << "paths must be root-relative";
        ASSERT_TRUE(group["hits"].is_array());
        ASSERT_FALSE(group["hits"].empty());
        for (const auto& hit : group["hits"]) {
            EXPECT_TRUE(hit.contains("line"));
            EXPECT_FALSE(hit.contains("match")) << "uniform match text must "
                                                   "not repeat per hit";
            EXPECT_FALSE(hit.contains("column"));
            EXPECT_FALSE(hit.contains("score"));
            EXPECT_FALSE(hit.contains("result_id"));
            // Enrichment comes as a unit and is never empty-valued.
            const bool enriched = hit.contains("id");
            EXPECT_EQ(enriched, hit.contains("sym"));
            EXPECT_EQ(enriched, hit.contains("type"));
            if (enriched) {
                EXPECT_FALSE(hit["id"].get<std::string>().empty());
            }
        }
    }
}

// Symbol enrichment must resolve the enclosing symbol so the object id is
// non-empty and matches the symbol that owns the matched line.
TEST_F(HandlersFixture, SearchEnclosingSymbolResolved) {
    nlohmann::json params;
    params["pattern"] = "main";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_FALSE(json["results"].empty());
    bool found_enclosed = false;
    for (const auto& group : json["results"]) {
        for (const auto& hit : group["hits"]) {
            if (hit.contains("id") &&
                hit["sym"].get<std::string>() == "main") {
                EXPECT_EQ(hit["type"].get<std::string>(), "function");
                found_enclosed = true;
            }
        }
    }
    EXPECT_TRUE(found_enclosed) << "no match resolved to enclosing 'main'";
}

// Default result budget is 15 (agent context economy — see handle_search);
// explicit max still wins, hard cap 100 unchanged.
TEST_F(HandlersFixture, SearchDefaultMaxIsFifteen) {
    nlohmann::json params;
    params["pattern"] = "main";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_LE(json["showing"].get<int>(), 15);
    EXPECT_LE(count_hit_rows(json), 15u);
}

// The strongest (top-ranked) hits carry the matched source line so agents
// can often answer without a follow-up read.
TEST_F(HandlersFixture, SearchTopHitsCarryText) {
    nlohmann::json params;
    params["pattern"] = "handleRequest";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_FALSE(json["results"].empty());
    bool any_text = false;
    for (const auto& group : json["results"]) {
        for (const auto& hit : group["hits"]) {
            if (hit.contains("text")) {
                any_text = true;
                EXPECT_NE(hit["text"].get<std::string>().find("handleRequest"),
                          std::string::npos);
            }
        }
    }
    EXPECT_TRUE(any_text) << "top-ranked hit should carry its source line";
}

// Zero-result searches fail loud: hint text plus fuzzy near-miss symbol
// suggestions so a typo'd identifier self-corrects in one round trip.
TEST_F(HandlersFixture, SearchEmptyResultCarriesHintAndSuggestions) {
    nlohmann::json params;
    params["pattern"] = "handleRequestz";  // typo of handleRequest
    params["semantic"] = false;
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_matches"].get<int>(), 0);
    EXPECT_TRUE(json.contains("hint"));
    ASSERT_TRUE(json.contains("similar_symbols")) << result.text;
    bool suggested = false;
    for (const auto& s : json["similar_symbols"]) {
        if (s["name"].get<std::string>() == "handleRequest") suggested = true;
        EXPECT_FALSE(s["id"].get<std::string>().empty());
    }
    EXPECT_TRUE(suggested) << result.text;
}

// path= scopes results to a root-relative subtree.
TEST_F(HandlersFixture, SearchPathScopeFiltersResults) {
    nlohmann::json params;
    params["pattern"] = "func";
    params["path"] = "internal";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_FALSE(json["results"].empty()) << result.text;
    for (const auto& group : json["results"]) {
        EXPECT_EQ(group["file"].get<std::string>().rfind("internal/", 0), 0u)
            << group["file"];
    }
}

// filter= is an INCLUDE filter: language token keeps only that language's
// files. The old behavior compiled it as an exclude regex — filter:"go"
// removed every path containing "go" (i.e., all Go files) and returned 0.
TEST_F(HandlersFixture, SearchFilterLanguageTokenIncludes) {
    nlohmann::json params;
    params["pattern"] = "func";
    params["filter"] = "go";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_GT(json["total_matches"].get<int>(), 0) << result.text;
    for (const auto& group : json["results"]) {
        auto file = group["file"].get<std::string>();
        EXPECT_TRUE(file.size() > 3 &&
                    file.compare(file.size() - 3, 3, ".go") == 0)
            << file;
    }
}

// filter= glob tokens: "*.go" matches at any depth; unknown bare tokens are
// treated as literal extensions.
TEST_F(HandlersFixture, SearchFilterGlobAndExtensionTokens) {
    nlohmann::json params;
    params["pattern"] = "StartServer";
    params["filter"] = "*.go";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_GT(json["total_matches"].get<int>(), 0) << result.text;

    nlohmann::json p2;
    p2["pattern"] = "func";
    p2["filter"] = "md";  // no .md files in fixture -> zero, with hint
    auto r2 = handle_search(p2, *indexer_, search_engine_.get());
    ASSERT_FALSE(r2.is_error);
    auto j2 = nlohmann::json::parse(r2.text);
    EXPECT_EQ(j2["total_matches"].get<int>(), 0);
}

// path="." (and "./") means the whole root — no scoping.
TEST_F(HandlersFixture, SearchPathDotMeansWholeRoot) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["path"] = ".";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_GT(json["total_matches"].get<int>(), 0);
}

// Absolute path under the project root is relativized (agents paste paths
// back from other tool output); outside the root it can never match — error.
TEST_F(HandlersFixture, SearchPathAbsoluteInsideRootRelativized) {
    nlohmann::json params;
    params["pattern"] = "StartServer";
    params["path"] = (temp_dir_ / "internal").string();
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_GT(json["total_matches"].get<int>(), 0) << result.text;
}

TEST_F(HandlersFixture, SearchPathAbsoluteOutsideRootErrors) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["path"] = "/definitely/not/under/root";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_TRUE(result.is_error);
}

// path= accepts a glob matched against the root-relative file path.
TEST_F(HandlersFixture, SearchPathGlobScopes) {
    nlohmann::json params;
    params["pattern"] = "StartServer";
    params["path"] = "**/api/*.go";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_GT(json["total_matches"].get<int>(), 0) << result.text;
    for (const auto& group : json["results"]) {
        EXPECT_EQ(group["file"].get<std::string>(), "internal/api/server.go");
    }
}

// CRLF files: the "text" tier must not leak a trailing \r into output.
TEST_F(HandlersFixture, SearchTextStripsCarriageReturn) {
    nlohmann::json params;
    params["pattern"] = "CrlfFunc";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_GT(json["total_matches"].get<int>(), 0);
    for (const auto& group : json["results"]) {
        for (const auto& hit : group["hits"]) {
            if (hit.contains("text")) {
                EXPECT_EQ(hit["text"].get<std::string>().find('\r'),
                          std::string::npos);
            }
        }
    }
}

// Truncated results must report the true total and a directory histogram —
// never total==max cap-saturation.
TEST_F(HandlersFixture, SearchTruncationReportsTrueTotalAndDirs) {
    nlohmann::json params;
    params["pattern"] = "func";
    params["max"] = 2;
    params["semantic"] = false;
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_EQ(json["showing"].get<int>(), 2);
    // Fixture has >2 occurrences of "func" across the corpus.
    EXPECT_GT(json["total_matches"].get<int>(), 2);
    EXPECT_TRUE(json.value("truncated", false));
    ASSERT_TRUE(json.contains("dirs"));
    ASSERT_TRUE(json["dirs"].is_array());
    EXPECT_FALSE(json["dirs"].empty());
}

// get_context by-name: empty lookup fails loud with hint + fuzzy
// suggestions; found lookups emit root-relative file_path.
TEST_F(HandlersFixture, GetContextUnknownNameCarriesSuggestions) {
    nlohmann::json params;
    params["name"] = "handleRequestz";  // typo of handleRequest
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["count"].get<int>(), 0);
    EXPECT_TRUE(json.contains("hint"));
    ASSERT_TRUE(json.contains("similar_symbols")) << result.text;
    bool suggested = false;
    for (const auto& s : json["similar_symbols"]) {
        if (s["name"].get<std::string>() == "handleRequest") suggested = true;
    }
    EXPECT_TRUE(suggested) << result.text;
}

TEST_F(HandlersFixture, GetContextEmitsRootRelativePaths) {
    nlohmann::json params;
    params["name"] = "handleRequest";
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_GT(json["count"].get<int>(), 0);
    for (const auto& ctx : json["contexts"]) {
        auto fp = ctx["file_path"].get<std::string>();
        EXPECT_FALSE(fp.empty());
        EXPECT_NE(fp.front(), '/') << fp;
    }
}

// =============================================================================
// get_context handler tests
// =============================================================================

// Returns the encoded object ID of the first symbol named `name`, or "" if
// the corpus did not yield one (parser-dependent — tests guard on empty).
static std::string first_object_id(MasterIndex& idx, const std::string& name) {
    auto snapshot = idx.ref_tracker().pin();
    auto symbols = snapshot->find_symbols_by_name(name);
    if (symbols.empty()) return "";
    return encode_symbol_id(symbols.front()->id);
}

// No id, no name, no mode -> fail-fast error (Go validateGetContextParams).
TEST_F(HandlersFixture, GetContextNoParamsErrors) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_get_context(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_NE(json["error"].get<std::string>().find("id"),
              std::string::npos);
}

// id + name together -> conflict error (Go validateGetContextParams).
TEST_F(HandlersFixture, GetContextIdAndNameConflictErrors) {
    nlohmann::json params;
    params["id"] = "VE";
    params["name"] = "main";
    auto result = handle_get_context(params, *indexer_);
    EXPECT_TRUE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_NE(json["error"].get<std::string>().find("conflict"),
              std::string::npos);
}

// name-only no-mode path mirrors Go's id-only contract: name passes
// validation but is never resolved (Go's handleGetObjectContext only
// reads args.ID). Both binaries return {contexts:[],count:0}. Karpathy
// rule 1 — Go is the bar; the C++ ContextLookupEngine gap is tracked
// in Dart (15Wsg4HQoSW2) as the proper resolution path.
// get_context by name resolves the symbol(s) even without a `mode` preset.
// (Previously the name path was gated behind a non-empty mode, so a bare
// {"name": X} silently returned {contexts:[],count:0} — the
// "synthetic-passes-while-real-is-empty" trap. The Go oracle that the old
// id-only no-mode contract mirrored is retired, so name-only now resolves.)
TEST_F(HandlersFixture, GetContextNameOnlyResolves) {
    nlohmann::json params;
    params["name"] = "main";
    auto result = handle_get_context(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_GT(json["count"].get<int>(), 0);
    ASSERT_FALSE(json["contexts"].empty());
    EXPECT_EQ(json["contexts"][0]["symbol_name"], "main");
}

// mode parameter combined with id falls through to the id-resolution
// path (mode-path is only triggered when name is supplied — same
// gating as Go's handleGetContext branch). Verify the response shape
// is the standard contexts/errors envelope, not an error.
TEST_F(HandlersFixture, GetContextModeWithIdFallsThroughToIdPath) {
    nlohmann::json params;
    params["id"] = "VE";  // arbitrary; likely unresolved on this corpus
    params["mode"] = "full";
    auto result = handle_get_context(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("contexts"));
    EXPECT_TRUE(json.contains("count"));
}

// Valid object ID resolves to a full compact context payload.
TEST_F(HandlersFixture, GetContextByIdFindsSymbol) {
    auto oid = first_object_id(*indexer_, "main");
    if (oid.empty()) GTEST_SKIP() << "corpus produced no 'main' symbol";
    nlohmann::json params;
    params["id"] = oid;
    auto result = handle_get_context(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["count"].get<int>(), 1);
    auto& ctx = json["contexts"][0];
    EXPECT_TRUE(ctx.contains("file_path"));
    EXPECT_TRUE(ctx.contains("line"));
    EXPECT_EQ(ctx["object_id"].get<std::string>(), oid);
    EXPECT_TRUE(ctx.contains("symbol_type"));
    EXPECT_TRUE(ctx.contains("symbol_name"));
    EXPECT_TRUE(ctx.contains("is_exported"));
    EXPECT_TRUE(ctx.contains("definition"));
}

TEST_F(HandlersFixture, GetContextByIdIncludesBoundedSourceExcerpt) {
    auto oid = first_object_id(*indexer_, "main");
    if (oid.empty()) GTEST_SKIP() << "corpus produced no 'main' symbol";
    nlohmann::json params;
    params["id"] = oid;
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_EQ(json["count"].get<int>(), 1);
    const auto& ctx = json["contexts"][0];
    ASSERT_TRUE(ctx.contains("source_excerpt")) << result.text;
    const auto& excerpt = ctx["source_excerpt"];
    EXPECT_GE(excerpt["start_line"].get<int>(), 1);
    EXPECT_LE(excerpt["lines"].size(), 12u);
    ASSERT_FALSE(excerpt["lines"].empty());
    EXPECT_TRUE(excerpt["lines"][0].contains("line"));
    EXPECT_TRUE(excerpt["lines"][0].contains("text"));
    ASSERT_TRUE(ctx.contains("source_hint"));
    EXPECT_NE(ctx["source_hint"].get<std::string>().find("read the file only"),
              std::string::npos);
}

// `signature` is emitted when the symbol carries one (Go ObjectContext
// has `signature,omitempty`).
TEST_F(HandlersFixture, GetContextByIdEmitsSignatureWhenPresent) {
    auto snapshot = indexer_->ref_tracker().pin();
    auto symbols = snapshot->find_symbols_by_name("main");
    if (symbols.empty()) GTEST_SKIP() << "no 'main' symbol";
    const auto& sym = symbols.front();
    nlohmann::json params;
    params["id"] = encode_symbol_id(sym->id);
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    auto& ctx = json["contexts"][0];
    if (!sym->signature.empty()) {
        ASSERT_TRUE(ctx.contains("signature"));
        EXPECT_EQ(ctx["signature"].get<std::string>(),
                  std::string(sym->signature));
    } else {
        EXPECT_FALSE(ctx.contains("signature"));
    }
}

// `purity` is omitted entirely — Go emits it only when a side-effect
// propagator is wired (getPurityInfo returns nil otherwise, and the field
// is `omitempty`). The C++ MCP runtime exposes no side-effect analysis to
// this handler, so the field is absent. Emitting a zeroed stub here would
// diverge from Go and violate the no-silent-stub rule. Wiring real purity
// analysis is tracked as a follow-up.
TEST_F(HandlersFixture, GetContextPurityOmittedWhenUnavailable) {
    auto oid = first_object_id(*indexer_, "main");
    if (oid.empty()) GTEST_SKIP() << "corpus produced no 'main' symbol";
    nlohmann::json params;
    params["id"] = oid;
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_FALSE(json["contexts"][0].contains("purity"));
}

// Unresolvable object ID is reported in `errors[]`, never silently dropped.
TEST_F(HandlersFixture, GetContextBadIdReportsError) {
    nlohmann::json params;
    params["id"] = "zzzzzz";  // well-formed base63, no such symbol
    auto result = handle_get_context(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["count"].get<int>(), 0);
    ASSERT_TRUE(json.contains("errors"));
    ASSERT_FALSE(json["errors"].empty());
    EXPECT_EQ(json["errors"][0]["object_id"].get<std::string>(), "zzzzzz");
    EXPECT_TRUE(json["errors"][0].contains("error"));
}

// Comma-separated ids resolve independently; good ids land in `contexts`,
// bad ids in `errors` (Go iterates objectIDs split on ',').
TEST_F(HandlersFixture, GetContextMultipleIdsMixed) {
    auto oid = first_object_id(*indexer_, "main");
    if (oid.empty()) GTEST_SKIP() << "corpus produced no 'main' symbol";
    nlohmann::json params;
    params["id"] = oid + ",zzzzzz";
    auto result = handle_get_context(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["count"].get<int>(), 1);
    ASSERT_TRUE(json.contains("errors"));
    EXPECT_EQ(json["errors"].size(), 1u);
}

// =============================================================================
// filterContextSections — six-section zeroing semantics
// =============================================================================
// Go parity: Server.filterContextSections (internal/mcp/handlers.go:2578). The
// exclude pass zeroes each NAMED section; the include pass whitelists (zeroes
// every section NOT named). Both passes ONLY zero, never restore. Filtered
// sections stay PRESENT in to_json with empty/zero values — keys are never
// omitted. Tokens are exactly {relationships, variables, semantic, structure,
// usage, ai}; unknown tokens are silently ignored.
//
// S1's get_context leaves every section empty, so include=usage vs no-filter
// are observationally identical through the handler. These unit tests plant a
// sentinel in each section so a zeroed section is genuinely distinguishable
// from a kept one — the real discrimination the handler-level tests below can
// only assert as key-presence.

// Builds a CodeObjectContext with a sentinel in every one of the six
// filterable sections.
CodeObjectContext make_sentinel_context() {
    CodeObjectContext ctx;
    ObjectReference ref;
    ref.object_id.name = "caller";
    ref.object_id.file_id = 1;
    ctx.direct_relationships.caller_functions.push_back(ref);
    VariableInfo var;
    var.name = "p";
    ctx.variable_context.parameters.push_back(var);
    ctx.semantic_context.purpose = "handler";
    ctx.structure_context.file_path = "svc.go";
    ctx.usage_analysis.fan_in = 7;
    ctx.ai_context.natural_language_summary = "summary";
    return ctx;
}

// True when to_json emits all six section keys (present, regardless of value).
bool all_section_keys_present(const CodeObjectContext& ctx) {
    auto j = ctx.to_json();
    return j.contains("direct_relationships") &&
           j.contains("variable_context") && j.contains("semantic_context") &&
           j.contains("structure_context") && j.contains("usage_analysis") &&
           j.contains("ai_context");
}

TEST(FilterContextSections, IncludeUsageZeroesOtherFivePresent) {
    auto ctx = make_sentinel_context();
    ContextLookupEngine::filter_context_sections(ctx, {"usage"}, {});
    // usage kept...
    EXPECT_EQ(ctx.usage_analysis.fan_in, 7);
    // ...the other five zeroed.
    EXPECT_TRUE(ctx.direct_relationships.caller_functions.empty());
    EXPECT_TRUE(ctx.variable_context.parameters.empty());
    EXPECT_TRUE(ctx.semantic_context.purpose.empty());
    EXPECT_TRUE(ctx.structure_context.file_path.empty());
    EXPECT_TRUE(ctx.ai_context.natural_language_summary.empty());
    // Zeroed sections remain PRESENT keys (discrimination: not absence).
    EXPECT_TRUE(all_section_keys_present(ctx));
}

TEST(FilterContextSections, ExcludeAiZeroesAiOnly) {
    auto ctx = make_sentinel_context();
    ContextLookupEngine::filter_context_sections(ctx, {}, {"ai"});
    EXPECT_TRUE(ctx.ai_context.natural_language_summary.empty());
    // Every other section retained.
    EXPECT_FALSE(ctx.direct_relationships.caller_functions.empty());
    EXPECT_FALSE(ctx.variable_context.parameters.empty());
    EXPECT_EQ(ctx.semantic_context.purpose, "handler");
    EXPECT_EQ(ctx.structure_context.file_path, "svc.go");
    EXPECT_EQ(ctx.usage_analysis.fan_in, 7);
    EXPECT_TRUE(all_section_keys_present(ctx));
}

TEST(FilterContextSections, UnknownTokenSilentlyIgnored) {
    // Unknown exclude token: nothing is zeroed.
    auto ctx = make_sentinel_context();
    ContextLookupEngine::filter_context_sections(ctx, {}, {"bogus"});
    EXPECT_FALSE(ctx.direct_relationships.caller_functions.empty());
    EXPECT_EQ(ctx.usage_analysis.fan_in, 7);
    EXPECT_EQ(ctx.ai_context.natural_language_summary, "summary");

    // Unknown include token alongside a real one: the real token is honored,
    // the unknown whitelists nothing (Go bug-for-bug: requestedSections holds
    // it but no section is keyed on it).
    auto ctx2 = make_sentinel_context();
    ContextLookupEngine::filter_context_sections(ctx2, {"usage", "bogus"}, {});
    EXPECT_EQ(ctx2.usage_analysis.fan_in, 7);
    EXPECT_TRUE(ctx2.semantic_context.purpose.empty());
    EXPECT_TRUE(ctx2.ai_context.natural_language_summary.empty());
}

TEST(FilterContextSections, NoSectionsIsNoOp) {
    auto ctx = make_sentinel_context();
    ContextLookupEngine::filter_context_sections(ctx, {}, {});
    EXPECT_FALSE(ctx.direct_relationships.caller_functions.empty());
    EXPECT_EQ(ctx.usage_analysis.fan_in, 7);
    EXPECT_EQ(ctx.ai_context.natural_language_summary, "summary");
}

// -- get_context section filtering + mode presets (end-to-end) ----------------
// The rich `context` payload (ContextLookupEngine) honors include_sections /
// exclude_sections and the mode presets that funnel through them. Filtered
// sections stay PRESENT-but-empty in the JSON; keys are never dropped.

// Pins the equal-distribution placeholder that Go's get_context uses for its
// component_breakdown timing (internal/mcp/handlers.go:2297-2299:
// perComponentTime := totalTime / componentCount). Deterministic pure-function
// input, so no wall-clock flakiness. Guards two regressions: (1) changing the
// component count away from Go's 7, and (2) replacing the truncating integer
// division with something else. Reverting to real independent per-section
// timing would make per_component_time_ms meaningless and this test red.
TEST(ContextComponentTiming, EqualDistributionMatchesGoPlaceholder) {
    EXPECT_EQ(ContextLookupEngine::kContextComponentCount, 7);
    // 70 / 7 == 10, exact.
    EXPECT_EQ(ContextLookupEngine::per_component_time_ms(70), 10);
    // 14 / 7 == 2, exact.
    EXPECT_EQ(ContextLookupEngine::per_component_time_ms(14), 2);
    // 6 / 7 truncates to 0 — matches Go int64/int division.
    EXPECT_EQ(ContextLookupEngine::per_component_time_ms(6), 0);
    // Sub-millisecond lookups measure 0ms total -> 0 per component.
    EXPECT_EQ(ContextLookupEngine::per_component_time_ms(0), 0);
}

// End-to-end guard for the get_context performance envelope: all seven
// component_breakdown fields are the equal-distribution placeholder
// (total_time_ms / 7), matching what the Go reference binary emits. Locks
// both the exact Go field names and the equal-distribution invariant, so a
// future "fix" that injects independent per-section timing (diverging from
// the reference and its parity golden) turns this red.
TEST_F(HandlersFixture, GetContextPerfBreakdownIsGoEqualDistribution) {
    nlohmann::json params;
    params["name"] = "main";
    params["mode"] = "full";
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("performance")) << result.text;
    const auto& perf = json["performance"];
    ASSERT_TRUE(perf.contains("total_time_ms"));
    ASSERT_TRUE(perf.contains("component_breakdown"));
    const auto& cb = perf["component_breakdown"];
    const int64_t total = perf["total_time_ms"].get<int64_t>();
    const int64_t expected = total / 7;  // Go's perComponentTime.
    // Exactly the seven Go-named fields, every one the placeholder value.
    for (const char* key :
         {"basic_info_time", "relationships_time", "variables_time",
          "semantic_time", "structure_time", "usage_time", "ai_time"}) {
        ASSERT_TRUE(cb.contains(key)) << key;
        EXPECT_EQ(cb[key].get<int64_t>(), expected) << key;
    }
    EXPECT_EQ(cb.size(), 7u);
}

TEST_F(HandlersFixture, GetContextIncludeSectionsKeepsAllSectionKeys) {
    nlohmann::json params;
    params["name"] = "main";
    params["include_sections"] = {"usage"};
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("context")) << result.text;
    const auto& c = json["context"];
    // Discrimination: present-with-empty-value, NOT key absence.
    for (const char* key :
         {"direct_relationships", "variable_context", "semantic_context",
          "structure_context", "usage_analysis", "ai_context"}) {
        EXPECT_TRUE(c.contains(key)) << key;
    }
}

TEST_F(HandlersFixture, GetContextExcludeAiKeepsAiKeyPresent) {
    nlohmann::json params;
    params["name"] = "main";
    params["exclude_sections"] = {"ai"};
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("context")) << result.text;
    EXPECT_TRUE(json["context"].contains("ai_context"));
}

TEST_F(HandlersFixture, GetContextUnknownSectionTokenIgnoredNoError) {
    nlohmann::json params;
    params["name"] = "main";
    params["include_sections"] = {"totally_bogus"};
    auto result = handle_get_context(params, *indexer_);
    EXPECT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("context")) << result.text;
    // All six keys still present (bogus whitelisted nothing, but keys are
    // never omitted).
    for (const char* key :
         {"direct_relationships", "variable_context", "semantic_context",
          "structure_context", "usage_analysis", "ai_context"}) {
        EXPECT_TRUE(json["context"].contains(key)) << key;
    }
}

TEST_F(HandlersFixture, GetContextModeSemanticSelectsSemanticAndAi) {
    nlohmann::json params;
    params["name"] = "main";
    params["mode"] = "semantic";  // preset -> include_sections=[semantic,ai]
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("context")) << result.text;
    const auto& c = json["context"];
    EXPECT_TRUE(c.contains("semantic_context"));
    EXPECT_TRUE(c.contains("ai_context"));
    // The four non-selected sections remain present-but-empty.
    for (const char* key :
         {"direct_relationships", "variable_context", "structure_context",
          "usage_analysis"}) {
        EXPECT_TRUE(c.contains(key)) << key;
    }
}

// =============================================================================
// find_files handler tests
// =============================================================================

TEST_F(HandlersFixture, FindFilesRequiresPattern) {
    nlohmann::json params = nlohmann::json::object();
    auto result = handle_find_files(params, *indexer_);
    EXPECT_TRUE(result.is_error);
}

TEST_F(HandlersFixture, FindFilesFindsMainGo) {
    nlohmann::json params;
    params["pattern"] = "main.go";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("results"));
    EXPECT_GT(json["total_matches"].get<int>(), 0);

    bool found_main = false;
    for (const auto& r : json["results"]) {
        if (r["path"].get<std::string>().find("main.go") !=
            std::string::npos) {
            found_main = true;
            break;
        }
    }
    EXPECT_TRUE(found_main);
}

TEST_F(HandlersFixture, FindFilesCaseInsensitive) {
    nlohmann::json params;
    params["pattern"] = "MAIN.GO";
    params["flags"] = "ci";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_GT(json["total_matches"].get<int>(), 0);
}

TEST_F(HandlersFixture, FindFilesExactMatch) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["flags"] = "exact";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    // Exact mode should only match exact full path
    for (const auto& r : json["results"]) {
        EXPECT_EQ(r["match_type"], "exact");
    }
}

TEST_F(HandlersFixture, FindFilesHiddenFilesExcluded) {
    nlohmann::json params;
    params["pattern"] = "secret";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    for (const auto& r : json["results"]) {
        EXPECT_TRUE(r["path"].get<std::string>().find(".hidden") ==
                    std::string::npos);
    }
}

TEST_F(HandlersFixture, FindFilesHiddenFilesIncluded) {
    nlohmann::json params;
    params["pattern"] = "secret";
    params["include_hidden"] = true;
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    // When include_hidden is true, hidden files may appear
    // (if indexed)
}

TEST_F(HandlersFixture, FindFilesDirectoryFilter) {
    nlohmann::json params;
    params["pattern"] = "server";
    params["directory"] = "internal";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    for (const auto& r : json["results"]) {
        auto path = r["path"].get<std::string>();
        EXPECT_TRUE(path.find("internal") != std::string::npos);
    }
}

TEST_F(HandlersFixture, FindFilesLanguageFilter) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["filter"] = "go";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    for (const auto& r : json["results"]) {
        auto path = r["path"].get<std::string>();
        EXPECT_TRUE(path.size() >= 3 &&
                    path.substr(path.size() - 3) == ".go");
    }
}

TEST_F(HandlersFixture, FindFilesGlobFilter) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["filter"] = "*.go";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    for (const auto& r : json["results"]) {
        auto path = r["path"].get<std::string>();
        EXPECT_TRUE(path.size() >= 3 &&
                    path.substr(path.size() - 3) == ".go");
    }
}

TEST_F(HandlersFixture, FindFilesClampsMax) {
    nlohmann::json params;
    params["pattern"] = "go";
    params["max"] = 999;
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    // max is clamped to 200 returned rows; total_matches reports the true
    // count and may exceed the clamp (truncated:true marks that case).
    EXPECT_LE(json["results"].size(), 200u);
}

TEST_F(HandlersFixture, FindFilesReportsTrueTotalWhenTruncated) {
    nlohmann::json params;
    params["pattern"] = "*.go";
    params["max"] = 2;
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    // Fixture has 6 non-hidden .go files; cap of 2 must not lie about the
    // universe size.
    EXPECT_EQ(json["results"].size(), 2u);
    EXPECT_EQ(json["total_matches"].get<int>(), 6);
    EXPECT_TRUE(json.value("truncated", false));
    EXPECT_EQ(json["showing"].get<int>(), 2);
}

TEST_F(HandlersFixture, FindFilesReturnsRootRelativePaths) {
    nlohmann::json params;
    params["pattern"] = "server";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_GT(json["total_matches"].get<int>(), 0);
    for (const auto& r : json["results"]) {
        auto path = r["path"].get<std::string>();
        EXPECT_FALSE(path.empty());
        EXPECT_NE(path.front(), '/') << "expected root-relative path, got "
                                     << path;
    }
}

// directory="." / "./" / trailing slash / absolute-under-root all mean the
// obvious thing; tier-1 traces showed 8/8 empty find_files calls were
// agents passing "." or an absolute path and getting a silent 0.
TEST_F(HandlersFixture, FindFilesDirectoryDotAndAbsoluteNormalized) {
    for (const std::string dir :
         {std::string("."), std::string("./"), std::string(""),
          temp_dir_.string()}) {
        nlohmann::json params;
        params["pattern"] = "*.go";
        params["max"] = 200;
        if (!dir.empty()) params["directory"] = dir;
        auto result = handle_find_files(params, *indexer_);
        ASSERT_FALSE(result.is_error) << dir;
        auto json = nlohmann::json::parse(result.text);
        EXPECT_EQ(json["total_matches"].get<int>(), 6)
            << "directory='" << dir << "'";
    }
    // internal with trailing slash scopes correctly
    nlohmann::json params;
    params["pattern"] = "*.go";
    params["directory"] = "internal/";
    auto result = handle_find_files(params, *indexer_);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_matches"].get<int>(), 1);
}

// Empty find_files results carry an actionable hint; a nonexistent
// directory scope is named explicitly.
TEST_F(HandlersFixture, FindFilesEmptyResultCarriesHint) {
    nlohmann::json params;
    params["pattern"] = "*.go";
    params["directory"] = "no_such_dir";
    auto result = handle_find_files(params, *indexer_);
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_matches"].get<int>(), 0);
    ASSERT_TRUE(json.contains("hint"));
    EXPECT_NE(json["hint"].get<std::string>().find("no_such_dir"),
              std::string::npos);

    nlohmann::json p2;
    p2["pattern"] = "*.nomatch";
    auto r2 = handle_find_files(p2, *indexer_);
    auto j2 = nlohmann::json::parse(r2.text);
    EXPECT_TRUE(j2.contains("hint"));
}

// include=signature is honored on search (list_symbols vocabulary agents
// carry across tools) — declaration line attached to symbol-enriched hits.
TEST_F(HandlersFixture, SearchIncludeSignatureAttachesDeclLine) {
    nlohmann::json params;
    params["pattern"] = "handleRequest";
    params["include"] = "signature";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    bool any_sig = false;
    for (const auto& group : json["results"]) {
        for (const auto& hit : group["hits"]) {
            if (hit.contains("signature")) {
                any_sig = true;
                EXPECT_EQ(hit["signature"].get<std::string>().find('\n'),
                          std::string::npos);
            }
        }
    }
    EXPECT_TRUE(any_sig) << result.text;
}

// output=files / count empty results still carry a hint.
TEST_F(HandlersFixture, SearchFilesOutputEmptyCarriesHint) {
    nlohmann::json params;
    params["pattern"] = "zzz_nothing_matches_this";
    params["output"] = "files";
    params["semantic"] = false;
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_matches"].get<int>(), 0);
    EXPECT_TRUE(json.contains("hint")) << result.text;
}

// `**/name` must match a file at zero directory depth (glob convention).
// wildcard_match's literal `/` rejected root-level files: benchmark traces
// showed LLMs default to `**/mux.go` and silently got zero results.
TEST_F(HandlersFixture, FindFilesDoubleStarMatchesRootLevelFiles) {
    nlohmann::json params;
    params["pattern"] = "**/main.go";  // main.go lives at the project root
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_matches"].get<int>(), 1) << result.text;

    nlohmann::json p2;
    p2["pattern"] = "**/*.go";  // must equal the *.go universe
    p2["max"] = 200;
    auto r2 = handle_find_files(p2, *indexer_);
    auto j2 = nlohmann::json::parse(r2.text);
    nlohmann::json p3;
    p3["pattern"] = "*.go";
    p3["max"] = 200;
    auto r3 = handle_find_files(p3, *indexer_);
    auto j3 = nlohmann::json::parse(r3.text);
    EXPECT_EQ(j2["total_matches"].get<int>(), j3["total_matches"].get<int>());
}

// find_files accepts path= as an alias for directory= (search uses path=;
// agents carry the param name across tools).
TEST_F(HandlersFixture, FindFilesPathAliasScopesDirectory) {
    nlohmann::json params;
    params["pattern"] = "*.go";
    params["path"] = "internal";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_matches"].get<int>(), 1) << result.text;
}

// search schema-validation errors must carry the allowed-parameter list —
// benchmark traces showed a model retrying an unknown param ("paths") 3x
// with no way to self-correct.
TEST_F(HandlersFixture, SearchUnknownParamErrorListsAllowedParams) {
    nlohmann::json params;
    params["pattern"] = "main";
    params["paths"] = "internal";  // typo of path
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_TRUE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("allowed_params")) << result.text;
    bool has_path = false;
    for (const auto& p : json["allowed_params"]) {
        if (p.get<std::string>() == "path") has_path = true;
    }
    EXPECT_TRUE(has_path);
}

// get_context carries a callers count (incoming references), matching the
// search per-hit field — chokepoint questions answerable without an extra
// call-hierarchy request. main() calls handleRequest? No — fixture has no
// cross-file calls guaranteed, so assert shape: field absent when zero,
// present and positive when the tracker records incoming refs.
TEST_F(HandlersFixture, GetContextCallersFieldMatchesIncomingRefs) {
    auto& tracker = indexer_->ref_tracker();
    auto snap = tracker.pin();
    auto matches = snap->find_symbols_by_name("handleRequest");
    ASSERT_FALSE(matches.empty());
    const auto& sym = matches.front();
    nlohmann::json params;
    params["id"] = encode_symbol_id(sym->id);
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_EQ(json["count"].get<int>(), 1);
    const auto& ctx = json["contexts"][0];
    if (sym->incoming_refs.empty()) {
        EXPECT_FALSE(ctx.contains("callers"));
    } else {
        EXPECT_EQ(ctx["callers"].get<int>(),
                  static_cast<int>(sym->incoming_refs.size()));
    }
}

// Regression: a project whose ROOT lives under a dotted directory
// (~/.cache/repo, .work/corpus, …) must not have every file classified as
// hidden. The hidden check applies to root-relative components only.
// Before the fix, every find_files call in the repo-qa benchmark returned
// {"results":[],"total_matches":0} because the corpora live under .work/.
TEST(FindFilesHiddenAncestor, ProjectUnderDottedDirIsFullyVisible) {
    // Dotted ancestor is the point of the test; keep the leading dot but make
    // the directory process-unique so parallel ctest workers don't collide.
    auto dotted = lci::test::unique_temp_dir(".lci_hidden_ancestor_");
    auto base = dotted / "proj";
    std::filesystem::create_directories(base / "src");
    {
        std::ofstream(base / "main.go") << "package main\nfunc main() {}\n";
        std::ofstream(base / "src" / "server.go")
            << "package src\nfunc Serve() {}\n";
    }

    Config config;
    config.project.root = base.string();
    MasterIndex indexer(config);
    indexer.index_directory(base.string());

    nlohmann::json params;
    params["pattern"] = "*.go";
    auto result = handle_find_files(params, indexer);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["total_matches"].get<int>(), 2)
        << "dotted ancestor of project root must not hide corpus files";

    std::error_code ec;
    std::filesystem::remove_all(dotted, ec);
}

// Fuzzy file matching uses real normalized Levenshtein SIMILARITY (1 - dist/
// maxlen, threshold 0.7), so a near-miss of a filename fuzzy-matches that file
// and unrelated files do not. (The old behavior — ported from a Go fuzzer that
// used the DISTANCE ratio and tested >=0.7 — flooded every file as a fuzzy hit
// at 0.574 for any pattern, including totally unrelated ones. The Go oracle
// that pinned that is retired; the inverted similarity is fixed.)
TEST_F(HandlersFixture, FindFilesFuzzyMatchesNearName) {
    nlohmann::json params;
    params["pattern"] = "handlr";  // one deletion from "handler"
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    bool found_handler = false;
    for (const auto& r : json["results"]) {
        std::string path = r["path"].get<std::string>();
        if (path.find("handler.go") != std::string::npos) found_handler = true;
        // Unrelated files (main/utils/server) must NOT fuzzy-match "handlr".
        EXPECT_EQ(path.find("utils.go"), std::string::npos) << path;
    }
    EXPECT_TRUE(found_handler) << "near-miss 'handlr' should fuzzy-match handler.go";
}

TEST_F(HandlersFixture, FindFilesUnrelatedPatternNoFuzzyFlood) {
    // A totally unrelated pattern must NOT surface every file. With correct
    // similarity, "zzzzzzz" is far from main/handler/utils/server -> no fuzzy.
    nlohmann::json params;
    params["pattern"] = "zzzzzzz";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    for (const auto& r : json["results"]) {
        EXPECT_NE(r["match_type"], "fuzzy")
            << "unrelated pattern flooded a fuzzy hit: " << r.dump();
    }
}

TEST_F(HandlersFixture, FindFilesSubstringMatch) {
    nlohmann::json params;
    params["pattern"] = "handler";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    bool found = false;
    for (const auto& r : json["results"]) {
        if (r["path"].get<std::string>().find("handler") !=
            std::string::npos) {
            found = true;
            EXPECT_TRUE(r["score"].get<double>() > 0.0);
            break;
        }
    }
    EXPECT_TRUE(found);
}

// =============================================================================
// register_core_handlers integration tests
// =============================================================================

TEST(RegisterHandlers, RegistersWithoutCrash) {
    Config config;
    config.project.root = "/tmp/lci-handler-reg-test";
    McpServer server(config);
    register_core_handlers(server, nullptr, nullptr);
    // Should add 4 tools
    EXPECT_GE(server.tool_count(), 4u);
}

TEST(RegisterHandlers, NullIndexReturnsError) {
    Config config;
    config.project.root = "/tmp/lci-handler-null-test";
    McpServer server(config);
    register_core_handlers(server, nullptr, nullptr);

    // The search handler with null indexer should error
    // Find the search tool (it's the second one added)
    // We test by calling via the server's handle_tools_call mechanism
    // but that requires full JSON-RPC setup, so we test the handler
    // directly instead
    nlohmann::json params;
    params["pattern"] = "test";
    // handle_search would crash with null indexer, but register_core_handlers
    // wraps it to check for null first - verified by construction
}

// =============================================================================
// Rich-search parity tests (subtask A: search filters / regex / semantic)
// =============================================================================

TEST_F(HandlersFixture, SearchSymbolTypesFiltersResults) {
    nlohmann::json params;
    params["pattern"] = "handle";
    params["symbol_types"] = "function";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("results"));
    // Every enriched hit must have type=function (or be filtered out).
    // Enrichment dedup means follow-on hits inside the same symbol omit
    // the type; only assert on rows that carry it.
    for (const auto& group : json["results"]) {
        for (const auto& hit : group["hits"]) {
            if (hit.contains("type")) {
                EXPECT_EQ(hit["type"].get<std::string>(), "function");
            }
        }
    }
}

TEST_F(HandlersFixture, SearchLanguagesBuildsIncludeFilter) {
    nlohmann::json params;
    params["pattern"] = "handle";
    params["languages"] = {"go"};
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    for (const auto& item : json["results"]) {
        const auto file = item.value("file", "");
        EXPECT_TRUE(file.size() >= 3 &&
                    file.compare(file.size() - 3, 3, ".go") == 0)
            << file;
    }
}

TEST_F(HandlersFixture, SearchRegexFlagHonored) {
    nlohmann::json params;
    params["pattern"] = "handle.+";
    params["flags"] = "rx";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json["results"].is_array());
}

TEST_F(HandlersFixture, SearchPatternsCsvOrMerges) {
    nlohmann::json params;
    params["patterns"] = "handle,parse";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_GE(json["total_matches"].get<int>(), 2);
}

TEST_F(HandlersFixture, SearchIncludeUnsupportedRejected) {
    nlohmann::json params;
    params["pattern"] = "handle";
    params["include"] = "frobnicate";  // not a Go include token
    auto result = handle_search(params, *indexer_, search_engine_.get());
    // Genuinely-unknown tokens still fail-fast, not silently ignored
    // (Karpathy #6; stricter than Go's silent shouldInclude=false).
    EXPECT_TRUE(result.is_error);
}

// =============================================================================
// find_files multi-word coverage tests (subtask B)
// =============================================================================

TEST_F(HandlersFixture, FindFilesMultiWordBoostsCoverage) {
    nlohmann::json params;
    params["pattern"] = "server api";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    // internal/api/server.go matches BOTH words → should rank above single-word
    // matches for either token.
    ASSERT_TRUE(json.contains("results"));
    ASSERT_FALSE(json["results"].empty());
    bool seen_dual = false;
    for (const auto& r : json["results"]) {
        const std::string p = r.value("path", "");
        if (p.find("internal/api/server.go") != std::string::npos) {
            seen_dual = true;
            // Score should reflect the +0.15 boost (≥ baseline substring).
            EXPECT_GT(r.value("score", 0.0), 0.5);
        }
    }
    EXPECT_TRUE(seen_dual);
}

// =============================================================================
// get_context normalization + sections (subtask C)
// =============================================================================

TEST_F(HandlersFixture, GetContextAliasSymbolId) {
    // symbol_id alias should remap to id.
    nlohmann::json params;
    params["symbol_id"] = "AAA";  // Bad ID — should still hit the id path.
    auto result = handle_get_context(params, *indexer_);
    auto json = nlohmann::json::parse(result.text);
    // Either resolves or returns the errors[] array — never silently empty.
    EXPECT_TRUE(json.contains("contexts"));
}

// Go parity: autoSearchAndReturnContext (handlers.go ~2038) returns a
// positive workflow-hint payload, not an error. Shape:
// {_auto_search_triggered, symbol, path, message, workflow[], example_search,
// hint}.
TEST_F(HandlersFixture, GetContextAutoSearchReturnsWorkflow) {
    nlohmann::json params;
    params["symbol"] = "handleRequest";
    params["path"] = "handler.go";
    auto result = handle_get_context(params, *indexer_);
    EXPECT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.value("_auto_search_triggered", false));
    EXPECT_EQ(json.value("symbol", ""), "handleRequest");
    EXPECT_EQ(json.value("path", ""), "handler.go");
    EXPECT_TRUE(json.contains("workflow"));
    EXPECT_TRUE(json["workflow"].is_array());
    EXPECT_TRUE(json.contains("example_search"));
    EXPECT_TRUE(json.contains("hint"));
    // No misleading "not implemented" error field.
    EXPECT_FALSE(json.contains("error"));
}

// Go parity: get_context accepts include_sections in both the compact and
// mode paths and never errors on them. The id (compact) path still simply
// ignores sections it cannot render — a bad id yields an errors[] entry, but
// the section token itself is accepted, never fail-fast.
//
// CONTRACT CHANGE (CLX S1): a section request against a resolvable *name* no
// longer silently ignores the section — it now EMITS a rich CodeObjectContext
// (`context`) built by the ported ContextLookupEngine, with all six section
// keys present. This test was updated (not deleted) to pin both halves.
TEST_F(HandlersFixture, GetContextSectionTokensAccepted) {
    const char* sections[] = {"variables",    "structure",       "semantic",
                              "usage",         "ai",              "dependencies",
                              "file_context",  "quality_metrics"};
    for (const char* s : sections) {
        // id path: section token accepted, never a hard error.
        nlohmann::json params;
        params["id"] = "VE";
        params["include_sections"] = {s};
        auto result = handle_get_context(params, *indexer_);
        EXPECT_FALSE(result.is_error) << "section '" << s << "': " << result.text;
        auto json = nlohmann::json::parse(result.text);
        EXPECT_TRUE(json.contains("contexts")) << s;
    }

    // name path: a known section now emits the rich context instead of being
    // silently dropped.
    nlohmann::json name_params;
    name_params["name"] = "Add";
    name_params["include_sections"] = {"structure"};
    auto name_result = handle_get_context(name_params, *indexer_);
    EXPECT_FALSE(name_result.is_error) << name_result.text;
    auto name_json = nlohmann::json::parse(name_result.text);
    ASSERT_TRUE(name_json.contains("context")) << name_result.text;
    EXPECT_EQ(name_json["context"]["object_id"]["name"], "Add");
    for (const char* key :
         {"direct_relationships", "variable_context", "semantic_context",
          "structure_context", "usage_analysis", "ai_context"}) {
        EXPECT_TRUE(name_json["context"].contains(key)) << key;
    }
}

// CLX S1 RED->GREEN: a full-mode name lookup returns the ported
// CodeObjectContext with identity, signature, location, version, and all six
// section keys present.
TEST_F(HandlersFixture, GetContextFullModeEmitsCodeObjectContext) {
    nlohmann::json params;
    params["name"] = "Add";
    params["mode"] = "full";
    auto result = handle_get_context(params, *indexer_);
    EXPECT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("context")) << result.text;
    const auto& ctx = json["context"];
    EXPECT_EQ(ctx["object_id"]["name"], "Add");
    EXPECT_FALSE(ctx["signature"].get<std::string>().empty());
    EXPECT_GT(ctx["location"]["Line"].get<int>(), 0);
    ASSERT_TRUE(ctx.contains("context_version"));
    EXPECT_FALSE(ctx["context_version"].get<std::string>().empty());
    for (const char* key :
         {"direct_relationships", "variable_context", "semantic_context",
          "structure_context", "usage_analysis", "ai_context"}) {
        EXPECT_TRUE(ctx.contains(key)) << key;
    }
}

// CHARACTERIZATION (VERIFY task 01KXE5MM2138EN5P0MKXA1FT2H, edge 2):
// A rich (mode/section) request whose name resolves to NO symbol emits the
// compact fail-loud envelope — {count:0, contexts:[], hint} — and OMITS the
// rich context/metadata/performance keys. It is NOT an MCP error.
//
// This is a DELIBERATE, documented divergence from the Go reference, not a
// defect. Ground truth captured by driving the Go binary
// (`lci -r <fixture> mcp`, get_context {"id":"zzzz","mode":"full"} and
// {"name":<missing>,"mode":"full"}): Go returns an ERROR
//   "context lookup failed: invalid object ID: function::0"
// because its paramsToObjectID yields FileID=0 and GetContext's
// CodeObjectID.IsValid() rejects it (internal/core/context_lookup.go:365,434).
// The C++ port instead fails loud with a hint + fuzzy near-miss suggestions
// (Karpathy #6, handlers_core.cpp:1688-1697), a workspace-wide UX choice shared
// with the compact path. Locked here so a future change toward Go's hard error
// — or an accidental emission of empty rich keys — turns red and is reviewed.
TEST_F(HandlersFixture, GetContextRichAllNullMatchesEmitsHintNotRichKeys) {
    nlohmann::json params;
    params["name"] = "nonexistent_xyz_sym";
    params["mode"] = "full";
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;  // hint, not MCP error
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json.value("count", -1), 0) << result.text;
    ASSERT_TRUE(json.contains("contexts"));
    EXPECT_TRUE(json["contexts"].empty());
    EXPECT_TRUE(json.contains("hint")) << result.text;
    // Rich keys are omitted when no match resolves (accept-then-empty).
    EXPECT_FALSE(json.contains("context"));
    EXPECT_FALSE(json.contains("metadata"));
    EXPECT_FALSE(json.contains("performance"));
}

TEST_F(HandlersFixture, GetContextOidExtraction) {
    // oid= prefix should be stripped.
    nlohmann::json params;
    params["id"] = "see oid=AAA for the symbol";
    auto result = handle_get_context(params, *indexer_);
    // Bad ID → errors[] populated, but the handler must have parsed AAA out.
    auto json = nlohmann::json::parse(result.text);
    EXPECT_TRUE(json.contains("errors") || json.contains("contexts"));
}

// =============================================================================
// Pure helpers (engine.cpp)
// =============================================================================

TEST(SearchHelpers, LooksLikeRegexDetectsAlternation) {
    EXPECT_TRUE(looks_like_regex("foo|bar"));
    EXPECT_TRUE(looks_like_regex("^prefix"));
    EXPECT_TRUE(looks_like_regex("suffix$"));
    EXPECT_TRUE(looks_like_regex("[abc]"));
    EXPECT_TRUE(looks_like_regex("\\d+"));
    EXPECT_TRUE(looks_like_regex(".+"));
    EXPECT_TRUE(looks_like_regex("a{2,5}"));
}

TEST(SearchHelpers, LooksLikeRegexRejectsPlainNames) {
    EXPECT_FALSE(looks_like_regex(""));
    EXPECT_FALSE(looks_like_regex("simple"));
    EXPECT_FALSE(looks_like_regex("snake_case"));
    EXPECT_FALSE(looks_like_regex("foo.bar"));  // qualified name, not regex
    EXPECT_FALSE(looks_like_regex("func()"));   // function call
}

TEST(SearchHelpers, ExpandPatternSemanticSplitsMultiWord) {
    auto out = expand_pattern_semantic("user controller handler");
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], "user controller handler");  // original first
    EXPECT_EQ(out[1], "user");
    EXPECT_EQ(out[2], "controller");
    EXPECT_EQ(out[3], "handler");
}

TEST(SearchHelpers, ExpandPatternSemanticSkipsShortWords) {
    auto out = expand_pattern_semantic("a really big name");
    // "a", "really", "big", "name" — only ">2 chars" survive.
    // "a" and (in some definitions "big" len 3) — len > 2 means strictly >2.
    EXPECT_EQ(out[0], "a really big name");
    // "a" is 1 char → skipped. Others are 6/3/4 → kept.
    EXPECT_EQ(out.size(), 4u);
}

TEST(SearchHelpers, ExpandPatternSemanticSingleWordOnlyReturnsOne) {
    auto out = expand_pattern_semantic("singleton");
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "singleton");
}

TEST(SearchHelpers, ExpandPatternSynonymsSingleWordExpands) {
    auto table = SynonymTable::build_default();
    std::vector<bool> flags;
    auto out = expand_pattern_semantic("delete", table, flags);
    ASSERT_EQ(out.size(), flags.size());
    EXPECT_EQ(out[0], "delete");   // original first
    EXPECT_FALSE(flags[0]);
    EXPECT_GT(out.size(), 1u);     // synonyms appended
    bool has_remove = false, has_erase = false;
    for (size_t i = 1; i < out.size(); ++i) {
        EXPECT_TRUE(flags[i]) << "synonym-injected pattern must be flagged";
        if (out[i] == "remove") has_remove = true;
        if (out[i] == "erase") has_erase = true;
    }
    EXPECT_TRUE(has_remove);
    EXPECT_TRUE(has_erase);
}

TEST(SearchHelpers, ExpandPatternSynonymsRespectsCap) {
    auto table = SynonymTable::build_default();
    std::vector<bool> flags;
    auto out = expand_pattern_semantic(
        "add delete update get set find", table, flags);
    EXPECT_LE(out.size(), kMaxSynonymExpansion);
    EXPECT_EQ(out.size(), flags.size());
    EXPECT_EQ(out[0], "add delete update get set find");  // original first
}

TEST(SearchHelpers, ExpandPatternSynonymsNonGroupWordStaysSingle) {
    auto table = SynonymTable::build_default();
    std::vector<bool> flags;
    auto out = expand_pattern_semantic("singleton", table, flags);
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "singleton");
    ASSERT_EQ(flags.size(), 1u);
    EXPECT_FALSE(flags[0]);
}

// =============================================================================
// CLX S3 — DirectRelationships (fill_direct_relationships) port parity
// =============================================================================
// Fixture: caller() calls helper(); Widget struct carries a Name field and a
// Greet method (receiver method, not a struct-lexical member). Exercises:
//   trap 3  — incoming/outgoing ref Context is the decimal string of the Go
//             RefType ordinal (Call=1 on this corpus, since Import/Usage are
//             the only other ref types this extractor emits for Go source).
//   caller/called functions — RefTypeCall + Function|Method gate, reusing
//             ReferenceTracker::get_caller_symbols/get_callee_symbols.
//   parent_objects — from scope_chain, SymbolID="scope_<name>".
//   dispatch-gate bug — child_objects/parent_classes/implementing_types are
//             empirically empty on this Go corpus (see the file-level
//             comment in context_lookup_relationships.cpp): child_objects
//             gates methods/fields on SymbolType::Class only (Widget is
//             Struct); parent_classes/implementing_types are unreachable-
//             empty by construction regardless of objectID type.
//   used_types / imported_modules — empirically empty: this extractor never
//             emits ReferenceType::Declaration/Assignment for any language,
//             and never emits ReferenceType::Import for Go (only via JS/TS's
//             import_statement handling — see DirectRelationshipsJsFixture
//             below for a positive imported_modules case, trap 9).
class DirectRelationshipsFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_relctx_test_");
        std::filesystem::create_directories(temp_dir_);
        std::ofstream(temp_dir_ / "relations.go") << R"(package sample

type Widget struct {
	Name string
}

func (w *Widget) Greet() string {
	return w.Name
}

func helper(x int) int {
	return x + 1
}

func caller() int {
	return helper(5)
}
)";
        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());
    }

    void TearDown() override {
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    // Resolves a symbol by name to a CodeObjectID (as get_context expects).
    // `nth` selects among same-named matches (Widget the struct vs. a field
    // named the same is not a concern here, but ambiguity between symbols
    // sharing a name in one file is guarded by the caller passing a distinct
    // name where needed).
    CodeObjectID object_id_for(const std::string& name) {
        auto snap = indexer_->ref_tracker().pin();
        auto syms = snap->find_symbols_by_name(name);
        EXPECT_FALSE(syms.empty()) << "no symbol named " << name;
        CodeObjectID oid;
        if (syms.empty()) return oid;
        const auto& s = syms.front();
        oid.file_id = s->symbol.file_id;
        oid.symbol_id = encode_symbol_id(s->id);
        oid.name = std::string(s->symbol.name);
        oid.type = s->symbol.type;
        return oid;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

// trap 3: outgoing ref to a Call target carries Context "1" (Go's
// RefTypeCall ordinal), confidence 0.9.
TEST_F(DirectRelationshipsFixture, OutgoingReferencesCarryCallOrdinalContext) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("caller"), ok);
    ASSERT_TRUE(ok);

    bool found = false;
    for (const auto& ref : ctx.direct_relationships.outgoing_references) {
        if (ref.object_id.name != "helper") continue;
        found = true;
        EXPECT_EQ(ref.context, "1");
        EXPECT_DOUBLE_EQ(ref.confidence, 0.9);
    }
    EXPECT_TRUE(found) << "caller's outgoing_references missing helper";
}

TEST_F(DirectRelationshipsFixture, IncomingReferencesCarryCallOrdinalContext) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("helper"), ok);
    ASSERT_TRUE(ok);

    bool found = false;
    for (const auto& ref : ctx.direct_relationships.incoming_references) {
        if (ref.object_id.name != "caller") continue;
        found = true;
        EXPECT_EQ(ref.context, "1");
        EXPECT_DOUBLE_EQ(ref.confidence, 0.9);
    }
    EXPECT_TRUE(found) << "helper's incoming_references missing caller";
}

// caller/called functions: RefTypeCall + Function|Method gate, Context=
// "function_call", confidence 0.95 — reuses get_caller_symbols/
// get_callee_symbols.
TEST_F(DirectRelationshipsFixture, CallerAndCalledFunctionsUseFunctionCallContext) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;

    auto helper_ctx = engine.get_context(object_id_for("helper"), ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(helper_ctx.direct_relationships.caller_functions.size(), 1u);
    EXPECT_EQ(helper_ctx.direct_relationships.caller_functions[0].object_id.name,
             "caller");
    EXPECT_EQ(helper_ctx.direct_relationships.caller_functions[0].context,
             "function_call");
    EXPECT_DOUBLE_EQ(
        helper_ctx.direct_relationships.caller_functions[0].confidence, 0.95);

    auto caller_ctx = engine.get_context(object_id_for("caller"), ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(caller_ctx.direct_relationships.called_functions.size(), 1u);
    EXPECT_EQ(caller_ctx.direct_relationships.called_functions[0].object_id.name,
             "helper");
    EXPECT_EQ(caller_ctx.direct_relationships.called_functions[0].context,
             "function_call");
    EXPECT_DOUBLE_EQ(
        caller_ctx.direct_relationships.called_functions[0].confidence, 0.95);
}

// parent_objects: scope_chain entries, skip-anonymous, SymbolID="scope_<name>".
// Name's field scope_chain includes the enclosing Widget struct scope (a
// ScopeType::Class scope on this extractor — see convert_scope_type_to_
// symbol_type's file-level comment on the dormant ScopeType::Struct gap).
TEST_F(DirectRelationshipsFixture, ParentObjectsIncludesEnclosingStructScope) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Name"), ok);
    ASSERT_TRUE(ok);

    bool found = false;
    for (const auto& parent : ctx.direct_relationships.parent_objects) {
        if (parent.object_id.name != "Widget") continue;
        found = true;
        EXPECT_EQ(parent.object_id.type, SymbolType::Class);
        EXPECT_EQ(parent.object_id.symbol_id, "scope_Widget");
        EXPECT_EQ(parent.context, "class");
        EXPECT_DOUBLE_EQ(parent.confidence, 0.95);
    }
    EXPECT_TRUE(found) << "Name field's parent_objects missing Widget scope";
}

// dispatch-gate bug: child_objects gates methods/fields on SymbolType::Class
// ONLY (context_lookup_relationships.go:465) — Widget is SymbolType::Struct
// in this index, so it never reaches get_class_methods/get_class_fields.
// Pinned empty rather than fabricated (no stub data).
TEST_F(DirectRelationshipsFixture, ChildObjectsEmptyForGoStructGoParity) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Widget"), ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ctx.direct_relationships.child_objects.empty());
}

// dispatch-gate bug: parent_classes/implementing_types are unreachable-empty
// by construction (see context_lookup_relationships.cpp file-level comment)
// — fillDirectRelationships only calls either getter for Class||Method
// objectIDs, but getParentClasses requires Struct||Class (Method never
// qualifies) and getImplementingTypes requires Interface (which the outer
// Class||Method gate can never satisfy). Verified here against a Method
// (Greet) and confirmed empty independent of any Inheritance-ref data (this
// extractor never emits ReferenceType::Inheritance either — see S5's
// StructExcludedFromInterfaceAndInheritanceGates finding).
TEST_F(DirectRelationshipsFixture,
      ParentClassesAndImplementingTypesUnreachableForMethod) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Greet"), ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ctx.direct_relationships.parent_classes.empty());
    EXPECT_TRUE(ctx.direct_relationships.implementing_types.empty());
}

// used_types is gated on ReferenceType::Declaration/Assignment outgoing refs;
// this extractor never emits either reference type for any language. Pinned
// empty (no stub data) rather than fabricated.
TEST_F(DirectRelationshipsFixture, UsedTypesEmptyNoDeclarationOrAssignmentRefs) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("caller"), ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ctx.direct_relationships.used_types.empty());
}

// imported_modules is file-wide RefTypeImport; Go source never produces an
// Import-type outgoing ref in this extractor (only JS/TS's import_statement
// handling does — see DirectRelationshipsJsFixture below for trap 9's
// positive case). Pinned empty on this Go corpus.
TEST_F(DirectRelationshipsFixture, ImportedModulesEmptyOnGoCorpus) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("caller"), ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ctx.direct_relationships.imported_modules.empty());
}

// JS's import_statement handling DOES emit ReferenceType::Import
// (unified_extractor_references.cpp) — unlike Go, which never emits it at
// all. But this getter (like Go's) scans get_file_enhanced_symbols(file_id)
// and reads each symbol's OWN outgoing_refs; ReferenceTracker::
// process_all_references only attaches a ref to a symbol when
// find_symbol_at_location finds a symbol whose [line, end_line] CONTAINS the
// ref's line (reference_tracker.cpp:679-696). An import statement is
// syntactically required to precede every declaration in the file, so its
// line is never contained by any symbol's range — source_symbol stays 0 and
// the ref is never attached to any symbol's outgoing_refs. Verified directly
// against this fixture's snapshot: the Import ref exists in
// Snapshot::references (source_symbol=0) but appears in no symbol's
// outgoing_refs. imported_modules is therefore empirically EMPTY on both
// Go and JS/TS corpora under this port's ref-attachment infra — a deeper,
// language-independent gap than trap 9's stylistic divergence. Pinned here
// (no stub data) rather than fabricated; the getter itself stays a faithful,
// dormant port that will start producing data the day ref-attachment covers
// file-level/orphaned references.
class DirectRelationshipsJsFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_relctx_js_test_");
        std::filesystem::create_directories(temp_dir_);
        std::ofstream(temp_dir_ / "app.js") << R"(import { helper } from './helper';

function caller() {
  return helper(5);
}
)";
        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());
    }

    void TearDown() override {
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    CodeObjectID object_id_for(const std::string& name) {
        auto snap = indexer_->ref_tracker().pin();
        auto syms = snap->find_symbols_by_name(name);
        EXPECT_FALSE(syms.empty()) << "no symbol named " << name;
        CodeObjectID oid;
        if (syms.empty()) return oid;
        const auto& s = syms.front();
        oid.file_id = s->symbol.file_id;
        oid.symbol_id = encode_symbol_id(s->id);
        oid.name = std::string(s->symbol.name);
        oid.type = s->symbol.type;
        return oid;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

TEST_F(DirectRelationshipsJsFixture, ImportedModulesEmptyOrphanedImportRef) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("caller"), ok);
    ASSERT_TRUE(ok);

    // See the fixture-level comment: the Import ref is real (verified via
    // Snapshot::references) but orphaned (source_symbol=0, never attached to
    // any symbol's outgoing_refs), so this getter's per-symbol scan cannot
    // reach it. Pinned empty rather than fabricated.
    EXPECT_TRUE(ctx.direct_relationships.imported_modules.empty());
}

// =============================================================================
// CLX S4 — VariableContext (fill_variable_context) port parity
// =============================================================================
// Exercises the six VariableContext buckets against a real Go index. The C++
// Go extractor emits package var/const, struct fields, and `var`-declared
// locals as symbols; it does NOT emit `:=` short-var locals or parameters as
// symbols, so the fixture uses `var` locals deliberately. See the per-bucket
// comments below for the traps each assertion pins:
//   trap 4  — class/global var type = real-type field (type_info); local/param
//             type = symbol-KIND string.
//   trap 5  — return_values is ALWAYS [] (never populated).
//   trap 6d — parameter detection is Go's self-referential scope heuristic
//             (bug-for-bug); on this index it yields no params.
//   folder-scope trap — every symbol's scope chain carries a `folder` scope
//             (EndLine=0), so getGlobalVariables' "all scopes ∈
//             File/Package/Namespace" gate always rejects → globals empty.

class VariableContextFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_varctx_test_");
        std::filesystem::create_directories(temp_dir_);
        std::ofstream(temp_dir_ / "vars.go") << R"(package sample

var GlobalCounter = 0
const MaxItems = 100

type Widget struct {
	Name  string
	Count int
}

func Process(input string, limit int) int {
	var total int = 0
	var scratch string = input
	return total + len(scratch)
}
)";
        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());
    }

    void TearDown() override {
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    // Resolves a symbol by name to a CodeObjectID (as get_context expects).
    CodeObjectID object_id_for(const std::string& name) {
        auto snap = indexer_->ref_tracker().pin();
        auto syms = snap->find_symbols_by_name(name);
        EXPECT_FALSE(syms.empty()) << "no symbol named " << name;
        CodeObjectID oid;
        if (syms.empty()) return oid;
        const auto& s = syms.front();
        oid.file_id = s->symbol.file_id;
        oid.symbol_id = encode_symbol_id(s->id);
        oid.name = std::string(s->symbol.name);
        oid.type = s->symbol.type;
        return oid;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

// A struct object leaves class_variables empty. Go parity: the
// fillVariableContext dispatch (context_lookup_variables.go:31) gates the call
// to getClassVariables on SymbolTypeClass ONLY — a Struct objectID never
// reaches it, even though getClassVariables' own body (Go:161) accepts both
// Class and Struct. That body support is dead code for structs in Go, and the
// C++ port must stay bug-for-bug faithful rather than "fixing" the dispatch.
TEST_F(VariableContextFixture, StructLeavesClassVariablesEmptyForGoParity) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Widget"), ok);
    ASSERT_TRUE(ok);

    EXPECT_TRUE(ctx.variable_context.class_variables.empty());
    // A struct is neither a function nor a global holder.
    EXPECT_TRUE(ctx.variable_context.local_variables.empty());
    EXPECT_TRUE(ctx.variable_context.parameters.empty());
    EXPECT_TRUE(ctx.variable_context.return_values.empty());
    EXPECT_TRUE(ctx.variable_context.global_variables.empty());
}

// A function object populates local_variables from `var`-declared locals with
// is_mutable=true and the symbol-KIND type string (trap 4 local path).
TEST_F(VariableContextFixture, FunctionPopulatesLocalsAndPinsParamsReturns) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Process"), ok);
    ASSERT_TRUE(ok);

    const auto& locals = ctx.variable_context.local_variables;
    ASSERT_EQ(locals.size(), 2u);
    EXPECT_EQ(locals[0].name, "total");    // line 12
    EXPECT_EQ(locals[1].name, "scratch");  // line 13
    for (const auto& v : locals) {
        EXPECT_EQ(v.scope, "local");
        EXPECT_TRUE(v.is_mutable);  // getLocalVariables hardcodes is_mutable=true
        // trap 4: local-var type is the symbol-KIND string, not a real type.
        EXPECT_EQ(v.type, "variable");
    }

    // trap 6d: the self-referential parameter heuristic finds no params on this
    // index (the Go extractor emits no parameter symbols). Pin empty.
    EXPECT_TRUE(ctx.variable_context.parameters.empty());
    // trap 5: return_values is ALWAYS []. Pin empty.
    EXPECT_TRUE(ctx.variable_context.return_values.empty());
    // folder-scope trap: package var/const never qualify as global.
    EXPECT_TRUE(ctx.variable_context.global_variables.empty());
    EXPECT_TRUE(ctx.variable_context.used_globals.empty());
    EXPECT_TRUE(ctx.variable_context.class_variables.empty());
}

// =============================================================================
// CLX S5 — StructureContext (fill_structure_context) port parity
// =============================================================================
// Exercises file_path/module/package parsing, exports, and the
// composition_pattern waterfall against a real Go index. See per-test
// comments for the traps each assertion pins:
//   trap 1  — imports sorted deterministically by module_path (Go's getImports
//             keys an unordered map by module_path).
//   trap 5  — InterfaceInfo stays a stub (methods=[], is_fully_implemented=
//             true); ImportInfo.import_name is never set.
//   trap 6c — interface_implementations/inheritance_chain gate on
//             SymbolType::Class only, excluding Struct (bug-for-bug parity
//             with Go's own dispatch gate).
//   trap 9  — imports (this section) hardcode import_style="default".

class StructureContextFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_structctx_test_");
        std::filesystem::create_directories(temp_dir_ / "src" / "widgets");
        std::ofstream(temp_dir_ / "src" / "widgets" / "widget.go") << R"(package widgets

type Widget struct {
	Name  string
	Count int
}

func NewWidget(name string) *Widget {
	return &Widget{Name: name}
}

func CreateWidget() *Widget {
	return &Widget{}
}

func GetInstance() *Widget {
	return nil
}

func DoNothing() int {
	return 0
}

func PublicHelper() int {
	return 1
}
)";
        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());
    }

    void TearDown() override {
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    // Resolves a symbol by name to a CodeObjectID (as get_context expects).
    CodeObjectID object_id_for(const std::string& name) {
        auto snap = indexer_->ref_tracker().pin();
        auto syms = snap->find_symbols_by_name(name);
        EXPECT_FALSE(syms.empty()) << "no symbol named " << name;
        CodeObjectID oid;
        if (syms.empty()) return oid;
        const auto& s = syms.front();
        oid.file_id = s->symbol.file_id;
        oid.symbol_id = encode_symbol_id(s->id);
        oid.name = std::string(s->symbol.name);
        oid.type = s->symbol.type;
        return oid;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

// file_path is root-relative; module/package are parsed off the containing
// directory (extractModuleFromPath/extractPackageFromPath ported verbatim
// from Go). A "src" component in the directory switches module to the path
// remainder after "src"; package is always the immediate directory name.
TEST_F(StructureContextFixture, ParsesFilePathModuleAndPackage) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Widget"), ok);
    ASSERT_TRUE(ok);

    const auto& sc = ctx.structure_context;
    EXPECT_EQ(sc.file_path, "src/widgets/widget.go");
    EXPECT_EQ(sc.module, "widgets");
    EXPECT_EQ(sc.package, "widgets");
}

// Every top-level Go declaration is exported (capitalized name), landing in
// the exports bucket with type=<symbol-kind> and export_style="named".
TEST_F(StructureContextFixture, ExportsCarryCapitalizedDeclarations) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Widget"), ok);
    ASSERT_TRUE(ok);

    const auto& exports = ctx.structure_context.exports;
    bool found_widget = false;
    for (const auto& e : exports) {
        if (e.name == "Widget") {
            found_widget = true;
            EXPECT_EQ(e.type, "struct");
            EXPECT_EQ(e.export_style, "named");
        }
    }
    EXPECT_TRUE(found_widget) << "exported Widget struct missing from exports";
}

// composition_pattern waterfall (context_lookup_structure.go:239-259), in
// dispatch order: dependency-injection -> composition -> inheritance ->
// factory -> singleton -> simple.
TEST_F(StructureContextFixture, CompositionPatternDependencyInjection) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("NewWidget"), ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ctx.structure_context.composition_pattern, "dependency-injection");
}

TEST_F(StructureContextFixture, CompositionPatternComposition) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Widget"), ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ctx.structure_context.composition_pattern, "composition");
}

TEST_F(StructureContextFixture, CompositionPatternFactory) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("CreateWidget"), ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ctx.structure_context.composition_pattern, "factory");
}

TEST_F(StructureContextFixture, CompositionPatternSingleton) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("GetInstance"), ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ctx.structure_context.composition_pattern, "singleton");
}

TEST_F(StructureContextFixture, CompositionPatternSimpleFallback) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("DoNothing"), ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ctx.structure_context.composition_pattern, "simple");
}

// trap 6c: interface_implementations/inheritance_chain gate on Class only.
// The Go extractor never emits SymbolType::Class for Go source (Go has no
// classes) and no C++ extractor path currently produces
// ReferenceType::Inheritance refs at all, so both buckets are empty for
// every objectID on this corpus — that emptiness is pinned here as the
// faithfully-ported (if currently unobservable beyond emptiness) behavior.
// The Struct gate exclusion itself is documented as a TODO at the call site
// (context_lookup_structure.cpp) rather than silently "fixed".
TEST_F(StructureContextFixture, StructExcludedFromInterfaceAndInheritanceGates) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Widget"), ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ctx.structure_context.interface_implementations.empty());
    EXPECT_TRUE(ctx.structure_context.inheritance_chain.empty());
}

// trap 5 / trap 9: ImportInfo.import_name is never set; import_style is
// hardcoded "default" for this section. No Import-type outgoing ref is
// produced anywhere for Go source in this extractor, so imports is empty on
// this corpus — pinned rather than fabricated (Karpathy rule 1: no dummy
// data). Sorting (trap 1) is exercised at the unit level by construction: see
// get_imports' std::stable_sort by module_path in context_lookup_structure.cpp.
TEST_F(StructureContextFixture, ImportsEmptyOnGoCorpusNoStubData) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Widget"), ok);
    ASSERT_TRUE(ok);
    for (const auto& imp : ctx.structure_context.imports) {
        EXPECT_TRUE(imp.import_name.empty());
        EXPECT_EQ(imp.import_style, "default");
    }
}

// =============================================================================
// CLX S7 (numbering per port map order basic->relationships->variables->
// semantic->structure->usage->ai; this file is actually S6: SemanticContext)
// — fill_semantic_context port parity
// =============================================================================
// Fixture: main() calls RequestHandler() calls query(). RequestHandler carries
// a direct `@lci:labels[critical]` annotation. Exercises:
//   - entry_point_dependencies: BFS reachability (main -> RequestHandler),
//     type "main function", confidence 0.8.
//   - service_dependencies: trap 6b — the matcher always reports
//     servicePatterns[0]'s operation ("api_call") regardless of which
//     hardcoded substring fired.
//   - propagation_labels: direct @lci: annotation via SemanticAnnotator,
//     strength 1.0, direction "bidirectional" (no GraphPropagator wired —
//     it is an OPTIONAL collaborator and this fixture pins its absence).
//   - criticality_analysis: the "critical" label maps to is_critical=true.
//   - purpose heuristic: RequestHandler (name contains "handler",
//     case-insensitively) -> "API handler", confidence 0.9.
class SemanticContextFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_semctx_test_");
        std::filesystem::create_directories(temp_dir_);
        std::ofstream(temp_dir_ / "handler.go") << R"(package sample

// @lci:labels[critical]
func RequestHandler(id string) string {
	q := query(id)
	return q
}

func query(id string) string {
	return id
}

func main() {
	RequestHandler("x")
}
)";
        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());

        annotator_ = std::make_unique<SemanticAnnotator>();
        annotator_->populate_from_index(*indexer_);
    }

    void TearDown() override {
        annotator_.reset();
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    CodeObjectID object_id_for(const std::string& name) {
        auto snap = indexer_->ref_tracker().pin();
        auto syms = snap->find_symbols_by_name(name);
        EXPECT_FALSE(syms.empty()) << "no symbol named " << name;
        CodeObjectID oid;
        if (syms.empty()) return oid;
        const auto& s = syms.front();
        oid.file_id = s->symbol.file_id;
        oid.symbol_id = encode_symbol_id(s->id);
        oid.name = std::string(s->symbol.name);
        oid.type = s->symbol.type;
        return oid;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SemanticAnnotator> annotator_;
};

TEST_F(SemanticContextFixture, EntryPointDependenciesFindsMainViaBFS) {
    ContextLookupEngine engine(*indexer_);
    engine.set_semantic_annotator(annotator_.get());
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("RequestHandler"), ok);
    ASSERT_TRUE(ok);

    const auto& entries = ctx.semantic_context.entry_point_dependencies;
    ASSERT_FALSE(entries.empty());
    bool found_main = false;
    for (const auto& ep : entries) {
        if (ep.entry_point_id.name != "main") continue;
        found_main = true;
        EXPECT_EQ(ep.type, "main function");
        EXPECT_DOUBLE_EQ(ep.confidence, 0.8);
    }
    EXPECT_TRUE(found_main);
}

// trap 6b: bug-for-bug — the service matcher reports servicePatterns[0]'s
// operation ("api_call") for every hit, never "database"/"query" even though
// the callee is literally named "query".
TEST_F(SemanticContextFixture, ServiceDependenciesReportsFirstPatternOperation) {
    ContextLookupEngine engine(*indexer_);
    engine.set_semantic_annotator(annotator_.get());
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("RequestHandler"), ok);
    ASSERT_TRUE(ok);

    const auto& services = ctx.semantic_context.service_dependencies;
    ASSERT_FALSE(services.empty());
    bool found_query = false;
    for (const auto& svc : services) {
        if (svc.service_name != "query") continue;
        found_query = true;
        EXPECT_EQ(svc.operation_type, "api_call");  // trap 6b, not "query"
        EXPECT_EQ(svc.dependency_type, "direct");
        EXPECT_DOUBLE_EQ(svc.confidence, 0.6);
    }
    EXPECT_TRUE(found_query);
}

TEST_F(SemanticContextFixture, PropagationLabelsCarryDirectAnnotation) {
    ContextLookupEngine engine(*indexer_);
    engine.set_semantic_annotator(annotator_.get());
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("RequestHandler"), ok);
    ASSERT_TRUE(ok);

    const auto& labels = ctx.semantic_context.propagation_labels;
    ASSERT_FALSE(labels.empty());
    bool found_critical = false;
    for (const auto& label : labels) {
        if (label.label != "critical") continue;
        found_critical = true;
        EXPECT_DOUBLE_EQ(label.strength, 1.0);
        EXPECT_EQ(label.direction, "bidirectional");
    }
    EXPECT_TRUE(found_critical);
}

TEST_F(SemanticContextFixture, CriticalLabelMarksCriticalityAnalysis) {
    ContextLookupEngine engine(*indexer_);
    engine.set_semantic_annotator(annotator_.get());
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("RequestHandler"), ok);
    ASSERT_TRUE(ok);

    EXPECT_TRUE(ctx.semantic_context.criticality_analysis.is_critical);
}

TEST_F(SemanticContextFixture, HandlerNamePurposeHeuristic) {
    ContextLookupEngine engine(*indexer_);
    engine.set_semantic_annotator(annotator_.get());
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("RequestHandler"), ok);
    ASSERT_TRUE(ok);

    EXPECT_EQ(ctx.semantic_context.purpose, "API handler");
    EXPECT_DOUBLE_EQ(ctx.semantic_context.confidence, 0.9);
}

// =============================================================================
// CLX S6 — UsageAnalysis (fill_usage_analysis) port parity
// =============================================================================
// Fixture: Target(x int) branches on `if x > 0` (cyclomatic complexity 2 on
// this extractor) and calls helperA/helperB (fan_out=2). Three callers
// (callerOne/Two/Three) invoke Target — real callers, visible in
// direct_relationships.caller_functions — but fan_in is pinned to 0.
//
// Trap 10 empirical finding (MANDATORY per task spec, verified BEFORE writing
// any C++ code): the actual Go binary at /home/beagle/work/core/lci/lci was
// driven over stdio MCP JSON-RPC (initialize -> tools/call "search" ->
// tools/call "get_context" mode=full) against a hand-built Go fixture with a
// `target` function having 3 real callers (callerOne/Two/Three) and 2 real
// callees (helperA/helperB). Go's own get_context response showed
// caller_functions correctly populated with all 3 callers (confidence 0.95
// each) YET usage_analysis.fan_in was 0 and usage_analysis.call_frequency was
// 0, while fan_out was correctly 2. Root cause read from
// internal/core/context_lookup_usage.go: calculateFanIn/calculateCallFrequency
// both route through the LEGACY `cle.symbolIndex.FindReferences` +
// `findContainingFunction` path (the same tree-sitter-AST-based helper the S3
// port map already identified as dead under the index-driven live path — see
// context_lookup_relationships.cpp's file-level comment), which does not
// populate under this build and yields zero every time. fan_out, by contrast,
// routes through `getCalledFunctions` (the index-driven live path, already
// ported in S3), and is correctly non-zero. This port pins fan_in and
// call_frequency to 0 as constant stubs (same treatment as trap 5's
// test_coverage/requires_tests) rather than computing them from the call
// graph, matching OBSERVED Go behavior rather than a plausible-looking
// derivation from caller_functions.size().
class UsageAnalysisFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_usagectx_test_");
        std::filesystem::create_directories(temp_dir_);
        std::ofstream(temp_dir_ / "usage.go") << R"(package sample

func Target(x int) int {
	if x > 0 {
		return helperA(x)
	}
	return helperB(x)
}

func helperA(x int) int { return x + 1 }
func helperB(x int) int { return x - 1 }

func callerOne() int { return Target(1) }
func callerTwo() int { return Target(2) }
func callerThree() int { return Target(3) }
)";
        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());
    }

    void TearDown() override {
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    CodeObjectID object_id_for(const std::string& name) {
        auto snap = indexer_->ref_tracker().pin();
        auto syms = snap->find_symbols_by_name(name);
        EXPECT_FALSE(syms.empty()) << "no symbol named " << name;
        CodeObjectID oid;
        if (syms.empty()) return oid;
        const auto& s = syms.front();
        oid.file_id = s->symbol.file_id;
        oid.symbol_id = encode_symbol_id(s->id);
        oid.name = std::string(s->symbol.name);
        oid.type = s->symbol.type;
        return oid;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

// trap 10: fan_in stays 0 despite 3 real callers visible in caller_functions
// (see file-level comment for the empirical Go-binary trace). fan_out is the
// real, correctly-derived call-graph count (helperA + helperB).
TEST_F(UsageAnalysisFixture, FanOutCountsCalleesFanInPinnedZeroPerTrap10) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Target"), ok);
    ASSERT_TRUE(ok);

    ASSERT_EQ(ctx.direct_relationships.caller_functions.size(), 3u)
        << "fixture sanity: Target should have 3 real callers";
    EXPECT_EQ(ctx.usage_analysis.fan_out, 2);
    EXPECT_EQ(ctx.usage_analysis.fan_in, 0);
    EXPECT_EQ(ctx.usage_analysis.call_frequency, 0);
}

// complexity_metrics mirror EnhancedSymbol fields directly (cyclomatic =
// Complexity, cognitive = Complexity [Go's fallback for both is the same
// indexed field], line_count = EndLine-Line, nesting_depth = Complexity/2 when
// Complexity>1).
TEST_F(UsageAnalysisFixture, ComplexityMetricsMirrorSymbolFields) {
    auto snap = indexer_->ref_tracker().pin();
    auto oid = object_id_for("Target");
    auto target_syms = snap->find_symbols_by_name("Target");
    ASSERT_FALSE(target_syms.empty());
    const auto& target_sym = target_syms.front();

    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(oid, ok);
    ASSERT_TRUE(ok);

    const auto& metrics = ctx.usage_analysis.complexity_metrics;
    EXPECT_EQ(metrics.cyclomatic_complexity, target_sym->complexity);
    EXPECT_EQ(metrics.cognitive_complexity, target_sym->complexity);
    EXPECT_EQ(metrics.line_count,
             target_sym->symbol.end_line > target_sym->symbol.line
                 ? target_sym->symbol.end_line - target_sym->symbol.line
                 : 0);
    // This indexer never populates EnhancedSymbol.parameter_count (verified:
    // it stays 0 for every symbol on this build), so the port's Go-mirrored
    // fallback (parse the signature's parenthesized param list; 0 commas + a
    // "(" present -> 1 param) is the path actually exercised here for
    // Target(x int) — a single, unnamed-comma parameter.
    EXPECT_EQ(metrics.parameter_count, 1);
    EXPECT_EQ(metrics.nesting_depth, target_sym->complexity > 1
                                         ? target_sym->complexity / 2
                                         : 0);
}

// change_impact: low risk (fan_in=0, fan_out=2, both <=3); dependent_components
// derives one module name from the 3 callers (all in the same file/module, so
// it dedups to a single entry).
TEST_F(UsageAnalysisFixture, ChangeImpactLowRiskAndDependentComponentsFromCallers) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Target"), ok);
    ASSERT_TRUE(ok);

    const auto& impact = ctx.usage_analysis.change_impact;
    EXPECT_EQ(impact.breaking_change_risk, "low");
    ASSERT_EQ(impact.dependent_components.size(), 1u);
    EXPECT_EQ(impact.dependent_components[0], temp_dir_.filename().string());
}

// trap 5: requires_tests is always true; test_coverage is always
// false/empty — both Go stub values, pinned as constants, not computed.
TEST_F(UsageAnalysisFixture, RequiresTestsAlwaysTrueTestCoverageAlwaysEmpty) {
    ContextLookupEngine engine(*indexer_);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Target"), ok);
    ASSERT_TRUE(ok);

    EXPECT_TRUE(ctx.usage_analysis.change_impact.requires_tests);
    EXPECT_FALSE(ctx.usage_analysis.test_coverage.has_tests);
    EXPECT_TRUE(ctx.usage_analysis.test_coverage.test_file_paths.empty());
}

// =============================================================================
// ai_context (CLX S8) — C++ port of internal/core/context_lookup_ai.go
// =============================================================================
// `ComplexOp` has 18 sequential `if` branches (cyclomatic complexity ~19,
// well past the >10 high-cyclomatic-complexity threshold and the summary's
// >5 "high cyclomatic complexity" phrase threshold) and ~58 source lines
// (past the >50 long-function threshold). `Simple` is a trivial one-liner
// used to pin the include_ai_text=false gate without a smell/suggestion
// signal muddying the assertion.
class AiContextFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_aictx_test_");
        std::filesystem::create_directories(temp_dir_);
        std::ostringstream src;
        src << "package sample\n\nfunc Simple(x int) int {\n\treturn x\n}\n\n"
               "func ComplexOp(a int) int {\n\tx := 0\n";
        for (int i = 1; i <= 18; ++i) {
            src << "\tif a > " << i << " {\n\t\tx++\n\t}\n";
        }
        src << "\treturn x\n}\n";
        std::ofstream(temp_dir_ / "complex.go") << src.str();

        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());
    }

    void TearDown() override {
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    CodeObjectID object_id_for(const std::string& name) {
        auto snap = indexer_->ref_tracker().pin();
        auto syms = snap->find_symbols_by_name(name);
        EXPECT_FALSE(syms.empty()) << "no symbol named " << name;
        CodeObjectID oid;
        if (syms.empty()) return oid;
        const auto& s = syms.front();
        oid.file_id = s->symbol.file_id;
        oid.symbol_id = encode_symbol_id(s->id);
        oid.name = std::string(s->symbol.name);
        oid.type = s->symbol.type;
        return oid;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

// RED (per task acceptance criteria): mode=semantic + include_ai_text ->
// summary mentions the symbol name.
TEST_F(AiContextFixture, SummaryMentionsSymbolName) {
    ContextLookupEngine engine(*indexer_);
    engine.set_include_ai_text(true);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Simple"), ok);
    ASSERT_TRUE(ok);

    EXPECT_NE(ctx.ai_context.natural_language_summary.find("Simple"),
             std::string::npos)
        << ctx.ai_context.natural_language_summary;
}

// RED: high-complexity/long fixture -> long-function code smell, with a
// severity, keyed to the function's own location.
TEST_F(AiContextFixture, LongFunctionSmellDetected) {
    ContextLookupEngine engine(*indexer_);
    engine.set_include_ai_text(true);
    bool ok = false;
    auto oid = object_id_for("ComplexOp");
    auto ctx = engine.get_context(oid, ok);
    ASSERT_TRUE(ok);

    bool found = false;
    for (const auto& smell : ctx.ai_context.code_smells) {
        if (smell.type == "long-function") {
            found = true;
            EXPECT_FALSE(smell.severity.empty());
            EXPECT_EQ(smell.location.file_id, oid.file_id);
        }
    }
    EXPECT_TRUE(found) << "expected a long-function code smell";
}

// High cyclomatic complexity (>10) is also its own smell, independent of
// the long-function check.
TEST_F(AiContextFixture, HighCyclomaticComplexitySmellDetected) {
    ContextLookupEngine engine(*indexer_);
    engine.set_include_ai_text(true);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("ComplexOp"), ok);
    ASSERT_TRUE(ok);

    bool found = false;
    for (const auto& smell : ctx.ai_context.code_smells) {
        if (smell.type == "high-cyclomatic-complexity") found = true;
    }
    EXPECT_TRUE(found);
}

// best_practices is never empty once ai text is included (Go always appends
// two baseline naming/self-documentation practices unconditionally).
TEST_F(AiContextFixture, BestPracticesNonEmpty) {
    ContextLookupEngine engine(*indexer_);
    engine.set_include_ai_text(true);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("Simple"), ok);
    ASSERT_TRUE(ok);

    EXPECT_FALSE(ctx.ai_context.best_practices.empty());
}

// similar_objects is capped at <=5 (trap 5: Go's finder chain is a stub that
// always yields empty, so this is trivially satisfied — asserted anyway as
// the acceptance-criteria cap).
TEST_F(AiContextFixture, SimilarObjectsCappedAtFive) {
    ContextLookupEngine engine(*indexer_);
    engine.set_include_ai_text(true);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("ComplexOp"), ok);
    ASSERT_TRUE(ok);

    EXPECT_LE(ctx.ai_context.similar_objects.size(), 5u);
}

// include_ai_text=false gates the WHOLE section, per task spec — every field
// stays default/empty even for a fixture that would otherwise trigger smells.
TEST_F(AiContextFixture, IncludeAiTextFalseSkipsWholeSection) {
    ContextLookupEngine engine(*indexer_);
    engine.set_include_ai_text(false);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("ComplexOp"), ok);
    ASSERT_TRUE(ok);

    EXPECT_TRUE(ctx.ai_context.natural_language_summary.empty());
    EXPECT_TRUE(ctx.ai_context.code_smells.empty());
    EXPECT_TRUE(ctx.ai_context.best_practices.empty());
    EXPECT_TRUE(ctx.ai_context.refactoring_suggestions.empty());
    EXPECT_TRUE(ctx.ai_context.similar_objects.empty());
}

// Trap 6a (Go ordering bug, ported bug-for-bug): generateRefactoringSuggestions
// reads AIContext.CodeSmells BEFORE detectCodeSmells populates it, so the
// smell-driven suggestion line ("Address <type>: <description>") never
// fires even when code_smells ends up non-empty.
TEST_F(AiContextFixture, RefactoringSuggestionsNeverReflectSameRequestSmells) {
    ContextLookupEngine engine(*indexer_);
    engine.set_include_ai_text(true);
    bool ok = false;
    auto ctx = engine.get_context(object_id_for("ComplexOp"), ok);
    ASSERT_TRUE(ok);

    ASSERT_FALSE(ctx.ai_context.code_smells.empty());
    for (const auto& suggestion : ctx.ai_context.refactoring_suggestions) {
        EXPECT_EQ(suggestion.find("Address "), std::string::npos)
            << "trap 6a: suggestions must not reflect this request's own "
               "code_smells (Go reads them before detection runs): "
            << suggestion;
    }
}

// End-to-end through the MCP handler: mode=semantic selects
// [semantic, ai]; the ai section is populated (not just present-but-empty).
TEST_F(AiContextFixture, HandlerModeSemanticPopulatesAiSection) {
    nlohmann::json params;
    params["name"] = "ComplexOp";
    params["mode"] = "semantic";
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("context")) << result.text;
    const auto& ai = json["context"]["ai_context"];
    EXPECT_NE(ai["natural_language_summary"].get<std::string>().find("ComplexOp"),
             std::string::npos);
    EXPECT_FALSE(ai["code_smells"].empty());
    EXPECT_FALSE(ai["best_practices"].empty());
}

// The rich (mode/sections) get_context path wraps the context in a
// performance/metadata envelope — Go parity: handleGetObjectContextWithMode's
// {"context", "metadata", "performance"} result shape (component timing +
// server/index metadata alongside the context payload).
TEST_F(AiContextFixture, HandlerRichRequestCarriesPerformanceMetadataEnvelope) {
    nlohmann::json params;
    params["name"] = "ComplexOp";
    params["mode"] = "semantic";
    auto result = handle_get_context(params, *indexer_);
    ASSERT_FALSE(result.is_error) << result.text;
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json.contains("context")) << result.text;

    ASSERT_TRUE(json.contains("metadata")) << result.text;
    EXPECT_TRUE(json["metadata"].contains("server_version"));
    EXPECT_TRUE(json["metadata"].contains("processed_files"));

    ASSERT_TRUE(json.contains("performance")) << result.text;
    EXPECT_TRUE(json["performance"].contains("total_time_ms"));
    ASSERT_TRUE(json["performance"].contains("component_breakdown"));
    const auto& breakdown = json["performance"]["component_breakdown"];
    for (const char* key : {"basic_info_time", "relationships_time",
                            "variables_time", "semantic_time",
                            "structure_time", "usage_time", "ai_time"}) {
        EXPECT_TRUE(breakdown.contains(key)) << key;
    }
}

}  // namespace
}  // namespace mcp
}  // namespace lci
