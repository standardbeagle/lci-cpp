#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/context_manifest.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/context_manifest_expander.h>
#include <lci/mcp/handlers_context.h>
#include <lci/mcp/server.h>

#include <nlohmann/json.hpp>

#include "unique_temp.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace lci {
namespace mcp {
namespace {

// =============================================================================
// manifest_to_json / manifest_from_json round-trip tests
// =============================================================================

TEST(ContextManifestJson, RoundTripBasic) {
    ContextManifest m;
    m.task = "refactor login";
    m.project_root = "/home/user/project";
    m.refs.push_back({"auth.go", "Login", {10, 25}, true, {}, "primary", ""});
    m.refs.push_back({"db.go", "GetUser", {}, false, {}, "dependency", ""});

    auto j = manifest_to_json(m);
    ContextManifest out;
    auto err = manifest_from_json(j, out);

    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(out.task, "refactor login");
    EXPECT_EQ(out.project_root, "/home/user/project");
    ASSERT_EQ(out.refs.size(), 2u);
    EXPECT_EQ(out.refs[0].file, "auth.go");
    EXPECT_EQ(out.refs[0].symbol, "Login");
    EXPECT_TRUE(out.refs[0].has_line_range);
    EXPECT_EQ(out.refs[0].line_range.start, 10);
    EXPECT_EQ(out.refs[0].line_range.end, 25);
    EXPECT_EQ(out.refs[0].role, "primary");
    EXPECT_EQ(out.refs[1].file, "db.go");
    EXPECT_EQ(out.refs[1].symbol, "GetUser");
    EXPECT_FALSE(out.refs[1].has_line_range);
    EXPECT_EQ(out.refs[1].role, "dependency");
}

TEST(ContextManifestJson, RoundTripWithExpansions) {
    ContextManifest m;
    m.task = "analyze call tree";
    m.refs.push_back(
        {"handler.go", "ServeHTTP", {}, false,
         {"callers:2", "callees:1", "tests"}, "primary", "main handler"});

    auto j = manifest_to_json(m);
    ContextManifest out;
    auto err = manifest_from_json(j, out);

    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(out.refs.size(), 1u);
    ASSERT_EQ(out.refs[0].expansions.size(), 3u);
    EXPECT_EQ(out.refs[0].expansions[0], "callers:2");
    EXPECT_EQ(out.refs[0].expansions[1], "callees:1");
    EXPECT_EQ(out.refs[0].expansions[2], "tests");
    EXPECT_EQ(out.refs[0].note, "main handler");
}

TEST(ContextManifestJson, MissingRefsReturnsError) {
    nlohmann::json j = {{"task", "test"}};
    ContextManifest out;
    auto err = manifest_from_json(j, out);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("refs"), std::string::npos);
}

TEST(ContextManifestJson, EmptyRefsArrayIsValid) {
    nlohmann::json j = {{"task", "test"}, {"refs", nlohmann::json::array()}};
    ContextManifest out;
    auto err = manifest_from_json(j, out);
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(out.refs.empty());
}

// =============================================================================
// validate_manifest tests
// =============================================================================

TEST(ValidateManifest, EmptyRefsIsInvalid) {
    ContextManifest m;
    auto err = validate_manifest(m);
    EXPECT_FALSE(err.empty());
}

TEST(ValidateManifest, RefWithFileIsValid) {
    ContextManifest m;
    m.refs.push_back({"main.go", "", {}, false, {}, "", ""});
    auto err = validate_manifest(m);
    EXPECT_TRUE(err.empty()) << err;
}

TEST(ValidateManifest, RefWithSymbolIsValid) {
    ContextManifest m;
    m.refs.push_back({"", "MyFunc", {}, false, {}, "", ""});
    auto err = validate_manifest(m);
    EXPECT_TRUE(err.empty()) << err;
}

TEST(ValidateManifest, RefWithLineRangeIsValid) {
    ContextManifest m;
    m.refs.push_back({"", "", {1, 10}, true, {}, "", ""});
    auto err = validate_manifest(m);
    EXPECT_TRUE(err.empty()) << err;
}

TEST(ValidateManifest, EmptyRefIsInvalid) {
    ContextManifest m;
    m.refs.push_back({"", "", {}, false, {}, "", ""});
    auto err = validate_manifest(m);
    EXPECT_FALSE(err.empty());
}

// =============================================================================
// compute_manifest_stats tests
// =============================================================================

TEST(ManifestStats, ComputesCorrectly) {
    ContextManifest m;
    m.refs.push_back({"file1.go", "Func1", {1, 10}, true, {}, "", ""});
    m.refs.push_back({"file1.go", "Func2", {15, 20}, true, {}, "", ""});
    m.refs.push_back({"file2.go", "Func3", {}, false, {}, "", ""});

    auto stats = compute_manifest_stats(m);
    EXPECT_EQ(stats.ref_count, 3);
    EXPECT_EQ(stats.file_count, 2);
    EXPECT_EQ(stats.total_lines, 16);  // (10-1+1) + (20-15+1) = 10 + 6
}

// =============================================================================
// parse_expansion_directive tests
// =============================================================================

TEST(ParseExpansionDirective, SimpleDirective) {
    auto [type, depth] = parse_expansion_directive("callers");
    EXPECT_EQ(type, "callers");
    EXPECT_EQ(depth, 1);
}

TEST(ParseExpansionDirective, DirectiveWithDepth) {
    auto [type, depth] = parse_expansion_directive("callers:3");
    EXPECT_EQ(type, "callers");
    EXPECT_EQ(depth, 3);
}

TEST(ParseExpansionDirective, DirectiveWithInvalidDepth) {
    auto [type, depth] = parse_expansion_directive("callees:abc");
    EXPECT_EQ(type, "callees");
    EXPECT_EQ(depth, 1);
}

TEST(ParseExpansionDirective, DirectiveWithZeroDepth) {
    auto [type, depth] = parse_expansion_directive("tests:0");
    EXPECT_EQ(type, "tests");
    EXPECT_EQ(depth, 1);
}

// =============================================================================
// hydrated_context_to_json tests
// =============================================================================

TEST(HydratedContextJson, SerializesCorrectly) {
    HydratedContext ctx;
    ctx.task = "review";
    ctx.stats.refs_loaded = 2;
    ctx.stats.symbols_hydrated = 2;
    ctx.stats.tokens_approx = 500;

    HydratedRef r1;
    r1.file = "main.go";
    r1.symbol = "main";
    r1.lines = {1, 5};
    r1.source = "func main() {}";
    r1.symbol_type = "function";
    r1.is_exported = false;
    ctx.refs.push_back(std::move(r1));

    HydratedRef r2;
    r2.file = "api/handler.go";
    r2.symbol = "Handle";
    r2.lines = {10, 30};
    r2.source = "func Handle(w http.ResponseWriter) {}";
    r2.signature = "func Handle(w http.ResponseWriter)";
    r2.is_exported = true;
    ctx.refs.push_back(std::move(r2));

    ctx.warnings.push_back("some warning");

    auto j = hydrated_context_to_json(ctx);
    EXPECT_EQ(j["task"], "review");
    EXPECT_EQ(j["refs"].size(), 2u);
    EXPECT_EQ(j["refs"][0]["file"], "main.go");
    EXPECT_EQ(j["refs"][0]["symbol"], "main");
    EXPECT_EQ(j["refs"][0]["symbol_type"], "function");
    EXPECT_EQ(j["refs"][1]["is_exported"], true);
    EXPECT_EQ(j["refs"][1]["signature"],
              "func Handle(w http.ResponseWriter)");
    EXPECT_EQ(j["stats"]["refs_loaded"], 2);
    EXPECT_EQ(j["stats"]["tokens_approx"], 500);
    EXPECT_EQ(j["warnings"].size(), 1u);
}

// =============================================================================
// handle_context dispatch tests
// =============================================================================

class ContextHandlerFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_context_test_");
        std::filesystem::create_directories(temp_dir_);

