#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/pipeline_scanner.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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
    // enable_fuzzy: default true for Go schema parity; not yet wired
    // into the C++ search engine (tracked Dart task qkbC8BBuW14H).
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
// Default-exclude contract: pin the exact pattern list to match Go binary.
// Any drift here is a parity-defeating divergence — file a fix or update
// both sides together.
// ---------------------------------------------------------------------------

TEST(DefaultExcludeContract, MatchesGoBinaryPatternCount) {
    auto cfg = make_default_config();
    // Go binary reports "Exclude Patterns (124)" on `lci config show`.
    // Locking the count here catches accidental drops or duplicates.
    EXPECT_EQ(cfg.exclude.size(), 124u);
}

TEST(DefaultExcludeContract, IncludesAllVcsAndDotfileDirs) {
    auto cfg = make_default_config();
    auto has = [&](const std::string& p) {
        return std::find(cfg.exclude.begin(), cfg.exclude.end(), p) !=
               cfg.exclude.end();
    };
    EXPECT_TRUE(has("**/.git/**"));
    EXPECT_TRUE(has("**/.*/**"));
}

TEST(DefaultExcludeContract, IncludesAllBuildAndDepDirs) {
    auto cfg = make_default_config();
    auto has = [&](const std::string& p) {
        return std::find(cfg.exclude.begin(), cfg.exclude.end(), p) !=
               cfg.exclude.end();
    };
    for (const auto* p : {"**/node_modules/**", "**/vendor/**",
                           "**/bower_components/**", "**/jspm_packages/**",
                           "**/dist/**", "**/build/**", "**/out/**",
                           "**/target/**", "**/bin/**", "**/obj/**",
                           "**/ui/**", "**/public/**"}) {
        EXPECT_TRUE(has(p)) << "missing build/dep exclude: " << p;
    }
}

TEST(DefaultExcludeContract, IncludesAllTestFilePatterns) {
    auto cfg = make_default_config();
    auto has = [&](const std::string& p) {
        return std::find(cfg.exclude.begin(), cfg.exclude.end(), p) !=
               cfg.exclude.end();
    };
    // Language-specific test file patterns.
    for (const auto* p : {"**/*_test.go", "**/*_tests.go",
                           "**/*_test.py", "**/*_tests.py",
                           "**/test_*.py", "**/tests_*.py",
                           "**/*.test.js", "**/*.test.ts",
                           "**/*.test.tsx", "**/*.test.jsx",
                           "**/*.spec.js", "**/*.spec.ts",
                           "**/*.spec.tsx", "**/*.spec.jsx",
                           "**/*_test.rb", "**/*_spec.rb",
                           "**/*Test.java", "**/*Tests.java",
                           "**/*TestCase.java", "**/*Test.cs",
                           "**/*Tests.cs", "**/*Test.csproj",
                           "**/*Test.php", "**/*TestCase.php",
                           "**/*Test.kt", "**/*Tests.kt",
                           "**/*TestCase.kt", "**/*Test.swift",
                           "**/*Test.m", "**/*Test.h",
                           "**/test_*", "**/tests_*"}) {
        EXPECT_TRUE(has(p)) << "missing test file exclude: " << p;
    }
    // Test directory patterns.
    for (const auto* p : {"**/__tests__/**", "**/test/**", "**/tests/**",
                           "**/testdata/**", "**/__testdata__/**",
                           "**/fixtures/**", "**/.test/**"}) {
        EXPECT_TRUE(has(p)) << "missing test dir exclude: " << p;
    }
}

TEST(DefaultExcludeContract, IncludesMinifiedAndCacheArtifacts) {
    auto cfg = make_default_config();
    auto has = [&](const std::string& p) {
        return std::find(cfg.exclude.begin(), cfg.exclude.end(), p) !=
               cfg.exclude.end();
    };
    for (const auto* p : {"**/*.min.js", "**/*.min.css", "**/*.bundle.js",
                           "**/*.chunk.js", "**/*.min.map",
                           "**/__pycache__/**", "**/*.pyc",
                           "**/Thumbs.db", "**/desktop.ini",
                           "**/logs/**", "**/*.log",
                           "**/*.swp", "**/*.swo", "**/*~"}) {
        EXPECT_TRUE(has(p)) << "missing cache/minified exclude: " << p;
    }
}

