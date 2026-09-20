#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/context_manifest.h>
#include <lci/core/context_lookup.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/context_manifest_expander.h>
#include <lci/mcp/handlers_context.h>
#include <lci/mcp/server.h>

#include <nlohmann/json.hpp>

#include "unique_temp.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#ifndef _WIN32
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

// Appending an invalid ref must fail the whole save explicitly and leave the
// existing manifest byte-for-byte unchanged (the load path isolates invalid
// refs, but a save that persisted them would write an un-hydratable manifest).
// An invalid appended ref is any of: a verbose-only `{file, symbol}` ref, a
// bare `{}`, or a present-but-wrong-typed selector — each carries no honoured
// compact selector, so it is never a legitimate ref to store. A valid sibling
// in the same batch must not let the invalid one through, and none of the new
// refs may reach the file.
TEST_F(ContextHandlerFixture, AppendRejectsInvalidRefsAndLeavesFile) {
    const std::string manifest_name = "append_invalid.json";
    auto manifest_path = temp_dir_ / manifest_name;

    // Seed a valid one-ref manifest.
    nlohmann::json seed = {
        {"operation", "save"},
        {"to_file", manifest_name},
        {"task", "seed"},
        {"refs", {{{"f", "main.go"}, {"s", "main"}}}}};
    auto seeded = handle_context(seed, *indexer_, temp_dir_.string());
    ASSERT_FALSE(seeded.is_error) << seeded.text;

    std::ifstream before(manifest_path, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(before)),
                               std::istreambuf_iterator<char>());
    ASSERT_FALSE(original.empty());

    // A well-formed sibling that would merge cleanly if it stood alone.
    const nlohmann::json valid = {{"f", "handler.go"}, {"s", "handleRequest"}};
    const std::vector<nlohmann::json> invalid = {
        nlohmann::json{{"file", "main.go"}, {"symbol", "main"}},  // verbose-only
        nlohmann::json::object(),                                  // bare {}
        nlohmann::json{{"f", 123}, {"s", "main"}},                 // wrong-typed
        nlohmann::json{{"f", "main.go"},
                       {"l", {{"s", "not-an-int"}}}},              // bad bound
    };

    for (size_t i = 0; i < invalid.size(); ++i) {
        nlohmann::json refs = nlohmann::json::array();
        refs.push_back(valid);
        refs.push_back(invalid[i]);
        nlohmann::json attempt = {
            {"operation", "save"},
            {"to_file", manifest_name},
            {"append", true},
            {"refs", refs}};
        auto r = handle_context(attempt, *indexer_, temp_dir_.string());
        EXPECT_TRUE(r.is_error)
            << "appending invalid ref #" << i << " must fail: " << r.text;

        std::ifstream after(manifest_path, std::ios::binary);
        const std::string now((std::istreambuf_iterator<char>(after)),
                              std::istreambuf_iterator<char>());
        EXPECT_EQ(now, original)
            << "appending invalid ref #" << i
            << " must leave the existing file byte-for-byte unchanged: "
            << now;
    }

    // The valid sibling did not sneak in either: the manifest still holds the
    // single seed ref.
    nlohmann::json load = {{"operation", "load"},
                           {"from_file", manifest_name}};
    auto loaded = handle_context(load, *indexer_, temp_dir_.string());
    ASSERT_FALSE(loaded.is_error) << loaded.text;
    auto lj = nlohmann::json::parse(loaded.text);
    EXPECT_EQ(lj.value("stats", nlohmann::json::object())
                  .value("refs_loaded", -1),
              1)
        << "a failed append must not add any ref to the manifest: "
        << loaded.text;
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

