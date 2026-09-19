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

// Manifest text is caller/source-derived and may hold non-UTF-8 bytes (0x8A
// latin-1). The strict dump(2) used to throw type_error.316 and fail the
// whole save; both the to_file and to_string paths must serialize lossily.
TEST_F(ContextHandlerFixture, SaveToleratesInvalidUtf8InRefNote) {
    std::string bad_note = "latin1 \x8A byte";
    nlohmann::json params = {
        {"operation", "save"},
        {"to_string", true},
        {"task", "utf8"},
        {"refs", {{{"f", "main.go"}, {"s", "main"}, {"note", bad_note}}}}};
    auto result = handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_FALSE(result.is_error) << result.text;

    nlohmann::json file_params = {
        {"operation", "save"},
        {"to_file", "utf8_manifest.json"},
        {"task", "utf8"},
        {"refs", {{{"f", "main.go"}, {"s", "main"}, {"note", bad_note}}}}};
    auto file_result =
        handle_context(file_params, *indexer_, temp_dir_.string());
    EXPECT_FALSE(file_result.is_error) << file_result.text;
    EXPECT_TRUE(
        std::filesystem::exists(temp_dir_ / "utf8_manifest.json"));
}

// resolve_manifest_path must confine manifests to the project root: `..`
// traversal and out-of-root absolute paths are errors, never filesystem
// writes/reads outside the root.
TEST_F(ContextHandlerFixture, SaveRejectsPathEscapingProjectRoot) {
    for (const std::string& escape :
         {std::string("../escape.json"),
          std::string("sub/../../escape.json"),
          std::string("/tmp/lci_context_escape.json")}) {
        nlohmann::json params = {
            {"operation", "save"},
            {"to_file", escape},
            {"refs", {{{"f", "main.go"}, {"s", "main"}}}}};
        auto result =
            handle_context(params, *indexer_, temp_dir_.string());
        EXPECT_TRUE(result.is_error) << escape << ": " << result.text;
    }
    EXPECT_FALSE(std::filesystem::exists(temp_dir_.parent_path() /
                                         "escape.json"));
}

TEST_F(ContextHandlerFixture, LoadRejectsPathEscapingProjectRoot) {
    nlohmann::json params = {{"operation", "load"},
                             {"from_file", "../outside_manifest.json"}};
    auto result = handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_TRUE(result.is_error);
    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j.contains("error"));
}

