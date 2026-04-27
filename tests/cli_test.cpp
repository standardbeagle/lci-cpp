#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include <lci/cli/commands.h>

namespace lci {
namespace cli {
namespace {

// -- load_config_with_overrides tests -----------------------------------------

TEST(CliConfigTest, DefaultFlagsProduceValidConfig) {
    GlobalFlags flags;
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_FALSE(cfg.project.root.empty());
}

TEST(CliConfigTest, RootOverrideApplied) {
    GlobalFlags flags;
    flags.root = "/tmp";
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(cfg.project.root, "/tmp");
}

TEST(CliConfigTest, IncludeOverrideReplacesConfig) {
    GlobalFlags flags;
    flags.include = {"*.go", "*.rs"};
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(cfg.include.size(), 2u);
    EXPECT_EQ(cfg.include[0], "*.go");
    EXPECT_EQ(cfg.include[1], "*.rs");
}

TEST(CliConfigTest, ExcludeOverrideAppendsToConfig) {
    GlobalFlags flags;
    flags.exclude = {"vendor/**"};
    Config cfg;
    std::string err = load_config_with_overrides(flags, cfg);
    EXPECT_TRUE(err.empty()) << err;
    bool found = false;
    for (auto& e : cfg.exclude) {
        if (e == "vendor/**") found = true;
    }
    EXPECT_TRUE(found);
}

// -- format helpers -----------------------------------------------------------

TEST(CliFormatTest, FormatBytesSmall) {
    EXPECT_EQ(format_bytes(500), "500 bytes");
}

TEST(CliFormatTest, FormatBytesKB) {
    std::string result = format_bytes(2048);
    EXPECT_NE(result.find("KB"), std::string::npos);
}

TEST(CliFormatTest, FormatBytesMB) {
    std::string result = format_bytes(5 * 1024 * 1024);
    EXPECT_NE(result.find("MB"), std::string::npos);
}

TEST(CliFormatTest, FormatBytesGB) {
    std::string result = format_bytes(int64_t{2} * 1024 * 1024 * 1024);
    EXPECT_NE(result.find("GB"), std::string::npos);
}

TEST(CliFormatTest, FormatMillisecondsSmall) {
    EXPECT_EQ(format_milliseconds(42), "42 ms");
}

TEST(CliFormatTest, FormatMillisecondsSeconds) {
    std::string result = format_milliseconds(5500);
    EXPECT_NE(result.find("seconds"), std::string::npos);
}

TEST(CliFormatTest, FormatMillisecondsMinutes) {
    std::string result = format_milliseconds(120000);
    EXPECT_NE(result.find("minutes"), std::string::npos);
}

TEST(CliFormatTest, FormatSecondsSmall) {
    std::string result = format_seconds(45.0);
    EXPECT_NE(result.find("seconds"), std::string::npos);
}

TEST(CliFormatTest, FormatSecondsMinutes) {
    std::string result = format_seconds(300.0);
    EXPECT_NE(result.find("minutes"), std::string::npos);
}

TEST(CliFormatTest, FormatSecondsHours) {
    std::string result = format_seconds(7200.0);
    EXPECT_NE(result.find("hours"), std::string::npos);
}

TEST(CliFormatTest, FormatSecondsDays) {
    std::string result = format_seconds(200000.0);
    EXPECT_NE(result.find("days"), std::string::npos);
}

// -- MCP auto-detection -------------------------------------------------------

TEST(CliMcpDetectTest, DefaultReturnsFalseInTerminal) {
    // In a test runner, stdin is typically a terminal or redirected.
    // We just verify the function doesn't crash.
    // When connected to a terminal, it should return false.
    // When piped (as in CI), it may return true - both are valid.
    (void)is_mcp_mode();
}

TEST(CliMcpDetectTest, EnvVariableOverride) {
    // Save and restore env
    const char* old = std::getenv("LCI_MCP_MODE");
    setenv("LCI_MCP_MODE", "1", 1);
    EXPECT_TRUE(is_mcp_mode());
    if (old) {
        setenv("LCI_MCP_MODE", old, 1);
    } else {
        unsetenv("LCI_MCP_MODE");
    }
}

// -- config init command tests (no server needed) -----------------------------

TEST(CliConfigInitTest, KdlFormatCreatesFile) {
    std::string test_file = "/tmp/lci_test_config_init.kdl";
    std::remove(test_file.c_str());

    GlobalFlags flags;
    int rc = run_config_init(flags, "kdl", test_file, false, false);
    EXPECT_EQ(rc, 0);

    std::ifstream ifs(test_file);
    ASSERT_TRUE(ifs.good());
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("project"), std::string::npos);
    EXPECT_NE(content.find("index"), std::string::npos);
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, MinimalKdlCreatesFile) {
    std::string test_file = "/tmp/lci_test_config_init_min.kdl";
    std::remove(test_file.c_str());

    GlobalFlags flags;
    int rc = run_config_init(flags, "kdl", test_file, false, true);
    EXPECT_EQ(rc, 0);

    std::ifstream ifs(test_file);
    ASSERT_TRUE(ifs.good());
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("index"), std::string::npos);
    // Minimal should not have a project { } block
    EXPECT_EQ(content.find("project {"), std::string::npos);
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, JsonFormatCreatesValidJson) {
    std::string test_file = "/tmp/lci_test_config_init.json";
    std::remove(test_file.c_str());

    GlobalFlags flags;
    int rc = run_config_init(flags, "json", test_file, false, false);
    EXPECT_EQ(rc, 0);

    std::ifstream ifs(test_file);
    ASSERT_TRUE(ifs.good());
    nlohmann::json j;
    EXPECT_NO_THROW(ifs >> j);
    EXPECT_TRUE(j.contains("project"));
    EXPECT_TRUE(j.contains("index"));
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, YamlFormatCreatesFile) {
    std::string test_file = "/tmp/lci_test_config_init.yaml";
    std::remove(test_file.c_str());

    GlobalFlags flags;
    int rc = run_config_init(flags, "yaml", test_file, false, false);
    EXPECT_EQ(rc, 0);

    std::ifstream ifs(test_file);
    ASSERT_TRUE(ifs.good());
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("version: 1"), std::string::npos);
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, RefusesOverwriteWithoutForce) {
    std::string test_file = "/tmp/lci_test_config_exists.kdl";
    // Create file first
    {
        std::ofstream ofs(test_file);
        ofs << "existing";
    }

    GlobalFlags flags;
    int rc = run_config_init(flags, "kdl", test_file, false, false);
    EXPECT_EQ(rc, 1);

    // With force it should succeed
    rc = run_config_init(flags, "kdl", test_file, true, false);
    EXPECT_EQ(rc, 0);
    std::remove(test_file.c_str());
}

