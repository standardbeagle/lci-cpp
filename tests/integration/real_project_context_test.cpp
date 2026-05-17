// Real-project context extraction integration tests
//
// Mirrors the Go reference's workflow_scenarios/get_context tests.
// These verify get_context, find_files, and context_manifest on real codebases.

#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "helpers/real_project_helpers.h"

namespace lci {
namespace {

namespace fs = std::filesystem;

#define SKIP_IF_NO_REAL_PROJECT(lang, name)                                 \
    do {                                                                    \
        auto _rp = testing::find_real_project((lang), (name));              \
        if (!_rp) {                                                         \
            GTEST_SKIP() << "Real project not found: " << (lang) << "/"    \
                         << (name)                                          \
                         << ". Run ./scripts/add-real-projects.sh";        \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// Test: get_context by symbol name on real projects
//
// SKIP_NAME_BASED_CONTEXT — name-based get_context routes through Go's
// ContextLookupEngine (handleGetObjectContextWithMode). The C++ port does
// not have an equivalent engine yet; the handler surfaces this divergence
// via an explicit error rather than silently returning empty contexts.
// Tests below stay in place to document the expected shape once
// ContextLookupEngine is ported. Track: AN8m7hohEdlM Personal/lci loop.
// ---------------------------------------------------------------------------

#define SKIP_NAME_BASED_CONTEXT                                              \
    GTEST_SKIP() << "name-based get_context unimplemented in C++ port; "     \
                    "ContextLookupEngine not yet wired. See "                \
                    "src/mcp/handlers_core.cpp::handle_get_context."

class RealProjectGetContextTest : public ::testing::Test {};

TEST_F(RealProjectGetContextTest, ChiServeHTTPContext) {
    SKIP_NAME_BASED_CONTEXT;
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["name"] = "ServeHTTP";
    params["include_call_hierarchy"] = true;
    auto result = ctx.get_context(params);

    ASSERT_FALSE(result.contains("error"))
        << "get_context error: " << result.value("error", "unknown");

    ASSERT_TRUE(result.contains("contexts"));
    auto& contexts = result["contexts"];
    EXPECT_GT(contexts.size(), 0u)
        << "Expected at least one context for ServeHTTP";

    if (!contexts.empty()) {
        auto& first = contexts[0];
        EXPECT_EQ(first["symbol_name"].get<std::string>(), "ServeHTTP");
        EXPECT_TRUE(first.contains("file_path"));
        EXPECT_TRUE(first.contains("line"));
    }
}

TEST_F(RealProjectGetContextTest, ChiServeHTTPWithCallHierarchy) {
    SKIP_NAME_BASED_CONTEXT;
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["name"] = "ServeHTTP";
    params["include_call_hierarchy"] = true;
    params["max_depth"] = 3;
    auto result = ctx.get_context(params);

    ASSERT_FALSE(result.contains("error"));
    ASSERT_TRUE(result.contains("contexts"));
    ASSERT_GT(result["contexts"].size(), 0u);

    auto& first = result["contexts"][0];
    EXPECT_TRUE(first.contains("callers"));
    EXPECT_TRUE(first.contains("callees"));
    EXPECT_TRUE(first.contains("call_tree"));
    EXPECT_TRUE(first["call_tree"].contains("children"));
}

TEST_F(RealProjectGetContextTest, ChiMiddlewareContext) {
    SKIP_NAME_BASED_CONTEXT;
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["name"] = "Middleware";
    params["include_call_hierarchy"] = true;
    auto result = ctx.get_context(params);

    ASSERT_FALSE(result.contains("error"));
    ASSERT_TRUE(result.contains("contexts"));
    EXPECT_GT(result["contexts"].size(), 0u);
}

TEST_F(RealProjectGetContextTest, FastapiDependsContext) {
    SKIP_NAME_BASED_CONTEXT;
    SKIP_IF_NO_REAL_PROJECT("python", "fastapi");
    auto path = *testing::find_real_project("python", "fastapi");

    auto ctx = testing::setup_real_project(path, "fastapi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["name"] = "Depends";
    params["include_call_hierarchy"] = true;
    auto result = ctx.get_context(params);

    ASSERT_FALSE(result.contains("error"));
    ASSERT_TRUE(result.contains("contexts"));
    EXPECT_GT(result["contexts"].size(), 0u);
}

TEST_F(RealProjectGetContextTest, UnknownSymbolReturnsError) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["name"] = "ThisSymbolDoesNotExist12345";
    auto result = ctx.get_context(params);

    EXPECT_TRUE(result.contains("error"));
}

// ---------------------------------------------------------------------------
// Test: find_files on real projects
// ---------------------------------------------------------------------------

class RealProjectFindFilesTest : public ::testing::Test {};

TEST_F(RealProjectFindFilesTest, ChiFindsRouterFiles) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["pattern"] = "router";
    auto result = ctx.find_files(params);

    ASSERT_FALSE(result.contains("error"))
        << "find_files error: " << result.value("error", "unknown");

    EXPECT_TRUE(result.contains("results"));
}

TEST_F(RealProjectFindFilesTest, FastapiFindsMainFiles) {
    SKIP_IF_NO_REAL_PROJECT("python", "fastapi");
    auto path = *testing::find_real_project("python", "fastapi");

    auto ctx = testing::setup_real_project(path, "fastapi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["pattern"] = "main";
    auto result = ctx.find_files(params);

    ASSERT_FALSE(result.contains("error"));
    EXPECT_TRUE(result.contains("results"));
}

TEST_F(RealProjectFindFilesTest, EmptyPatternReturnsError) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    nlohmann::json params;
    params["pattern"] = "";
    auto result = ctx.find_files(params);

    EXPECT_TRUE(result.contains("error"));
}

// ---------------------------------------------------------------------------
// Test: context_manifest on real projects
// ---------------------------------------------------------------------------

class RealProjectContextManifestTest : public ::testing::Test {};

TEST_F(RealProjectContextManifestTest, SaveAndLoadManifest) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto search_results = ctx.search("ServeHTTP", 1);
    ASSERT_FALSE(search_results.empty())
        << "Need at least one search result to build manifest";

    auto file_path = search_results[0].path;

    nlohmann::json save_params;
    save_params["operation"] = "save";
    save_params["task"] = "refactor router";
    nlohmann::json refs = nlohmann::json::array();
    nlohmann::json ref;
    ref["f"] = file_path;
    ref["s"] = "ServeHTTP";
    ref["role"] = "primary";
    refs.push_back(ref);
    save_params["refs"] = refs;
    save_params["to_string"] = true;

    auto save_result = ctx.context_manifest(save_params);
    ASSERT_FALSE(save_result.contains("error"))
        << "save error: " << save_result.value("error", "unknown");

    ASSERT_TRUE(save_result.contains("manifest"));
    std::string manifest_str = save_result["manifest"];

    nlohmann::json load_params;
    load_params["operation"] = "load";
    load_params["from_string"] = manifest_str;
    auto load_result = ctx.context_manifest(load_params);

    ASSERT_FALSE(load_result.contains("error"))
        << "load error: " << load_result.value("error", "unknown");

    EXPECT_EQ(load_result["task"].get<std::string>(), "refactor router");
    ASSERT_TRUE(load_result.contains("refs"));
    ASSERT_GE(load_result["refs"].size(), 1u);
}

TEST_F(RealProjectContextManifestTest, HydrateManifest) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto search_results = ctx.search("ServeHTTP", 1);
    ASSERT_FALSE(search_results.empty());

