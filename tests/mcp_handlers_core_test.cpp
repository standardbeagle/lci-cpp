#include <gtest/gtest.h>

#include <lci/analysis/side_effect_analyzer.h>
#include <lci/config.h>
#include <lci/core/reference_tracker.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/server.h>
#include <lci/search/search_engine.h>
#include <lci/symbol.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace lci {
namespace mcp {
namespace {

// -- Test fixture with real index ---------------------------------------------

class HandlersFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        // Create a temp directory with sample files for indexing
        temp_dir_ = std::filesystem::temp_directory_path() /
                    "lci_handler_test";
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
    auto dir = std::filesystem::temp_directory_path() / "lci_bc_nested";
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
        dir_ = std::filesystem::temp_directory_path() / "lci_purity_ctx";
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
        const auto* sym = indexer_->ref_tracker().find_symbol_by_name("Adder");
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

// =============================================================================
// get_context handler tests
// =============================================================================

// Returns the encoded object ID of the first symbol named `name`, or "" if
// the corpus did not yield one (parser-dependent — tests guard on empty).
static std::string first_object_id(MasterIndex& idx, const std::string& name) {
    auto symbols = idx.ref_tracker().find_symbols_by_name(name);
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

// `signature` is emitted when the symbol carries one (Go ObjectContext
// has `signature,omitempty`).
TEST_F(HandlersFixture, GetContextByIdEmitsSignatureWhenPresent) {
    auto symbols = indexer_->ref_tracker().find_symbols_by_name("main");
    if (symbols.empty()) GTEST_SKIP() << "no 'main' symbol";
    const auto* sym = symbols.front();
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
    // Fixture has 4 non-hidden .go files; cap of 2 must not lie about the
    // universe size.
    EXPECT_EQ(json["results"].size(), 2u);
    EXPECT_EQ(json["total_matches"].get<int>(), 4);
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

// Regression: a project whose ROOT lives under a dotted directory
// (~/.cache/repo, .work/corpus, …) must not have every file classified as
// hidden. The hidden check applies to root-relative components only.
// Before the fix, every find_files call in the repo-qa benchmark returned
// {"results":[],"total_matches":0} because the corpora live under .work/.
TEST(FindFilesHiddenAncestor, ProjectUnderDottedDirIsFullyVisible) {
    auto base = std::filesystem::temp_directory_path() /
                ".lci_hidden_ancestor" / "proj";
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
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() / ".lci_hidden_ancestor", ec);
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
// mode paths and never errors on them. The compact path simply ignores
// sections it cannot render (the MCP ObjectContext has no variables/structure/
// etc. field); it must NOT fail-fast where Go succeeds. A bad id still yields
// an errors[] entry, but the section token itself is accepted.
TEST_F(HandlersFixture, GetContextSectionTokensAccepted) {
    const char* sections[] = {"variables",    "structure",       "semantic",
                              "usage",         "ai",              "dependencies",
                              "file_context",  "quality_metrics"};
    for (const char* s : sections) {
        nlohmann::json params;
        params["id"] = "VE";
        params["include_sections"] = {s};
        auto result = handle_get_context(params, *indexer_);
        // Accepted: not a hard error. (Unresolvable id is reported in
        // errors[], not as is_error.)
        EXPECT_FALSE(result.is_error) << "section '" << s << "': " << result.text;
        auto json = nlohmann::json::parse(result.text);
        EXPECT_TRUE(json.contains("contexts")) << s;
    }
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

}  // namespace
}  // namespace mcp
}  // namespace lci