TEST(CliConfigInitTest, UnsupportedFormatFails) {
    GlobalFlags flags;
    int rc = run_config_init(flags, "xml", "/tmp/lci_test.xml", false, false);
    EXPECT_EQ(rc, 1);
}

// -- config validate command tests --------------------------------------------

TEST(CliConfigValidateTest, ValidConfigReturnsZero) {
    GlobalFlags flags;
    int rc = run_config_validate(flags);
    EXPECT_EQ(rc, 0);
}

// -- config show command tests ------------------------------------------------

TEST(CliConfigShowTest, TableFormatReturnsZero) {
    GlobalFlags flags;
    int rc = run_config_show(flags, "table");
    EXPECT_EQ(rc, 0);
}

TEST(CliConfigShowTest, JsonFormatReturnsZero) {
    GlobalFlags flags;
    int rc = run_config_show(flags, "json");
    EXPECT_EQ(rc, 0);
}

// -- git-analyze validation tests ---------------------------------------------

TEST(CliGitAnalyzeTest, InvalidScopeFails) {
    GlobalFlags flags;
    int rc = run_git_analyze(flags, "invalid", "", "", {}, 0.8, 20, false);
    EXPECT_EQ(rc, 1);
}

TEST(CliGitAnalyzeTest, RangeScopeRequiresBase) {
    GlobalFlags flags;
    int rc = run_git_analyze(flags, "range", "", "", {}, 0.8, 20, false);
    EXPECT_EQ(rc, 1);
}

}  // namespace
}  // namespace cli
}  // namespace lci