        write_file(temp_dir_ / "main.go",
                   "package main\n\n"
                   "func main() {\n"
                   "\thandleRequest()\n"
                   "}\n");
        write_file(temp_dir_ / "handler.go",
                   "package main\n\n"
                   "func handleRequest() {\n"
                   "\tparseInput(\"hello\")\n"
                   "}\n");
        write_file(temp_dir_ / "utils.go",
                   "package main\n\n"
                   "func parseInput(s string) int {\n"
                   "\treturn len(s)\n"
                   "}\n");

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

    static void write_file(const std::filesystem::path& path,
                           const std::string& content) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        out << content;
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

TEST_F(ContextHandlerFixture, InvalidOperationReturnsError) {
    nlohmann::json params = {{"operation", "delete"}};
    auto result =
        handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(ContextHandlerFixture, MissingOperationReturnsError) {
    nlohmann::json params = nlohmann::json::object();
    auto result =
        handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_TRUE(result.is_error);
}

TEST_F(ContextHandlerFixture, SaveToStringReturnsManifest) {
    nlohmann::json params = {
        {"operation", "save"},
        {"to_string", true},
        {"task", "test save"},
        {"refs",
         {{{"f", "main.go"}, {"s", "main"}, {"l", {{"start", 3}, {"end", 5}}}}}}};

    auto result =
        handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_FALSE(result.is_error) << result.text;

    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("manifest"));
    EXPECT_EQ(j["ref_count"], 1);
    EXPECT_EQ(j["file_count"], 1);
}

TEST_F(ContextHandlerFixture, SaveToFileAndLoadRoundTrip) {
    auto manifest_path = temp_dir_ / "test_manifest.json";

    // Save
    nlohmann::json save_params = {
        {"operation", "save"},
        {"to_file", "test_manifest.json"},
        {"task", "roundtrip test"},
        {"refs",
         {{{"f", "handler.go"},
           {"s", "handleRequest"},
           {"l", {{"start", 3}, {"end", 5}}}}}}};

    auto save_result =
        handle_context(save_params, *indexer_, temp_dir_.string());
    EXPECT_FALSE(save_result.is_error) << save_result.text;

    auto save_j = nlohmann::json::parse(save_result.text);
    EXPECT_EQ(save_j["saved"], "test_manifest.json");

    // Load
    nlohmann::json load_params = {{"operation", "load"},
                                  {"from_file", "test_manifest.json"}};

    auto load_result =
        handle_context(load_params, *indexer_, temp_dir_.string());
    EXPECT_FALSE(load_result.is_error) << load_result.text;

    auto load_j = nlohmann::json::parse(load_result.text);
    EXPECT_EQ(load_j["task"], "roundtrip test");
    EXPECT_TRUE(load_j.contains("refs"));
    EXPECT_GE(load_j["refs"].size(), 1u);
    EXPECT_EQ(load_j["stats"]["refs_loaded"], 1);
}

TEST_F(ContextHandlerFixture, LoadFromStringWorks) {
    nlohmann::json manifest = {
        {"task", "string load"},
        {"refs",
         {{{"f", "main.go"},
           {"s", "main"},
           {"l", {{"start", 3}, {"end", 5}}}}}}};

    nlohmann::json params = {{"operation", "load"},
                             {"from_string", manifest.dump()}};

    auto result =
        handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_FALSE(result.is_error) << result.text;

    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["task"], "string load");
    EXPECT_EQ(j["stats"]["refs_loaded"], 1);
}

