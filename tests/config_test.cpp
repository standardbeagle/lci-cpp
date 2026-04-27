#include <gtest/gtest.h>

#include <lci/config.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace lci {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Default config
// ---------------------------------------------------------------------------
TEST(DefaultConfigTest, HasExpectedDefaults) {
    auto cfg = make_default_config();

    EXPECT_EQ(cfg.version, 1);
    EXPECT_FALSE(cfg.project.root.empty());

    EXPECT_EQ(cfg.index.max_file_size, 10 * 1024 * 1024);
    EXPECT_EQ(cfg.index.max_total_size_mb, 500);
    EXPECT_EQ(cfg.index.max_file_count, 10000);
    EXPECT_FALSE(cfg.index.follow_symlinks);
    EXPECT_TRUE(cfg.index.smart_size_control);
    EXPECT_EQ(cfg.index.priority_mode, "recent");
    EXPECT_TRUE(cfg.index.respect_gitignore);
    EXPECT_TRUE(cfg.index.watch_mode);
    EXPECT_EQ(cfg.index.watch_debounce_ms, 300);

    EXPECT_EQ(cfg.performance.max_memory_mb, 500);
    EXPECT_EQ(cfg.performance.debounce_ms, 100);
    EXPECT_EQ(cfg.performance.indexing_timeout_sec, 120);
    EXPECT_EQ(cfg.performance.startup_delay_ms, 1500);

    EXPECT_EQ(cfg.search.max_results, 100);
    EXPECT_TRUE(cfg.search.enable_fuzzy);
    EXPECT_EQ(cfg.search.max_context_lines, 100);
    EXPECT_TRUE(cfg.search.merge_file_results);
    EXPECT_FALSE(cfg.search.ensure_complete_stmt);
    EXPECT_TRUE(cfg.search.include_leading_comments);

    EXPECT_TRUE(cfg.search.ranking.enabled);
    EXPECT_DOUBLE_EQ(cfg.search.ranking.code_file_boost, 50.0);
    EXPECT_DOUBLE_EQ(cfg.search.ranking.doc_file_penalty, -20.0);
    EXPECT_DOUBLE_EQ(cfg.search.ranking.config_file_boost, 10.0);
    EXPECT_FALSE(cfg.search.ranking.require_symbol);
    EXPECT_DOUBLE_EQ(cfg.search.ranking.non_symbol_penalty, -30.0);

    EXPECT_TRUE(cfg.feature_flags.enable_memory_limits);
    EXPECT_TRUE(cfg.feature_flags.enable_graceful_degradation);
    EXPECT_FALSE(cfg.feature_flags.enable_relationship_analysis);
}

// ---------------------------------------------------------------------------
// Semantic scoring defaults
// ---------------------------------------------------------------------------
TEST(DefaultConfigTest, SemanticScoringDefaults) {
    auto cfg = make_default_config();

    EXPECT_DOUBLE_EQ(cfg.semantic_scoring.exact_weight, 1.0);
    EXPECT_DOUBLE_EQ(cfg.semantic_scoring.substring_weight, 0.9);
    EXPECT_DOUBLE_EQ(cfg.semantic_scoring.annotation_weight, 0.85);
    EXPECT_DOUBLE_EQ(cfg.semantic_scoring.fuzzy_weight, 0.70);
    EXPECT_DOUBLE_EQ(cfg.semantic_scoring.stemming_weight, 0.55);
    EXPECT_DOUBLE_EQ(cfg.semantic_scoring.name_split_weight, 0.40);
    EXPECT_DOUBLE_EQ(cfg.semantic_scoring.abbreviation_weight, 0.25);
    EXPECT_DOUBLE_EQ(cfg.semantic_scoring.fuzzy_threshold, 0.7);
    EXPECT_EQ(cfg.semantic_scoring.stem_min_length, 3);
    EXPECT_EQ(cfg.semantic_scoring.max_results, 10);
    EXPECT_DOUBLE_EQ(cfg.semantic_scoring.min_score, 0.2);
}

// ---------------------------------------------------------------------------
// Default exclusion patterns
// ---------------------------------------------------------------------------
TEST(DefaultConfigTest, HasExclusionPatterns) {
    auto cfg = make_default_config();
    EXPECT_FALSE(cfg.exclude.empty());

    auto has = [&](const std::string& pattern) {
        return std::find(cfg.exclude.begin(), cfg.exclude.end(), pattern) !=
               cfg.exclude.end();
    };
    EXPECT_TRUE(has("**/.git/**"));
    EXPECT_TRUE(has("**/node_modules/**"));
    EXPECT_TRUE(has("**/dist/**"));
}

// ---------------------------------------------------------------------------
// Load config from missing file returns defaults
// ---------------------------------------------------------------------------
TEST(LoadConfigTest, MissingFileReturnsDefaults) {
    auto result = load_config("/nonexistent/path/that/does/not/exist");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.config.project.root, "/nonexistent/path/that/does/not/exist");
    EXPECT_EQ(result.config.version, 1);
}

// ---------------------------------------------------------------------------
// Load config from KDL content
// ---------------------------------------------------------------------------
class KdlConfigTest : public ::testing::Test {
  protected:
    fs::path temp_dir_;

    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() / "lci_config_test";
        fs::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    void write_kdl(const std::string& content) {
        std::ofstream f(temp_dir_ / ".lci.kdl");
        f << content;
    }
};