TEST(DefaultExcludeContract, IncludesAllBinaryAssetExtensions) {
    auto cfg = make_default_config();
    auto has = [&](const std::string& p) {
        return std::find(cfg.exclude.begin(), cfg.exclude.end(), p) !=
               cfg.exclude.end();
    };
    // Fonts, images, video, audio, office docs.
    const char* font_video[] = {
        "**/*.avif", "**/*.webp", "**/*.wasm",
        "**/*.woff", "**/*.woff2", "**/*.ttf", "**/*.eot", "**/*.otf",
        "**/*.mp4", "**/*.avi", "**/*.mov", "**/*.wmv", "**/*.flv",
        "**/*.mkv", "**/*.webm", "**/*.m4v",
        "**/*.mpg", "**/*.mpeg", "**/*.3gp", "**/*.ogv",
    };
    for (const auto* p : font_video) {
        EXPECT_TRUE(has(p)) << "missing font/video exclude: " << p;
    }
    const char* audio[] = {"**/*.mp3", "**/*.wav", "**/*.flac",
                            "**/*.aac", "**/*.ogg", "**/*.wma",
                            "**/*.m4a", "**/*.aiff", "**/*.ape"};
    for (const auto* p : audio) {
        EXPECT_TRUE(has(p)) << "missing audio exclude: " << p;
    }
    const char* office[] = {"**/*.doc", "**/*.docx", "**/*.docm",
                             "**/*.xls", "**/*.xlsx", "**/*.xlsm",
                             "**/*.xlsb", "**/*.xlt", "**/*.xltx",
                             "**/*.xltm", "**/*.xlam",
                             "**/*.ppt", "**/*.pptx", "**/*.pptm",
                             "**/*.pps", "**/*.ppsx", "**/*.ppsm",
                             "**/*.pot", "**/*.potx", "**/*.potm",
                             "**/*.odt", "**/*.ods", "**/*.odp",
                             "**/*.rtf", "**/*.pages", "**/*.numbers",
                             "**/*.key"};
    for (const auto* p : office) {
        EXPECT_TRUE(has(p)) << "missing office exclude: " << p;
    }
}

// ---------------------------------------------------------------------------
// FileScanner::match_glob on default patterns: exercise representative paths
// to lock semantic behavior (NOT just pattern presence).
// ---------------------------------------------------------------------------

namespace {

// Lookup helper: does any default-exclude pattern match `rel_path`?
bool excluded_by_default(const std::vector<std::string>& patterns,
                         std::string_view rel_path) {
    for (const auto& p : patterns) {
        if (FileScanner::match_glob(p, rel_path)) return true;
    }
    return false;
}

}  // namespace

TEST(DefaultExcludeMatchGlob, GitDirectoryExcluded) {
    auto cfg = make_default_config();
    EXPECT_TRUE(excluded_by_default(cfg.exclude, ".git/HEAD"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, ".git/config"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "src/.git/HEAD"));
}

TEST(DefaultExcludeMatchGlob, NodeModulesExcluded) {
    auto cfg = make_default_config();
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "node_modules/lodash/index.js"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "frontend/node_modules/x.js"));
}

TEST(DefaultExcludeMatchGlob, BuildDirectoriesExcluded) {
    auto cfg = make_default_config();
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "build/main.o"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "dist/bundle.js"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "target/release/foo"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "out/lib.so"));
}

TEST(DefaultExcludeMatchGlob, MinifiedAssetsExcluded) {
    auto cfg = make_default_config();
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "vendor/jquery.min.js"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "styles/main.min.css"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "dist/app.bundle.js"));
}

TEST(DefaultExcludeMatchGlob, TestFilesExcluded) {
    auto cfg = make_default_config();
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "pkg/foo_test.go"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "src/test_helper.py"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "tests/foo_test.rb"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "src/CalculatorTest.java"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "app/components/Button.test.tsx"));
}

TEST(DefaultExcludeMatchGlob, TestDirectoriesExcluded) {
    auto cfg = make_default_config();
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "tests/integration/foo.go"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "src/__tests__/x.js"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "pkg/testdata/sample.json"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "internal/fixtures/data.yml"));
}

TEST(DefaultExcludeMatchGlob, NormalSourceFilesNotExcluded) {
    auto cfg = make_default_config();
    // Production code that must remain indexable.
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "src/main.go"));
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "lib/utils.py"));
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "app/handlers/auth.ts"));
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "include/lci/config.h"));
}