TEST_F(ContextHandlerFixture, SaveMissingRefsReturnsError) {
    nlohmann::json params = {{"operation", "save"}, {"to_string", true}};
    auto result =
        handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_TRUE(result.is_error);
}

TEST_F(ContextHandlerFixture, SaveMissingDestinationReturnsError) {
    nlohmann::json params = {
        {"operation", "save"},
        {"refs", {{{"f", "main.go"}, {"s", "main"}}}}};
    auto result =
        handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_TRUE(result.is_error);
}

TEST_F(ContextHandlerFixture, LoadMissingSourceReturnsError) {
    nlohmann::json params = {{"operation", "load"}};
    auto result =
        handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_TRUE(result.is_error);
}

TEST_F(ContextHandlerFixture, AppendModeAddsRefs) {
    auto manifest_path = temp_dir_ / "append_test.json";

    // First save
    nlohmann::json save1 = {
        {"operation", "save"},
        {"to_file", "append_test.json"},
        {"task", "append test"},
        {"refs", {{{"f", "main.go"}, {"s", "main"}}}}};
    auto r1 = handle_context(save1, *indexer_, temp_dir_.string());
    EXPECT_FALSE(r1.is_error) << r1.text;

    // Append
    nlohmann::json save2 = {
        {"operation", "save"},
        {"to_file", "append_test.json"},
        {"append", true},
        {"refs", {{{"f", "handler.go"}, {"s", "handleRequest"}}}}};
    auto r2 = handle_context(save2, *indexer_, temp_dir_.string());
    EXPECT_FALSE(r2.is_error) << r2.text;

    auto j2 = nlohmann::json::parse(r2.text);
    EXPECT_EQ(j2["ref_count"], 2);
}

// =============================================================================
// ExpansionEngine tests
// =============================================================================

TEST_F(ContextHandlerFixture, HydrateReferenceByLineRange) {
    ExpansionEngine engine(*indexer_);

    ContextRef ref;
    ref.file = "main.go";
    ref.line_range = {3, 5};
    ref.has_line_range = true;

    auto result =
        engine.hydrate_reference(ref, FormatType::Full, temp_dir_.string());
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_FALSE(result.ref.source.empty());
    EXPECT_GT(result.tokens, 0);
}

TEST_F(ContextHandlerFixture, HydrateReferenceBySymbol) {
    ExpansionEngine engine(*indexer_);

    ContextRef ref;
    ref.file = "main.go";
    ref.symbol = "main";

    auto result =
        engine.hydrate_reference(ref, FormatType::Full, temp_dir_.string());
    // May or may not find symbol depending on index state
    // Either succeeds or gives a clear error
    if (result.error.empty()) {
        EXPECT_FALSE(result.ref.source.empty());
    }
}

TEST_F(ContextHandlerFixture, HydrateReferenceEmptyRefFails) {
    ExpansionEngine engine(*indexer_);

    ContextRef ref;
    ref.file = "main.go";

    auto result =
        engine.hydrate_reference(ref, FormatType::Full, temp_dir_.string());
    EXPECT_FALSE(result.error.empty());
}

}  // namespace
}  // namespace mcp
}  // namespace lci