// A well-formed manifest with an unsupported version is reported as structured
// unresolved entries (original selectors + roles preserved), never a bare
// prose error and never a silent hydration.
TEST_F(ContextResolutionFixture, UnsupportedVersionIsStructuredUnresolved) {
    auto j = load({{"v", "9.9"},
                   {"r", {{{"f", "dup_a.go"}, {"s", "Dup"}, {"role", "primary"}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    EXPECT_EQ(j["refs"].size(), 0u) << "v=9.9 must not hydrate: " << j.dump();
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["reason"], "unsupported_version");
    EXPECT_EQ(j["unresolved"][0]["file"], "dup_a.go");
    EXPECT_EQ(j["unresolved"][0]["role"], "primary");
    EXPECT_EQ(j["stats"]["unresolved_count"], 1) << j.dump();
}

// A top-level version of the wrong type (numeric) is malformed input and fails
// explicitly — it must never be read as "absent" and default to a supported
// version (which would silently hydrate under a misread schema).
TEST_F(ContextResolutionFixture, NumericVersionFailsExplicitly) {
    nlohmann::json params = {
        {"operation", "load"},
        {"from_string", R"({"v":99,"r":[{"f":"dup_a.go","s":"Dup"}]})"}};
    auto r = handle_context(params, *indexer_, temp_dir_.string());
    EXPECT_TRUE(r.is_error)
        << "a non-string version must fail, not default to 1.0: " << r.text;
}

// A per-ref selector of the wrong type (a line bound that is a string) is
// isolated as invalid_ref; the well-formed sibling ref still hydrates and the
// malformed selector + role survive in the unresolved entry.
TEST_F(ContextResolutionFixture, MalformedRefIsolatedNotWholeLoadAbort) {
    auto j = load({{"r",
                    {{{"f", "dup_a.go"}, {"s", "Dup"}, {"role", "primary"}},
                     {{"f", "ghost.go"},
                      {"s", "GhostOnly"},
                      {"l", {{"s", "bad"}, {"e", 2}}},
                      {"role", "broken"}}}}});
    ASSERT_FALSE(j.contains("__error__"))
        << "a bad selector must not abort the load: " << j.dump();
    EXPECT_EQ(j["refs"].size(), 1u) << j.dump();
    EXPECT_EQ(j["refs"][0]["file"], "dup_a.go");
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["reason"], "invalid_ref");
    EXPECT_EQ(j["unresolved"][0]["file"], "ghost.go");
    EXPECT_EQ(j["unresolved"][0]["symbol"], "GhostOnly");
    EXPECT_EQ(j["unresolved"][0]["role"], "broken");
}

// A wrong-typed file selector must NOT be silently dropped — dropping it would
// turn a file-scoped identity into a global symbol-only lookup (cross-file
// substitution). It is isolated as invalid_ref with the symbol retained.
TEST_F(ContextResolutionFixture, WrongTypedFileSelectorIsInvalidRefNotGlobal) {
    auto j = load({{"r", {{{"f", 123}, {"s", "Dup"}, {"role", "contract"}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    EXPECT_EQ(j["refs"].size(), 0u)
        << "a bad file type must not degrade to a global Dup match: "
        << j.dump();
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["reason"], "invalid_ref");
    EXPECT_EQ(j["unresolved"][0]["symbol"], "Dup");
    EXPECT_EQ(j["unresolved"][0]["role"], "contract");
}

// A ref whose only selector keys are verbose `file`/`symbol` carries no
// honoured selector (f/s are compact-only on the ref level). At the resolver it
// must be classified invalid_ref and must NOT be pushed into refs as an empty
// accepted ref, and its raw selector strings must survive in the report. This
// is the pre-f1d0996 rejection contract, now surfaced per-ref rather than as a
// top-level error.
TEST(ContextManifestParse, VerboseSelectorRefIsInvalidRefNotEmptyRef) {
    nlohmann::json j = {
        {"r", {{{"file", "dup_a.go"}, {"symbol", "Dup"}, {"role", "contract"}}}}};
    ContextManifest out;
    std::vector<UnresolvedRef> invalid;
    auto err = manifest_from_json(j, out, invalid);
    EXPECT_TRUE(err.empty()) << "per-ref problems must not abort the load";
    EXPECT_TRUE(out.refs.empty())
        << "a selector-less ref must never enter refs as an empty accepted ref";
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_EQ(invalid[0].reason, RefResolution::InvalidRef);
    EXPECT_EQ(invalid[0].file, "dup_a.go") << "raw verbose file selector preserved";
    EXPECT_EQ(invalid[0].symbol, "Dup") << "raw verbose symbol selector preserved";
    EXPECT_EQ(invalid[0].role, "contract");
}

// A bare `{}` ref likewise yields no honoured selector: it is invalid_ref, not
// an empty accepted ref. Asserted at the resolver because the load path's
// hydrate step masks an empty ref into an identically-shaped unresolved entry,
// so only a direct parse distinguishes "never entered refs" from "entered refs
// and was rejected during hydration".
TEST(ContextManifestParse, BareObjectRefIsInvalidRefNotEmptyRef) {
    nlohmann::json j = {
        {"r", nlohmann::json::array({nlohmann::json::object()})}};
    ContextManifest out;
    std::vector<UnresolvedRef> invalid;
    auto err = manifest_from_json(j, out, invalid);
    EXPECT_TRUE(err.empty()) << "a bad ref must not abort the load";
    EXPECT_TRUE(out.refs.empty())
        << "a bare {} ref must never enter refs as an empty accepted ref";
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_EQ(invalid[0].reason, RefResolution::InvalidRef);
}

// An `l` object that carries no honoured bound (a bare `{}` line range) yields
// no valid line range, so — like a selector-less ref — it must be classified
// invalid_ref at the resolver and must NOT enter refs as an empty accepted ref.
TEST(ContextManifestParse, EmptyLineRangeObjectRefIsInvalidRefNotEmptyRef) {
    nlohmann::json j = {
        {"r", nlohmann::json::array(
                  {{{"l", nlohmann::json::object()}}})}};
    ContextManifest out;
    std::vector<UnresolvedRef> invalid;
    auto err = manifest_from_json(j, out, invalid);
    EXPECT_TRUE(err.empty()) << "a bad ref must not abort the load";
    EXPECT_TRUE(out.refs.empty())
        << "a ref whose only selector is an empty {} line range must never "
           "enter refs as an accepted ref";
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_EQ(invalid[0].reason, RefResolution::InvalidRef);
}

// End-to-end through the load handler: a selector-less verbose ref never counts
// as a resolved ref, yet a well-formed sibling still hydrates and the load
// returns no top-level error. The verbose selectors survive in the unresolved
// entry (which the hydrate-mask path would have lost).
TEST_F(ContextResolutionFixture,
       VerboseSelectorRefIsInvalidRefAndIsolatesValidSibling) {
    auto j = load({{"r",
                    {{{"f", "dup_a.go"}, {"s", "Dup"}, {"role", "primary"}},
                     {{"file", "ghost.go"},
                      {"symbol", "GhostOnly"},
                      {"role", "contract"}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    ASSERT_EQ(j["refs"].size(), 1u) << j.dump();
    EXPECT_EQ(j["refs"][0]["file"], "dup_a.go");
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["reason"], "invalid_ref");
    EXPECT_EQ(j["unresolved"][0]["file"], "ghost.go");
    EXPECT_EQ(j["unresolved"][0]["symbol"], "GhostOnly");
    EXPECT_EQ(j["unresolved"][0]["role"], "contract");
    EXPECT_EQ(j["stats"]["refs_loaded"], 1) << j.dump();
}

// End-to-end (MCP dispatch): a ref whose only selector is an empty `{}` line
// range carries no honoured selector. The load path must report it as an
// invalid_ref unresolved entry, NOT let it enter refs and be masked by hydrate
// into a misleading `missing_file` (the shape seen when has_line_range was set
// from a bound-less `l` object).
TEST_F(ContextResolutionFixture, EmptyLineRangeObjectIsInvalidRefNotMissingFile) {
    auto j = load({{"r", {{{"l", nlohmann::json::object()}}}}});
    ASSERT_FALSE(j.contains("__error__")) << j.dump();
    EXPECT_EQ(j["refs"].size(), 0u)
        << "an empty {} line range must never be accepted as a resolved ref: "
        << j.dump();
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["reason"], "invalid_ref")
        << "no honoured selector is invalid_ref, not the hydration-masking "
           "missing_file: "
        << j.dump();
    EXPECT_EQ(j["stats"]["refs_loaded"], 0) << j.dump();
}

// A pure line-range ref (file + line range, no symbol) keeps literal
// current-index line semantics in EVERY format. format=outline must not widen
// it into a whole-file listing that drops the saved range and the
// line_range_literal marker; it slices the named lines and flags them literal.
TEST_F(ContextResolutionFixture, OutlineHonoursLiteralLineRangeForPureLineRef) {
    nlohmann::json manifest = {
        {"r", {{{"f", "shift.go"}, {"l", {{"s", 3}, {"e", 3}}}}}}};
    nlohmann::json params = {{"operation", "load"},
                             {"format", "outline"},
                             {"from_string", manifest.dump()}};
    auto result = handle_context(params, *indexer_, temp_dir_.string());
    ASSERT_FALSE(result.is_error) << result.text;
    auto j = nlohmann::json::parse(result.text);
    ASSERT_EQ(j["refs"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"].size(), 0u) << j.dump();
    const auto& r = j["refs"][0];
    EXPECT_TRUE(r.contains("line_range_literal"))
        << "a pure line-range ref must identify the literal-line limitation "
           "in outline format too: "
        << r.dump();
    EXPECT_EQ(r["line_range_literal"], true) << r.dump();
    EXPECT_EQ(r["lines"], (nlohmann::json{{"start", 3}, {"end", 3}}))
        << "the saved literal range must be preserved, not zeroed by a "
           "whole-file outline: "
        << r.dump();
    EXPECT_NE(r["source"].get<std::string>().find("Filler"), std::string::npos)
        << "must return the literal line 3 body, not the whole-file symbol "
           "listing: "
        << r.dump();
}

// -- format=outline must not bypass symbol identity resolution (B1) ----------

TEST_F(ContextResolutionFixture, OutlineIgnoresSymbolThatIsNotInFile) {
    ExpansionEngine engine(*indexer_);
    ContextRef ref;
    ref.file = "ghost.go";  // no Dup here; the other files have one
    ref.symbol = "Dup";
    auto result =
        engine.hydrate_reference(ref, FormatType::Outline, temp_dir_.string());
    EXPECT_NE(result.reason, RefResolution::Resolved)
        << "outline must not return a full-file listing labelled with a symbol "
           "that is not in this file: "
        << result.ref.source;
    EXPECT_TRUE(result.ref.source.empty()) << result.ref.source;
}

TEST_F(ContextResolutionFixture, OutlineMissingSymbolIsUnresolved) {
    nlohmann::json manifest = {
        {"r", {{{"f", "ghost.go"}, {"s", "DefinitelyAbsent"}}}}};
    nlohmann::json params = {{"operation", "load"},
                             {"format", "outline"},
                             {"from_string", manifest.dump()}};
    auto result = handle_context(params, *indexer_, temp_dir_.string());
    ASSERT_FALSE(result.is_error) << result.text;
    auto j = nlohmann::json::parse(result.text);
    EXPECT_EQ(j["refs"].size(), 0u) << j.dump();
    ASSERT_EQ(j["unresolved"].size(), 1u) << j.dump();
    EXPECT_EQ(j["unresolved"][0]["reason"], "missing_symbol") << j.dump();
}

// =============================================================================
// Expansion source identity (B5): the same file-scoped, never-substitute,
// never-guess rule as hydration must apply to expansion directives.
// =============================================================================

class ContextExpansionFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_ctx_expand_");
        std::filesystem::create_directories(temp_dir_);
        // An ambiguous type name in one file (two same-name interfaces): an
        // expansion directive must not pick one of them.
        write_file(temp_dir_ / "two_iface.go",
                   "package p\n"
                   "\n"
                   "type Widget interface{ A() }\n"
                   "\n"
                   "type Widget interface{ B() }\n");
        // A helper that exists in another file only: expanding it from this
        // file must never borrow the other file's definitions.
        write_file(temp_dir_ / "elsewhere.go",
                   "package p\n"
                   "\n"
                   "type Widget interface{ Z() }\n");
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
        std::ofstream out(path);
        out << content;
    }
    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
};

// An ambiguous same-file symbol with an expansion directive resolves to
// unresolved (ambiguous_symbol) — the identity gate runs before expansion, so
// no single candidate is silently chosen.
TEST_F(ContextExpansionFixture, AmbiguousExpansionSourceIsNeverAGuess) {
    ExpansionEngine engine(*indexer_);
    ContextRef ref;
    ref.file = "two_iface.go";
    ref.symbol = "Widget";
    ref.expansions = {"implementations"};
    auto result =
        engine.hydrate_reference(ref, FormatType::Full, temp_dir_.string());
    EXPECT_EQ(result.reason, RefResolution::AmbiguousSymbol)
        << "Widget is ambiguous in two_iface.go; hydration must report it, "
           "not pick one, so apply_expansions never sees a guessed source";
    auto applied = engine.apply_expansions(
        ref, result.ref, FormatType::Full, 100000, temp_dir_.string());
    EXPECT_TRUE(applied.expanded.empty())
        << "an ambiguous source must expand to nothing";
}

// -- Actual MCP server tools/call dispatch (B6) ------------------------------
//
// The prior context tests called handle_context directly. These drive the real
// JSON-RPC transport: a registered tool invoked through McpServer::dispatch_wire
// with a tools/call frame, so the wire error envelope and the compact-key
// contract are exercised end to end.

class ContextWireFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = lci::test::unique_temp_dir("lci_ctx_wire_");
        std::filesystem::create_directories(temp_dir_);
        write_file(temp_dir_ / "dup_a.go",
                   "package p\n\nfunc Dup() int { return 1 }\n");
        write_file(temp_dir_ / "dup_b.go",
                   "package p\n\nfunc Dup() int { return 2 }\n");
        write_file(temp_dir_ / "ghost.go",
                   "package p\n\nfunc GhostOnly() int { return 7 }\n");
        Config config;
        config.project.root = temp_dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        indexer_->index_directory(temp_dir_.string());
        server_ = std::make_unique<McpServer>(config, *indexer_, nullptr);
        register_context_handlers(*server_, indexer_.get());
    }
    void TearDown() override {
        server_.reset();
        indexer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }
    static void write_file(const std::filesystem::path& path,
                           const std::string& content) {
        std::ofstream out(path);
        out << content;
    }
    // Builds a real tools/call frame for the `context` tool and returns the
    // parsed {payload, isError} of the JSON-RPC response.
    struct WireResult {
        nlohmann::json payload;
        bool is_error{};
    };
    WireResult call(nlohmann::json arguments) {
        nlohmann::json frame = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params", {{"name", "context"}, {"arguments", arguments}}}};
        auto raw = server_->dispatch_wire(frame.dump());
        WireResult out;
        auto resp = nlohmann::json::parse(raw);
        out.is_error = resp.contains("result") &&
                       resp["result"].value("isError", false);
        if (out.is_error) {
            out.payload = nlohmann::json{
                "__error__",
                resp["result"]["content"][0]["text"].get<std::string>()};
        } else {
            out.payload = nlohmann::json::parse(
                resp["result"]["content"][0]["text"].get<std::string>());
        }
        return out;
    }
    WireResult load(nlohmann::json manifest) {
        return call({{"operation", "load"},
                     {"from_string", manifest.dump()}});
    }
    std::filesystem::path temp_dir_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<McpServer> server_;
};

TEST_F(ContextWireFixture, WireNoCrossFileSubstitution) {
    auto r = load({{"r", {{{"f", "ghost.go"}, {"s", "Dup"}, {"role", "contract"}}}}});
    ASSERT_FALSE(r.is_error);
    EXPECT_EQ(r.payload["refs"].size(), 0u);
    ASSERT_EQ(r.payload["unresolved"].size(), 1u) << r.payload.dump();
    EXPECT_EQ(r.payload["unresolved"][0]["reason"], "missing_symbol");
    auto dumped = r.payload.dump();
    EXPECT_EQ(dumped.find("return 1"), std::string::npos);
    EXPECT_EQ(dumped.find("return 2"), std::string::npos);
}

TEST_F(ContextWireFixture, WireOutlineAbsentSymbolIsUnresolved) {
    auto r = call({{"operation", "load"},
                   {"format", "outline"},
                   {"from_string",
                    nlohmann::json{{"r",
                                    {{{"f", "ghost.go"},
                                      {"s", "DefinitelyAbsent"}}}}}
                        .dump()}});
    ASSERT_FALSE(r.is_error);
    EXPECT_EQ(r.payload["refs"].size(), 0u)
        << "outline must not mask a missing symbol with a file listing: "
        << r.payload.dump();
    ASSERT_EQ(r.payload["unresolved"].size(), 1u);
    EXPECT_EQ(r.payload["unresolved"][0]["reason"], "missing_symbol");
}

TEST_F(ContextWireFixture, WireNumericVersionFailsOnWire) {
    auto r = call({{"operation", "load"},
                   {"from_string",
                    R"({"v":99,"r":[{"f":"dup_a.go","s":"Dup"}]})"}});
    EXPECT_TRUE(r.is_error)
        << "a non-string version must be a wire error, not a default: "
        << (r.payload.contains("__error__") ? r.payload["__error__"]
                                             : r.payload);
}

TEST_F(ContextWireFixture, WireMalformedRefIsolated) {
    auto r = load({{"r",
                    {{{"f", "dup_a.go"}, {"s", "Dup"}, {"role", "primary"}},
                     {{"f", "dup_b.go"},
                      {"l", {{"s", "oops"}, {"e", 3}}},
                      {"role", "bad"}}}}});
    ASSERT_FALSE(r.is_error)
        << "one malformed ref must not abort the whole tools/call: "
        << r.payload.dump();
    EXPECT_EQ(r.payload["refs"].size(), 1u) << r.payload.dump();
    ASSERT_EQ(r.payload["unresolved"].size(), 1u);
    EXPECT_EQ(r.payload["unresolved"][0]["reason"], "invalid_ref");
    EXPECT_EQ(r.payload["unresolved"][0]["role"], "bad");
}

TEST_F(ContextWireFixture, WireUnsupportedVersionStructured) {
    auto r = load({{"v", "2.5"},
                   {"r", {{{"f", "dup_a.go"}, {"s", "Dup"}, {"role", "p"}}}}});
    ASSERT_FALSE(r.is_error)
        << "unsupported_version is a structured result, not a wire error: "
        << r.payload.dump();
    EXPECT_EQ(r.payload["refs"].size(), 0u);
    ASSERT_EQ(r.payload["unresolved"].size(), 1u);
    EXPECT_EQ(r.payload["unresolved"][0]["reason"], "unsupported_version");
    EXPECT_EQ(r.payload["unresolved"][0]["role"], "p");
}

// Through the real tools/call wire, a save-append of a wrong-typed selector ref
// must return an isError envelope whose text is an explicit validation error,
// not a generic "Internal error" (which is what an uncaught nlohmann type_error
// produced), and the on-disk manifest must be untouched.
TEST_F(ContextWireFixture, WireAppendWrongTypedRefFailsExplicitlyAndLeavesFile) {
    auto seed = call({{"operation", "save"},
                      {"to_file", "wire_append.json"},
                      {"task", "seed"},
                      {"refs", {{{"f", "dup_a.go"}, {"s", "Dup"}}}}});
    ASSERT_FALSE(seed.is_error) << seed.payload.dump();

    std::ifstream before(temp_dir_ / "wire_append.json", std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(before)),
                               std::istreambuf_iterator<char>());
    ASSERT_FALSE(original.empty());

    auto bad = call({{"operation", "save"},
                     {"to_file", "wire_append.json"},
                     {"append", true},
                     {"refs", {{{"f", "dup_b.go"}, {"s", "Dup"}},
                               {{"f", 42}, {"s", "Dup"}}}}});
    ASSERT_TRUE(bad.is_error) << bad.payload.dump();
    // The call() helper packs an isError envelope as ["__error__", text].
    const std::string msg = bad.payload[1].get<std::string>();
    EXPECT_EQ(msg.find("Internal error"), std::string::npos)
        << "an uncaught type_error is an internal error, not an explicit "
           "validation failure: "
        << msg;
    EXPECT_TRUE(msg.find("ref[") != std::string::npos)
        << "expected a ref-indexed validation message: " << msg;

    std::ifstream after(temp_dir_ / "wire_append.json", std::ios::binary);
    const std::string now((std::istreambuf_iterator<char>(after)),
                          std::istreambuf_iterator<char>());
    EXPECT_EQ(now, original) << "failed append must not touch the file: " << now;
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

// =============================================================================
// get_context relationships/semantic reads (task 01M1NCXFB391TT0BCYWX6JYT44)
//
// The semantic section's entry-point dependency scan used to walk every
// file's symbols and BFS by NAME from every discovered entry point on EVERY
// get_context request (O(corpus) per call, hash-order dependent), and the
// relationships section mixed live-tracker call-graph ids with the pinned
// snapshot's ids. These three tests pin the fixed contract: cost independent
// of file count, order identical across fresh processes, and every id read
// from the caller-pinned snapshot.
// =============================================================================

namespace {

constexpr const char* kCoreGoFiles[] = {
    // main.go -> handler_core -> leaf_fn (the reachability chain).
    "package main\n\nfunc main() {\n\thandler_core()\n}\n",
    "package main\n\nfunc handler_core() {\n\tleaf_fn()\n}\n",
    "package main\n\nfunc leaf_fn() int {\n\treturn 1\n}\n"};

// Writes `filler_count` Go files with 15 plain (non-entry-named) functions
// each, plus the 3-file core call chain. The core symbol graph is identical
// for any filler_count, so get_context cost on leaf_fn must be flat in
// filler_count once the per-request corpus walk is gone.
void write_entry_corpus(const std::filesystem::path& dir, int filler_count) {
    std::filesystem::create_directories(dir);
    const char* names[] = {"main.go", "handler.go", "leaf.go"};
    for (int i = 0; i < 3; ++i) {
        std::ofstream f(dir / names[i]);
        f << kCoreGoFiles[i];
    }
    for (int fi = 0; fi < filler_count; ++fi) {
        std::ofstream f(dir / ("filler_" + std::to_string(fi) + ".go"));
        f << "package f" << fi << "\n";
        for (int fn = 0; fn < 15; ++fn) {
            f << "func fill_" << fi << "_" << fn << "() int { return " << fn
              << " }\n";
        }
    }
}

CodeObjectID oid_for(const ReferenceTracker::Snapshot& snap,
                     const std::string& name) {
    auto syms = snap.find_symbols_by_name(name);
    EXPECT_EQ(syms.size(), 1u) << name;
    CodeObjectID oid;
    oid.file_id = syms.front()->symbol.file_id;
    oid.name = name;
    oid.type = syms.front()->symbol.type;
    oid.symbol_id = encode_symbol_id(syms.front()->id);
    return oid;
}

// get_context on `leaf_fn` in a corpus of `filler_count` filler files.
double best_of_get_context_leaf(int filler_count, int reps) {
    auto dir = lci::test::unique_temp_dir("lci_ctx_scale_");
    write_entry_corpus(dir, filler_count);
    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    if (!indexer.index_directory(dir.string())) return -1.0;

    ContextLookupEngine engine(indexer);
    auto snap = indexer.ref_tracker().pin();
    if (snap == nullptr) return -1.0;
    auto oid = oid_for(*snap, "leaf_fn");
    bool ok = false;
    engine.get_context(oid, ok);  // warm-up (lazy paths)
    if (!ok) return -1.0;

    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        auto ctx = engine.get_context(oid, ok);
        auto t1 = std::chrono::steady_clock::now();
        EXPECT_TRUE(ok);
        EXPECT_GT(ctx.semantic_context.entry_point_dependencies.size(), 0u)
            << "fixture must produce entry-point dependencies";
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        best = std::min(best, ms);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return best;
}

}  // namespace

// 10x the files (same core symbol graph) must not cost 2x the get_context
// latency. Baseline fails: find_all_entry_points scanned every file per call.
TEST(ContextPerfTest, GetContextScalingRatioFlatInFileCount) {
    constexpr int kReps = 5;
    double small = best_of_get_context_leaf(40, kReps);
    double large = best_of_get_context_leaf(400, kReps);
    ASSERT_GT(small, 0.0);
    ASSERT_GT(large, 0.0);
    EXPECT_LT(large, small * 2.0)
        << "get_context on a 10x-file corpus (same symbol) cost "
        << large << "ms vs " << small
        << "ms: per-request cost still scales with corpus size";
}

// entry_point_dependencies order must be identical across 5 fresh processes
// (abseil salt is per-process: an in-process repetition cannot catch a
// hash-order dependency — every child execs the test binary with a fixture
// dir and a pipe fd in the environment, and the parent diffs the raw JSON).
#ifndef _WIN32

namespace {

// 20 files, each with one handler-named function calling `dispatch`: all 20
// reach the target with equal 0.8 confidence, so emission order is decided
// solely by the corpus-walk order of the (hash-ordered) file map.
void write_handler_corpus(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "dispatch.go");
        f << "package p\n\nfunc dispatch() int { return 1 }\n";
    }
    for (int i = 0; i < 20; ++i) {
        std::string n = std::to_string(i);
        if (i < 10) n = "0" + n;
        std::ofstream f(dir / ("handler_" + n + ".go"));
        f << "package p\n\nfunc handler_" + n +
             "() int { return dispatch() }\n";
    }
}

std::string run_get_context_child_body(const std::filesystem::path& dir) {
    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    if (!indexer.index_directory(dir.string())) return {};
    ContextLookupEngine engine(indexer);
    auto snap = indexer.ref_tracker().pin();
    if (!snap) return {};
    auto oid = oid_for(*snap, "dispatch");
    bool ok = false;
    auto ctx = engine.get_context(oid, ok);
    if (!ok) return {};
    return nlohmann::json(ctx_json_array(
               ctx.semantic_context.entry_point_dependencies)).dump();
}

struct GctxChildRunner {
    GctxChildRunner() {
        const char* dir = std::getenv("LCI_GCTX_CHILD_DIR");
        const char* fd = std::getenv("LCI_GCTX_CHILD_FD");
        if (dir == nullptr || fd == nullptr) return;
        int out_fd = std::atoi(fd);
        std::string text = run_get_context_child_body(dir);
        size_t off = 0;
        while (off < text.size()) {
            ssize_t n = write(out_fd, text.data() + off, text.size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        _exit(0);
    }
} g_gctx_child_runner;

std::string child_get_context_output(const std::filesystem::path& dir) {
    int fds[2];
    if (pipe(fds) != 0) return {};
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);  // fd number survives below via env
        char fdbuf[16];
        snprintf(fdbuf, sizeof(fdbuf), "%d", STDOUT_FILENO);
        // STDOUT_FILENO==1 only after dup2; use the fixed number.
        setenv("LCI_GCTX_CHILD_DIR", dir.string().c_str(), 1);
        setenv("LCI_GCTX_CHILD_FD", fdbuf, 1);
        char exe[4096];
        ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (len <= 0) _exit(1);
        exe[len] = '\0';
        char* argv[] = {exe, nullptr};
        execv(exe, argv);
        _exit(1);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return out;
}

}  // namespace

TEST(ContextDeterminismTest, EntryPointDependenciesOrderAcrossProcesses) {
    std::string reference;
    for (int run = 0; run < 5; ++run) {
        auto dir = lci::test::unique_temp_dir("lci_ctx_det_");
        write_handler_corpus(dir);
        std::string text = child_get_context_output(dir);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        ASSERT_FALSE(text.empty()) << "child " << run << " produced nothing";
        if (run == 0) {
            reference = text;
        } else {
            EXPECT_EQ(text, reference) << "process " << run << " diverged";
        }
    }
    auto j = nlohmann::json::parse(reference);
    ASSERT_EQ(j.size(), 20u) << j.dump();
    for (size_t i = 1; i < j.size(); ++i) {
        EXPECT_LE(j[i - 1]["entry_point_id"]["name"].get<std::string>(),
                  j[i]["entry_point_id"]["name"].get<std::string>())
            << "rows must be emitted in sorted order, not hash order";
    }
}

// All relationship ids come from the pinned snapshot: mutating the live
// tracker after pinning (dropping a caller's call edge via update_file) must
// not change the result of a second get_context on the same pin.
TEST(ContextPinnedSnapshotTest, RelationshipsReadPinnedSnapshotNotLiveTracker) {
    auto dir = lci::test::unique_temp_dir("lci_ctx_pin_");
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "a.go");
        f << "package p\n\nfunc helper() int { return 3 }\n\n"
             "func target() int {\n\treturn helper()\n}\n";
    }
    {
        std::ofstream f(dir / "b.go");
        f << "package p\n\nfunc caller() int {\n\treturn target()\n}\n";
    }
    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    ASSERT_TRUE(indexer.index_directory(dir.string()));

    ContextLookupEngine engine(indexer);
    auto snap = indexer.ref_tracker().pin();
    ASSERT_TRUE(snap != nullptr);
    auto oid = oid_for(*snap, "target");

    bool ok = false;
    auto r1 = engine.get_context(oid, ok, snap);
    ASSERT_TRUE(ok);
    ASSERT_FALSE(r1.direct_relationships.caller_functions.empty())
        << "fixture needs a resolved caller of target";
    ASSERT_FALSE(r1.direct_relationships.called_functions.empty());

    // Mutate the LIVE tracker: rewrite b.go so its call edge to target is
    // gone. The pinned snapshot must keep answering as if it had never moved.
    ASSERT_TRUE(indexer.update_file(
        (dir / "b.go").string(),
        "package p\n\nfunc caller() int {\n\treturn 1\n}\n"));

    auto r2 = engine.get_context(oid, ok, snap);
    ASSERT_TRUE(ok);
    EXPECT_EQ(r1.direct_relationships.to_json().dump(),
              r2.direct_relationships.to_json().dump())
        << "relationships read the live tracker, not the pinned snapshot";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#endif  // !_WIN32

}  // namespace
}  // namespace mcp
}  // namespace lci