TEST(DefaultExcludeMatchGlob, TempDirLikePathsNotFalsePositive) {
    // Multi-component paths whose intermediate directory contains
    // `test_` as a substring must not be excluded. This was the
    // original watcher bug: temp dir `lci_watcher_test_42` was getting
    // filtered out by `**/test_*` because the glob was over-greedy.
    auto cfg = make_default_config();
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "main.go"));
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "src/normal.go"));
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "atest.go"));
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "my_helper.go"));

    // Strict: regression guards for the cross-component-substring bug.
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "lci_watcher_test_42/main.go"))
        << "Temp dir containing 'test_' as substring must not be excluded";
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "my_research_tool/index.py"));
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "atest_helper/x.go"));
}

TEST(DefaultExcludeMatchGlob, BinaryAssetsExcluded) {
    auto cfg = make_default_config();
    // Note: common image formats like .png, .jpg, .gif are NOT in the
    // default exclude list — only specific font/video/audio formats are.
    // If that policy changes, expand this test.
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "src/data/audio.mp3"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "docs/diagram.docx"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "design/preview.webp"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "build/output.wasm"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "videos/intro.mp4"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "fonts/inter.woff2"));
}

TEST(DefaultExcludeMatchGlob, CommonImageFormatsNotExcludedByDefault) {
    // Lock the current policy: .png/.jpg/.gif are intentionally indexable.
    // (Some users want OCR/alt-text indexing; default policy keeps them in.)
    auto cfg = make_default_config();
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "assets/logo.png"));
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "img/icon.jpg"));
    EXPECT_FALSE(excluded_by_default(cfg.exclude, "graphics/sprite.gif"));
}

TEST(DefaultExcludeMatchGlob, PythonCacheArtifactsExcluded) {
    auto cfg = make_default_config();
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "__pycache__/module.cpython-310.pyc"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "src/foo.pyc"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "app/__pycache__/x.pyc"));
}

TEST(DefaultExcludeMatchGlob, LogFilesExcluded) {
    auto cfg = make_default_config();
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "server.log"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "logs/access.log"));
    EXPECT_TRUE(excluded_by_default(cfg.exclude, "var/logs/app.log"));
}

TEST(DefaultExcludeMatchGlob, NoDuplicatePatterns) {
    // Currently `**/tests/**` appears twice in the list. Document the
    // current state; if dedup happens, update the expected count above.
    auto cfg = make_default_config();
    std::vector<std::string> sorted = cfg.exclude;
    std::sort(sorted.begin(), sorted.end());
    int dup_count = 0;
    for (size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i] == sorted[i - 1]) ++dup_count;
    }
    // `**/tests/**` is listed twice — once at index 38, once at 50 (per
    // src/config/config.cpp). Until dedup, expect exactly one duplicate.
    EXPECT_EQ(dup_count, 1)
        << "Unexpected number of duplicate patterns. The current source has "
           "one known duplicate ('**/tests/**'). If dedup happens, update "
           "MatchesGoBinaryPatternCount above to 123.";
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
// Malformed KDL: graceful error, no crash / no hang
// ---------------------------------------------------------------------------
TEST_F(KdlConfigTest, RejectsKdlV2TrueToken) {
    write_kdl("index {\n  respect_gitignore #true\n}\n");
    auto result = load_config(temp_dir_.string());
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find('#'), std::string::npos) << result.error;
}

TEST_F(KdlConfigTest, RejectsKdlV2FalseToken) {
    write_kdl("index {\n  respect_gitignore #false\n}\n");
    auto result = load_config(temp_dir_.string());
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find('#'), std::string::npos) << result.error;
}

TEST_F(KdlConfigTest, RejectsBareHashToken) {
    write_kdl("index {\n  respect_gitignore #\n}\n");
    auto result = load_config(temp_dir_.string());
    EXPECT_FALSE(result.ok());
}

TEST_F(KdlConfigTest, RejectsUnknownHashKeyword) {
    write_kdl("index {\n  respect_gitignore #unknown\n}\n");
    auto result = load_config(temp_dir_.string());
    EXPECT_FALSE(result.ok());
}

TEST_F(KdlConfigTest, MalformedErrorReportsLineNumber) {
    write_kdl("project {\n  name \"ok\"\n}\nindex {\n  respect_gitignore #true\n}\n");
    auto result = load_config(temp_dir_.string());
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error.find("line 5"), std::string::npos) << result.error;
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