    auto file_path = search_results[0].path;

    nlohmann::json save_params;
    save_params["operation"] = "save";
    save_params["task"] = "review handler";
    nlohmann::json refs = nlohmann::json::array();
    nlohmann::json ref;
    ref["f"] = file_path;
    ref["s"] = "ServeHTTP";
    ref["role"] = "primary";
    refs.push_back(ref);
    save_params["refs"] = refs;
    save_params["to_string"] = true;

    auto save_result = ctx.context_manifest(save_params);
    ASSERT_FALSE(save_result.contains("error"));
    std::string manifest_str = save_result["manifest"];

    nlohmann::json hydrate_params;
    hydrate_params["operation"] = "load";
    hydrate_params["from_string"] = manifest_str;
    auto hydrate_result = ctx.context_manifest(hydrate_params);

    ASSERT_FALSE(hydrate_result.contains("error"))
        << "hydrate error: " << hydrate_result.value("error", "unknown");

    EXPECT_TRUE(hydrate_result.contains("refs"));
    EXPECT_TRUE(hydrate_result.contains("stats"));
}

// Round-trip via file with the compact `f`/`s` ref-key contract that the
// MCP wire honours (verified iter-13 by parity descriptor
// tests/parity/descriptors/mcp/context_manifest/save-compact-keys.parity.json
// and probe against the Go binary — both binaries reject verbose
// `{file, symbol}` and accept `{f, s}`).
TEST_F(RealProjectContextManifestTest, SaveAndLoadCompactKeysFileRoundTrip) {
    SKIP_IF_NO_REAL_PROJECT("go", "chi");
    auto path = *testing::find_real_project("go", "chi");

    auto ctx = testing::setup_real_project(path, "chi");
    ASSERT_TRUE(ctx.valid());

    auto search_results = ctx.search("ServeHTTP", 1);
    ASSERT_FALSE(search_results.empty());
    auto file_path = search_results[0].path;

    auto tmp_dir = fs::temp_directory_path() /
                   ("lci_iter13_manifest_" +
                    std::to_string(::getpid()));
    fs::create_directories(tmp_dir);
    auto manifest_rel = ".lci-manifest-iter13.json";
    auto manifest_abs = tmp_dir / manifest_rel;

    // Save via compact `f`/`s` keys to a file under the project root.
    // Use absolute path so resolve_manifest_path doesn't relocate.
    nlohmann::json save_params;
    save_params["operation"] = "save";
    save_params["task"] = "iter13-roundtrip";
    nlohmann::json refs = nlohmann::json::array();
    nlohmann::json ref;
    ref["f"] = file_path;
    ref["s"] = "ServeHTTP";
    ref["role"] = "primary";
    refs.push_back(ref);
    save_params["refs"] = refs;
    save_params["to_file"] = manifest_abs.string();

    auto save_result = ctx.context_manifest(save_params);
    ASSERT_FALSE(save_result.contains("error"))
        << "save error: " << save_result.value("error", "unknown");
    ASSERT_TRUE(fs::exists(manifest_abs))
        << "manifest file not written at " << manifest_abs;

    // Iter-14 (DART-2PPeRKfyrceR): manifest body on disk must use the Go-shape
    // compact top-level keys (t/c/v/p/r/s) per
    // internal/types/context_manifest_types.go json tags. Verbose keys
    // (task/version/project_root/refs) would silently drop cross-binary —
    // karpathy rule 6 (no silent fallback).
    {
        std::ifstream in(manifest_abs);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        auto disk = nlohmann::json::parse(body);
        EXPECT_TRUE(disk.contains("t")) << "missing Go-shape top-level 't' (task)";
        EXPECT_TRUE(disk.contains("c")) << "missing Go-shape top-level 'c' (created)";
        EXPECT_TRUE(disk.contains("v")) << "missing Go-shape top-level 'v' (version)";
        EXPECT_TRUE(disk.contains("r")) << "missing Go-shape top-level 'r' (refs)";
        EXPECT_TRUE(disk.contains("s")) << "missing Go-shape top-level 's' (stats)";
        EXPECT_TRUE(disk.contains("p"))
            << "missing Go-shape top-level 'p' (project_root) — must survive "
               "the save handler's manifest build";
        EXPECT_FALSE(disk.contains("task"))
            << "verbose top-level 'task' must not be emitted";
        EXPECT_FALSE(disk.contains("refs"))
            << "verbose top-level 'refs' must not be emitted";
        EXPECT_FALSE(disk.contains("project_root"))
            << "verbose top-level 'project_root' must not be emitted";
        ASSERT_TRUE(disk["r"].is_array());
        ASSERT_GE(disk["r"].size(), 1u);
        EXPECT_EQ(disk["r"][0]["f"].get<std::string>(), file_path);
        EXPECT_EQ(disk["r"][0]["s"].get<std::string>(), "ServeHTTP");
        // Stats block carries rc per Go ManifestStats `rc` tag.
        EXPECT_TRUE(disk["s"].contains("rc"));
    }

    // Load it back — should resolve the same refs without losing `f` or `s`.
    nlohmann::json load_params;
    load_params["operation"] = "load";
    load_params["from_file"] = manifest_abs.string();
    auto load_result = ctx.context_manifest(load_params);

    ASSERT_FALSE(load_result.contains("error"))
        << "load error: " << load_result.value("error", "unknown");
    EXPECT_EQ(load_result["task"].get<std::string>(), "iter13-roundtrip");
    ASSERT_TRUE(load_result.contains("refs"));
    ASSERT_GE(load_result["refs"].size(), 1u);
    EXPECT_EQ(load_result["refs"][0]["symbol"].get<std::string>(),
              "ServeHTTP");

    // Negative case: a manifest written with verbose `{file, symbol}` ref
    // keys must be rejected when loaded — surfacing the contract per
    // karpathy rule 6 (no silent fallback to a forgiving alias).
    auto bad_manifest_abs = tmp_dir / ".lci-manifest-iter13-verbose.json";
    nlohmann::json bad;
    bad["t"] = "verbose";
    nlohmann::json bad_refs = nlohmann::json::array();
    nlohmann::json bad_ref;
    bad_ref["file"] = file_path;    // wrong key — should not be honoured
    bad_ref["symbol"] = "ServeHTTP"; // wrong key — should not be honoured
    bad_refs.push_back(bad_ref);
    bad["refs"] = bad_refs;
    {
        std::ofstream out(bad_manifest_abs);
        out << bad.dump(2);
    }

    nlohmann::json bad_load;
    bad_load["operation"] = "load";
    bad_load["from_file"] = bad_manifest_abs.string();
    auto bad_load_result = ctx.context_manifest(bad_load);
    EXPECT_TRUE(bad_load_result.contains("error"))
        << "verbose `{file, symbol}` ref keys must be rejected — the "
           "contract is compact `{f, s}`. Silent acceptance would let "
           "agents write the wrong shape and lose data on round-trip.";

    fs::remove_all(tmp_dir);
}

}  // namespace
}  // namespace lci