// An absolute path INSIDE the root stays accepted (pre-confinement behavior
// that legitimate callers rely on).
TEST_F(ContextHandlerFixture, SaveAcceptsAbsolutePathInsideRoot) {
    auto abs = (temp_dir_ / "abs_manifest.json").string();
    nlohmann::json params = {
        {"operation", "save"},
        {"to_file", abs},
        {"refs", {{{"f", "main.go"}, {"s", "main"}}}}};
    auto result = handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_FALSE(result.is_error) << result.text;
    EXPECT_TRUE(std::filesystem::exists(abs));
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

// append=true used to swallow every load_manifest_from_file error (bad JSON,
// failed validation) and then OVERWRITE the manifest with just the new refs —
// a silent destroy-on-corrupt. A corrupt existing manifest must fail the call
// and leave the file byte-identical.
TEST_F(ContextHandlerFixture, AppendToCorruptManifestErrorsAndLeavesFile) {
    const std::string corrupt = "{ this is not json !!!";
    write_file(temp_dir_ / "corrupt.json", corrupt);

    nlohmann::json params = {
        {"operation", "save"},
        {"to_file", "corrupt.json"},
        {"append", true},
        {"refs", {{{"f", "main.go"}, {"s", "main"}}}}};
    auto r = handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_TRUE(r.is_error) << r.text;

    std::ifstream in(temp_dir_ / "corrupt.json", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, corrupt) << "append must not overwrite a corrupt file";
}

// apply_expansions used to hydrate callers/callees and then discard them
// ((void)expanded), charging tokens for content that never reached the
// response. A 'callers' directive must surface the caller's source in the
// hydrated refs: main() calls handleRequest(), so expanding handleRequest
// must emit main.
TEST_F(ContextHandlerFixture, ExpansionCallersEmitContent) {
    nlohmann::json manifest = {
        {"refs",
         {{{"f", "handler.go"},
           {"s", "handleRequest"},
           {"x", nlohmann::json::array({"callers"})}}}}};
    nlohmann::json params = {{"operation", "load"},
                             {"from_string", manifest.dump()}};
    auto result = handle_context(params, *indexer_, temp_dir_.string());
    ASSERT_FALSE(result.is_error) << result.text;

    auto j = nlohmann::json::parse(result.text);
    bool found_caller = false;
    for (const auto& r : j["refs"]) {
        if (r.value("symbol", "") == "main") found_caller = true;
    }
    EXPECT_TRUE(found_caller) << "callers expansion must emit content: "
                              << j.dump();
}

// The budget was checked only before hydrating the NEXT ref, so the last
// admitted ref could overshoot arbitrarily and max_tokens - total_tokens
// went negative into apply_expansions. With max_tokens=1 the first ref is
// admitted (overshoot bounded by that one ref), the second is truncated,
// and expansions receive a zero — never negative — budget, emitting nothing.
TEST_F(ContextHandlerFixture, TokenBudgetTruncatesAndNeverGoesNegative) {
    nlohmann::json manifest = {
        {"refs",
         {{{"f", "main.go"},
           {"s", "main"},
           {"x", nlohmann::json::array({"callers"})}},
          {{"f", "handler.go"}, {"s", "handleRequest"}}}}};
    nlohmann::json params = {{"operation", "load"},
                             {"from_string", manifest.dump()},
                             {"max_tokens", 1}};
    auto result = handle_context(params, *indexer_, temp_dir_.string());
    ASSERT_FALSE(result.is_error) << result.text;

    auto j = nlohmann::json::parse(result.text);
    EXPECT_TRUE(j["stats"]["truncated"].get<bool>()) << j.dump();
    EXPECT_EQ(j["stats"]["refs_loaded"], 1) << j.dump();
    // Zero remaining budget: no expansion refs admitted.
    EXPECT_EQ(j["refs"].size(), 1u) << j.dump();
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

// Finding 3: format=signatures/outline were parsed but silently ignored
// (context_manifest_expander.cpp always returned the full body regardless
// of `format`). Implemented: signatures returns just the declaration line,
// outline lists the file's own symbols instead of any one symbol's body.
TEST_F(ContextHandlerFixture, HydrateReferenceSignaturesFormatReturnsOneLine) {
    ExpansionEngine engine(*indexer_);

    ContextRef ref;
    ref.file = "utils.go";
    ref.symbol = "parseInput";

    auto result = engine.hydrate_reference(ref, FormatType::Signatures,
                                           temp_dir_.string());
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.ref.source.find('\n'), std::string::npos)
        << "signatures format must not include the function body: "
        << result.ref.source;
    EXPECT_NE(result.ref.source.find("parseInput"), std::string::npos)
        << result.ref.source;
    EXPECT_EQ(result.ref.source.find("return"), std::string::npos)
        << "body statement leaked into signatures output: "
        << result.ref.source;
}

TEST_F(ContextHandlerFixture, HydrateReferenceOutlineFormatListsFileSymbols) {
    ExpansionEngine engine(*indexer_);

    ContextRef ref;
    ref.file = "handler.go";

    auto result = engine.hydrate_reference(ref, FormatType::Outline,
                                           temp_dir_.string());
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_NE(result.ref.source.find("handleRequest"), std::string::npos)
        << result.ref.source;
    // Outline is a listing, never the function's own body statement.
    EXPECT_EQ(result.ref.source.find("parseInput(\"hello\")"),
             std::string::npos)
        << result.ref.source;
}

// =============================================================================
// Identity resolution of saved references (task 01M2VE7HE4R66ZFB007ZHP1SYW)
//
// A saved file+symbol must resolve INSIDE that named file on every load. A
// stale line hint must never take precedence over the symbol identity, a
// same-name symbol in another file must never be substituted, and a
// same-file overload set must be reported as ambiguous rather than silently
// picking the first entry. Unresolved refs surface as structured entries
// carrying the original selector + role + a reason, while well-formed-but-
// unmatched input still keeps the successful refs.
// =============================================================================

class ContextResolutionFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_ctx_resolve_");
        std::filesystem::create_directories(temp_dir_);

        // Dup() is defined in two files with distinguishable bodies.
        write_file(temp_dir_ / "dup_a.go",
                   "package p\n"
                   "\n"
                   "func Dup() int { return 1 }\n");
        write_file(temp_dir_ / "dup_b.go",
                   "package p\n"
                   "\n"
                   "func Dup() int { return 2 }\n");
        // ghost.go is indexed but has NO Dup — a ref to ghost.go/Dup must
        // NOT borrow the other files' Dup source.
        write_file(temp_dir_ / "ghost.go",
                   "package p\n"
                   "\n"
                   "func GhostOnly() int { return 7 }\n");
        // Two same-file overloads: ambiguous by name.
        write_file(temp_dir_ / "over.cpp",
                   "int compute(int a) { return a; }\n"
                   "int compute(int a, int b) { return a + b; }\n");
        // Target() now lives on line 5; a stale hint at lines 1-2 would
        // return the unrelated package header.
        write_file(temp_dir_ / "shift.go",
                   "package p\n"
                   "\n"
                   "func Filler() int { return 0 }\n"
                   "\n"
                   "func Target() int { return 99 }\n");

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

    nlohmann::json load(nlohmann::json manifest) {
        nlohmann::json params = {{"operation", "load"},
                                 {"from_string", manifest.dump()}};
        auto result = handle_context(params, *indexer_, temp_dir_.string());
        if (result.is_error) {
            return nlohmann::json{{"__error__", result.text}};
        }
        return nlohmann::json::parse(result.text);
    }

    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

// A ref naming a file that does not contain the symbol must NOT substitute a
// same-name symbol from another file: it is unresolved with reason
// missing_symbol, and no other file's source leaks into the response.
TEST_F(ContextResolutionFixture, MissingSymbolInNamedFileIsNotSubstituted) {
    auto j = load({{"r", {{{"f", "ghost.go"}, {"s", "Dup"}, {"role", "contract"}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    EXPECT_EQ(j["refs"].size(), 0u)
        << "ghost.go has no Dup; the other files' Dup must not be borrowed: "
        << j.dump();
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["file"], "ghost.go");
    EXPECT_EQ(j["unresolved"][0]["symbol"], "Dup");
    EXPECT_EQ(j["unresolved"][0]["role"], "contract");
    EXPECT_EQ(j["unresolved"][0]["reason"], "missing_symbol");
    // The substituted bodies from dup_a/dup_b must not appear anywhere.
    auto dumped = j.dump();
    EXPECT_EQ(dumped.find("return 1"), std::string::npos);
    EXPECT_EQ(dumped.find("return 2"), std::string::npos);
}

// A ref to a file that is not in the index at all: missing_file.
TEST_F(ContextResolutionFixture, MissingFileIsReportedNotSubstituted) {
    auto j = load({{"r", {{{"f", "gone.go"}, {"s", "Dup"}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    EXPECT_EQ(j["refs"].size(), 0u) << j.dump();
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["file"], "gone.go");
    EXPECT_EQ(j["unresolved"][0]["symbol"], "Dup");
    EXPECT_EQ(j["unresolved"][0]["reason"], "missing_file");
    auto dumped = j.dump();
    EXPECT_EQ(dumped.find("return 1"), std::string::npos);
    EXPECT_EQ(dumped.find("return 2"), std::string::npos);
}

// A same-name overload set inside one file must be reported ambiguous; the
// engine must not silently return the first candidate.
TEST_F(ContextResolutionFixture, OverloadedSymbolIsAmbiguousNotFirstMatch) {
    auto j = load({{"r", {{{"f", "over.cpp"}, {"s", "compute"}, {"role", "pattern"}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    EXPECT_EQ(j["refs"].size(), 0u)
        << "compute is overloaded in over.cpp; picking either is a guess: "
        << j.dump();
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["reason"], "ambiguous_symbol");
    EXPECT_EQ(j["unresolved"][0]["file"], "over.cpp");
    EXPECT_EQ(j["unresolved"][0]["role"], "pattern");
}

// A symbol ref with a stale line hint resolves by name (identity), ignoring
// the hint; the returned source/lines are the symbol's current position.
TEST_F(ContextResolutionFixture, StaleLineHintIsIgnoredForSymbolRef) {
    auto j = load({{"r",
                    {{{"f", "shift.go"},
                      {"s", "Target"},
                      {"l", {{"s", 1}, {"e", 2}}}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    EXPECT_EQ(j["unresolved"].size(), 0u) << j.dump();
    ASSERT_EQ(j["refs"].size(), 1u) << j.dump();
    const auto& r = j["refs"][0];
    EXPECT_EQ(r["file"], "shift.go");
    EXPECT_NE(r["source"].get<std::string>().find("return 99"), std::string::npos)
        << "must resolve Target's current body, not the stale lines 1-2: "
        << r.dump();
    EXPECT_EQ(r["source"].get<std::string>().find("package"),
              std::string::npos)
        << "stale hint leaked the file header: " << r.dump();
    EXPECT_EQ(r["lines"]["start"], 5) << r.dump();
}

// A pure line-range ref (no symbol) keeps literal current-index line
// semantics and identifies that limitation, but still hydrates.
TEST_F(ContextResolutionFixture, PureLineRangeRefIsFlaggedLiteral) {
    auto j = load({{"r", {{{"f", "shift.go"}, {"l", {{"s", 3}, {"e", 3}}}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    EXPECT_EQ(j["unresolved"].size(), 0u) << j.dump();
    ASSERT_EQ(j["refs"].size(), 1u) << j.dump();
    EXPECT_TRUE(j["refs"][0].contains("line_range_literal")) << j.dump();
    EXPECT_EQ(j["refs"][0]["line_range_literal"], true) << j.dump();
}

// A well-formed manifest with an unsupported version fails explicitly.
TEST_F(ContextResolutionFixture, UnsupportedVersionFailsExplicitly) {
    auto j = load({{"v", "9.9"},
                   {"r", {{{"f", "dup_a.go"}, {"s", "Dup"}}}}});
    ASSERT_TRUE(j.contains("__error__"))
        << "v=9.9 must not be silently hydrated: " << j.dump();
    EXPECT_NE(j["__error__"].get<std::string>().find("version"),
              std::string::npos)
        << j["__error__"];
}

// v1.0 (and the default empty version) remain supported.
TEST_F(ContextResolutionFixture, VersionOneZeroIsSupported) {
    auto j = load({{"v", "1.0"}, {"r", {{{"f", "dup_a.go"}, {"s", "Dup"}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    EXPECT_EQ(j["unresolved"].size(), 0u) << j.dump();
    ASSERT_EQ(j["refs"].size(), 1u) << j.dump();
    EXPECT_EQ(j["refs"][0]["file"], "dup_a.go");
    EXPECT_NE(j["refs"][0]["source"].get<std::string>().find("return 1"),
              std::string::npos);
}

// Malformed top-level input fails explicitly (never an empty success).
TEST_F(ContextResolutionFixture, MalformedTopLevelFailsExplicitly) {
    nlohmann::json notjson = {{"operation", "load"}, {"from_string", "not json"}};
    auto r1 = handle_context(notjson, *indexer_, temp_dir_.string());
    EXPECT_TRUE(r1.is_error) << r1.text;

    nlohmann::json arr = {{"operation", "load"}, {"from_string", "[1,2,3]"}};
    auto r2 = handle_context(arr, *indexer_, temp_dir_.string());
    EXPECT_TRUE(r2.is_error) << r2.text;

    nlohmann::json norefs = {{"operation", "load"},
                             {"from_string", R"({"v":"1.0"})"}};
    auto r3 = handle_context(norefs, *indexer_, temp_dir_.string());
    EXPECT_TRUE(r3.is_error) << r3.text;
}

// One response can carry resolved refs and structured unresolved entries,
// with selectors and roles preserved for both sides.
TEST_F(ContextResolutionFixture, MixedResolvedAndUnresolvedRefs) {
    auto j = load({{"r",
                    {{{"f", "dup_a.go"}, {"s", "Dup"}, {"role", "primary"}},
                     {{"f", "dup_b.go"}, {"s", "Dup"}, {"role", "alt"}},
                     {{"f", "ghost.go"}, {"s", "Dup"}, {"role", "broken"}},
                     {{"f", "over.cpp"}, {"s", "compute"}, {"role", "amb"}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    // Both named-file Dups resolve to their own bodies.
    ASSERT_EQ(j["refs"].size(), 2u) << j.dump();
    EXPECT_EQ(j["refs"][0]["file"], "dup_a.go");
    EXPECT_NE(j["refs"][0]["source"].get<std::string>().find("return 1"),
              std::string::npos);
    EXPECT_EQ(j["refs"][1]["file"], "dup_b.go");
    EXPECT_NE(j["refs"][1]["source"].get<std::string>().find("return 2"),
              std::string::npos);
    // ghost/Dup -> missing_symbol, over.cpp/compute -> ambiguous_symbol.
    ASSERT_EQ(j["unresolved"].size(), 2u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["reason"], "missing_symbol");
    EXPECT_EQ(j["unresolved"][0]["role"], "broken");
    EXPECT_EQ(j["unresolved"][1]["reason"], "ambiguous_symbol");
    EXPECT_EQ(j["unresolved"][1]["role"], "amb");
    EXPECT_EQ(j["stats"]["unresolved_count"], 2) << j.dump();
    EXPECT_EQ(j["stats"]["refs_loaded"], 2) << j.dump();
}

// -- Resolver-level (ExpansionEngine) identity tests --------------------------

TEST_F(ContextResolutionFixture, ResolverRejectsCrossFileSubstitution) {
    ExpansionEngine engine(*indexer_);
    ContextRef ref;
    ref.file = "ghost.go";
    ref.symbol = "Dup";
    auto result =
        engine.hydrate_reference(ref, FormatType::Full, temp_dir_.string());
    EXPECT_FALSE(result.error.empty())
        << "ghost.go has no Dup; must not borrow another file's symbol";
    EXPECT_TRUE(result.ref.source.empty()) << result.ref.source;
}

TEST_F(ContextResolutionFixture, ResolverReportsSameFileAmbiguity) {
    ExpansionEngine engine(*indexer_);
    ContextRef ref;
    ref.file = "over.cpp";
    ref.symbol = "compute";
    auto result =
        engine.hydrate_reference(ref, FormatType::Full, temp_dir_.string());
    EXPECT_FALSE(result.error.empty())
        << "over.cpp/compute is overloaded; must not return the first match";
}

TEST_F(ContextResolutionFixture, ResolverIgnoresLineHintForSymbol) {
    ExpansionEngine engine(*indexer_);
    ContextRef ref;
    ref.file = "shift.go";
    ref.symbol = "Target";
    ref.line_range = {1, 2};
    ref.has_line_range = true;
    auto result =
        engine.hydrate_reference(ref, FormatType::Full, temp_dir_.string());
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.ref.lines.start, 5)
        << "stale hint {1,2} must be ignored; Target is at line 5";
    EXPECT_NE(result.ref.source.find("return 99"), std::string::npos);
}

// A ref with no selector at all is a per-ref invalid_ref unresolved entry;
// it must NOT fail the whole load (unlike malformed top-level input) and the
// other refs still hydrate.
TEST_F(ContextResolutionFixture, SelectorlessRefBecomesInvalidRefEntry) {
    auto j = load({{"r",
                    {{{"f", "dup_a.go"}, {"s", "Dup"}, {"role", "primary"}},
                     {{"role", "junk"}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    ASSERT_EQ(j["refs"].size(), 1u) << j.dump();
    EXPECT_EQ(j["refs"][0]["file"], "dup_a.go");
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["reason"], "invalid_ref");
    EXPECT_EQ(j["unresolved"][0]["role"], "junk");
}

}  // namespace
}  // namespace mcp
}  // namespace lci