TEST_F(KdlConfigTest, ParsesProjectSection) {
    write_kdl(R"(
project {
    name "my-project"
}
)");
    auto result = load_config(temp_dir_.string());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.config.project.name, "my-project");
}

TEST_F(KdlConfigTest, ParsesExcludeSection) {
    write_kdl(R"(
exclude "**/real_projects/**"
)");
    auto result = load_config(temp_dir_.string());
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.config.exclude.size(), 1u);
    EXPECT_EQ(result.config.exclude[0], "**/real_projects/**");
}

TEST_F(KdlConfigTest, ParsesIndexSection) {
    write_kdl(R"(
index {
    max_file_count 5000
    follow_symlinks true
    priority_mode "small"
    watch_debounce_ms 500
}
)");
    auto result = load_config(temp_dir_.string());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.config.index.max_file_count, 5000);
    EXPECT_TRUE(result.config.index.follow_symlinks);
    EXPECT_EQ(result.config.index.priority_mode, "small");
    EXPECT_EQ(result.config.index.watch_debounce_ms, 500);
}

TEST_F(KdlConfigTest, ParsesPerformanceSection) {
    write_kdl(R"(
performance {
    max_memory_mb 1024
    max_goroutines 8
    debounce_ms 200
    startup_delay_ms 500
}
)");
    auto result = load_config(temp_dir_.string());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.config.performance.max_memory_mb, 1024);
    EXPECT_EQ(result.config.performance.max_goroutines, 8);
    EXPECT_EQ(result.config.performance.debounce_ms, 200);
    EXPECT_EQ(result.config.performance.startup_delay_ms, 500);
}

TEST_F(KdlConfigTest, ParsesSearchRankingSection) {
    write_kdl(R"(
search {
    max_results 50
    enable_fuzzy false
    ranking {
        enabled true
        code_file_boost 75.0
        doc_file_penalty -10.0
    }
}
)");
    auto result = load_config(temp_dir_.string());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.config.search.max_results, 50);
    EXPECT_FALSE(result.config.search.enable_fuzzy);
    EXPECT_DOUBLE_EQ(result.config.search.ranking.code_file_boost, 75.0);
    EXPECT_DOUBLE_EQ(result.config.search.ranking.doc_file_penalty, -10.0);
}

TEST_F(KdlConfigTest, ParsesComments) {
    write_kdl(R"(
// This is a comment
project {
    name "test" // inline comment
}
)");
    auto result = load_config(temp_dir_.string());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.config.project.name, "test");
}

TEST_F(KdlConfigTest, ParsesRealLciConfig) {
    write_kdl(R"(
// LCI Configuration
project {
    name "lci"
}

exclude "**/real_projects/**"
)");
    auto result = load_config(temp_dir_.string());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.config.project.name, "lci");
    ASSERT_EQ(result.config.exclude.size(), 1u);
    EXPECT_EQ(result.config.exclude[0], "**/real_projects/**");
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
TEST(ValidateConfigTest, AcceptsValidDefaults) {
    auto cfg = make_default_config();
    cfg.project.root = "/some/path";
    auto err = validate_config(cfg);
    EXPECT_TRUE(err.empty()) << err;
}

TEST(ValidateConfigTest, RejectsEmptyRoot) {
    auto cfg = make_default_config();
    cfg.project.root = "";
    auto err = validate_config(cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("root"), std::string::npos);
}

TEST(ValidateConfigTest, RejectsNegativeMaxFileSize) {
    auto cfg = make_default_config();
    cfg.project.root = "/some/path";
    cfg.index.max_file_size = -1;
    auto err = validate_config(cfg);
    EXPECT_FALSE(err.empty());
}

TEST(ValidateConfigTest, RejectsExcessiveMaxFileSize) {
    auto cfg = make_default_config();
    cfg.project.root = "/some/path";
    cfg.index.max_file_size = 200 * 1024 * 1024;
    auto err = validate_config(cfg);
    EXPECT_FALSE(err.empty());
}

TEST(ValidateConfigTest, RejectsLowMemory) {
    auto cfg = make_default_config();
    cfg.project.root = "/some/path";
    cfg.performance.max_memory_mb = 50;
    auto err = validate_config(cfg);
    EXPECT_FALSE(err.empty());
}

TEST(ValidateConfigTest, RejectsNegativeGoroutines) {
    auto cfg = make_default_config();
    cfg.project.root = "/some/path";
    cfg.performance.max_goroutines = -1;
    auto err = validate_config(cfg);
    EXPECT_FALSE(err.empty());
}

TEST(ValidateConfigTest, SetsSmartDefaults) {
    auto cfg = make_default_config();
    cfg.project.root = "/some/path";
    cfg.performance.max_goroutines = 0;
    cfg.performance.parallel_file_workers = 0;
    auto err = validate_config(cfg);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_GT(cfg.performance.max_goroutines, 0);
    EXPECT_GT(cfg.performance.parallel_file_workers, 0);
}

TEST(ValidateConfigTest, RejectsNegativeContextLines) {
    auto cfg = make_default_config();
    cfg.project.root = "/some/path";
    cfg.search.max_context_lines = -5;
    auto err = validate_config(cfg);
    EXPECT_FALSE(err.empty());
}

}  // namespace
}  // namespace lci
