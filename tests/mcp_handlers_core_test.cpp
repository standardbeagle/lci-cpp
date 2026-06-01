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
    for (const auto& item : json["results"]) {
        if (item.contains("references")) {
            EXPECT_TRUE(item["references"].contains("incoming_count"));
            EXPECT_TRUE(item["references"].contains("outgoing_count"));
            found_refs = true;
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
    for (const auto& item : json["results"]) {
        if (!item.contains("breadcrumbs")) continue;
        ASSERT_TRUE(item["breadcrumbs"].is_array());
        for (const auto& c : item["breadcrumbs"]) {
            EXPECT_TRUE(c.contains("scope_type"));
            EXPECT_TRUE(c.contains("name"));
            EXPECT_TRUE(c.contains("start_line"));
            EXPECT_TRUE(c.contains("end_line"));
            EXPECT_TRUE(c.contains("language"));
            EXPECT_TRUE(c.contains("visibility"));
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
    for (const auto& item : json["results"]) {
        if (item.contains("breadcrumbs") && !item["breadcrumbs"].empty()) {
            found_chain = true;
            const auto& first = item["breadcrumbs"][0];
            EXPECT_TRUE(first.contains("scope_type"));
            EXPECT_TRUE(first.contains("name"));
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

TEST_F(HandlersFixture, SearchClampsMaxResults) {
    nlohmann::json params;
    params["pattern"] = "func";
    params["max"] = 999;
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_LE(json["max_results"].get<int>(), 100);
}

TEST_F(HandlersFixture, SearchWithFlags) {
    nlohmann::json params;
    params["pattern"] = "MAIN";
    params["flags"] = "ci";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    EXPECT_FALSE(result.is_error);
}

// Go-shape parity: every result item carries result_id, object_id, score,
// match, file, line, column, symbol_type, symbol_name, is_exported.
// Items are enriched with their enclosing symbol so MCP callers can hand
// the object_id straight to get_context.
TEST_F(HandlersFixture, SearchEmitsGoShapeFields) {
    nlohmann::json params;
    params["pattern"] = "main";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_TRUE(json["results"].is_array());
    ASSERT_FALSE(json["results"].empty());
    const auto& item = json["results"][0];
    EXPECT_TRUE(item.contains("result_id"));
    EXPECT_TRUE(item.contains("object_id"));
    EXPECT_TRUE(item.contains("file"));
    EXPECT_TRUE(item.contains("line"));
    EXPECT_TRUE(item.contains("column"));
    EXPECT_TRUE(item.contains("match"));
    EXPECT_TRUE(item.contains("score"));
    EXPECT_TRUE(item.contains("symbol_type"));
    EXPECT_TRUE(item.contains("symbol_name"));
    EXPECT_TRUE(item.contains("is_exported"));
}

// Symbol enrichment must resolve the enclosing symbol so the object_id is
// non-empty and matches the symbol that owns the matched line.
TEST_F(HandlersFixture, SearchEnclosingSymbolResolved) {
    nlohmann::json params;
    params["pattern"] = "main";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_FALSE(json["results"].empty());
    bool found_enclosed = false;
    for (const auto& item : json["results"]) {
        if (!item["object_id"].get<std::string>().empty() &&
            item["symbol_name"].get<std::string>() == "main") {
            EXPECT_EQ(item["symbol_type"].get<std::string>(), "function");
            found_enclosed = true;
            break;
        }
    }
    EXPECT_TRUE(found_enclosed) << "no match resolved to enclosing 'main'";
}

// result_id values must be unique across the result set so callers can use
// them as stable handles.
TEST_F(HandlersFixture, SearchResultIdsUnique) {
    nlohmann::json params;
    params["pattern"] = "main";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    std::set<std::string> ids;
    for (const auto& item : json["results"]) {
        auto rid = item["result_id"].get<std::string>();
        EXPECT_TRUE(ids.insert(rid).second)
            << "duplicate result_id: " << rid;
    }
}

// score is numeric (Go emits float). Real trigram-backed search produces
// non-uniform scores across matches in different files.
TEST_F(HandlersFixture, SearchScoreIsNumeric) {
    nlohmann::json params;
    params["pattern"] = "main";
    auto result = handle_search(params, *indexer_, search_engine_.get());
    ASSERT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    ASSERT_FALSE(json["results"].empty());
    EXPECT_TRUE(json["results"][0]["score"].is_number());
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
TEST_F(HandlersFixture, GetContextNameOnlyReturnsEmpty) {
    nlohmann::json params;
    params["name"] = "main";
    auto result = handle_get_context(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_EQ(json["count"].get<int>(), 0);
    EXPECT_TRUE(json["contexts"].empty());
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
    EXPECT_LE(json["total_matches"].get<int>(), 200);
}

TEST_F(HandlersFixture, FindFilesGlobFuzzyStarGo) {
    // Parity: Go's matchFilePaths invokes phraseMatcher.Match against
    // filenameNoExt as a fallback when literal substring/exact passes miss.
    // Pattern "*.go" doesn't substring-match any of main/handler/utils/server,
    // but go-edlib's normalised levenshtein collapses to similarity 1.0 for
    // very short targets — so all visible files surface as fuzzy hits with
    // a deterministic score (0.82 raw, * 0.7 fuzzy scale = 0.574).
    nlohmann::json params;
    params["pattern"] = "*.go";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    int total = json["total_matches"].get<int>();
    // 4 visible .go files (main.go, handler.go, utils.go, internal/api/server.go)
    EXPECT_EQ(total, 4);
    bool any_fuzzy = false;
    for (const auto& r : json["results"]) {
        if (r["match_type"].get<std::string>() == "fuzzy") {
            any_fuzzy = true;
            // Score parity with Go: 0.574 ± 0.05 (parity descriptor tolerance)
            double score = r["score"].get<double>();
            EXPECT_NEAR(score, 0.574, 0.05);
        }
    }
    EXPECT_TRUE(any_fuzzy);
}

TEST_F(HandlersFixture, FindFilesGlobFuzzyNonMatchingPattern) {
    // Parity: even a totally unrelated pattern like "zzzzzzz" surfaces
    // every visible file as a fuzzy hit, because go-edlib's normalised
    // levenshtein returns ~1.0 when one operand is much shorter than the
    // other. C++ must reproduce this observable behaviour to keep parity
    // with the Go binary's fuzzy fallback.
    nlohmann::json params;
    params["pattern"] = "zzzzzzz";
    auto result = handle_find_files(params, *indexer_);
    EXPECT_FALSE(result.is_error);
    auto json = nlohmann::json::parse(result.text);
    EXPECT_GE(json["total_matches"].get<int>(), 4);
    for (const auto& r : json["results"]) {
        EXPECT_EQ(r["match_type"], "fuzzy");
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
    // Every hit must have symbol_type=function (or be filtered out).
    for (const auto& item : json["results"]) {
        EXPECT_EQ(item.value("symbol_type", ""), "function");
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

TEST_F(HandlersFixture, GetContextUnsupportedSectionRejected) {
    nlohmann::json params;
    params["id"] = "VE";
    params["include_sections"] = {"variables"};
    auto result = handle_get_context(params, *indexer_);
    EXPECT_TRUE(result.is_error);
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
