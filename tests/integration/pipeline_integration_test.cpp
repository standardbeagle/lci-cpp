#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/config/gitignore.h>
#include <lci/core/file_service.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/trigram.h>
#include <lci/indexing/master_index.h>
#include <lci/indexing/pipeline.h>
#include <lci/indexing/pipeline_types.h>
#include <lci/search/search_engine.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace lci {
namespace {

// TempDir matches the pattern used by existing passing tests.
class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lci_pipeline_integ_" + std::to_string(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
                    std::hash<int>{}(counter_++)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    void write_file(const std::string& rel_path,
                    const std::string& content) {
        auto full = path_ / rel_path;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream f(full);
        f << content;
    }

  private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// -- Fixture: multi-language project -----------------------------------------

class PipelineIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_.write_file("main.go",
            "package main\n"
            "\n"
            "import \"fmt\"\n"
            "\n"
            "func main() {\n"
            "    fmt.Println(greet(\"world\"))\n"
            "}\n"
            "\n"
            "func greet(name string) string {\n"
            "    return \"Hello, \" + name\n"
            "}\n");
        dir_.write_file("util.go",
            "package main\n"
            "\n"
            "func add(a, b int) int {\n"
            "    return a + b\n"
            "}\n"
            "\n"
            "func multiply(a, b int) int {\n"
            "    return a * b\n"
            "}\n");
        dir_.write_file("app.py",
            "class UserService:\n"
            "    def __init__(self, db):\n"
            "        self.db = db\n"
            "\n"
            "    def get_user(self, user_id):\n"
            "        return self.db.find(user_id)\n"
            "\n"
            "    def create_user(self, name, email):\n"
            "        return self.db.insert({'name': name, 'email': email})\n");
        dir_.write_file("src/index.js",
            "const express = require('express');\n"
            "\n"
            "function createApp() {\n"
            "    const app = express();\n"
            "    app.get('/health', (req, res) => res.json({ok: true}));\n"
            "    return app;\n"
            "}\n"
            "\n"
            "module.exports = { createApp };\n");
        dir_.write_file("src/types.ts",
            "export interface User {\n"
            "    id: number;\n"
            "    name: string;\n"
            "    email: string;\n"
            "}\n"
            "\n"
            "export function formatUser(user: User): string {\n"
            "    return `${user.name} <${user.email}>`;\n"
            "}\n");
        dir_.write_file("lib.rs",
            "pub struct Config {\n"
            "    pub name: String,\n"
            "    pub debug: bool,\n"
            "}\n"
            "\n"
            "impl Config {\n"
            "    pub fn new(name: &str) -> Self {\n"
            "        Config { name: name.to_string(), debug: false }\n"
            "    }\n"
            "}\n");
        dir_.write_file("config.json",
            "{\"name\": \"test-project\", \"version\": \"1.0\"}\n");
        dir_.write_file("README.md",
            "# Test Project\n"
            "\n"
            "A multi-language test fixture for pipeline integration.\n");
    }

    TempDir dir_;
};

// -- Pipeline indexes all files -----------------------------------------------

TEST_F(PipelineIntegrationTest, IndexesMultiLanguageProject) {
    Config cfg = make_default_config();
    cfg.project.root = dir_.path().string();

    auto store = std::make_shared<FileContentStore>();
    auto file_service = std::make_shared<FileService>(store);
    TrigramIndex trigram_idx;
    ReferenceTracker ref_tracker;
    PostingsIndex postings_idx;

    Pipeline pipeline(cfg, file_service, &trigram_idx,
                      &ref_tracker, &postings_idx);
    pipeline.run();

    auto progress = pipeline.get_progress();
    EXPECT_FALSE(progress.is_scanning);
    EXPECT_EQ(progress.scanning_progress, 100.0);
    EXPECT_GE(progress.files_processed, 7);
    EXPECT_EQ(progress.total_files, progress.files_processed);
    EXPECT_GT(progress.files_per_second, 0.0);
    EXPECT_TRUE(progress.errors.empty())
        << "Pipeline errors: " << progress.errors.size();
}

TEST_F(PipelineIntegrationTest, IndexStateConsistent) {
    Config cfg = make_default_config();
    cfg.project.root = dir_.path().string();

    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir_.path().string()));

    EXPECT_GE(mi.file_count(), 7);
    EXPECT_GE(mi.get_stats().total_files, 7);
}

TEST_F(PipelineIntegrationTest, SearchFindsGoSymbol) {
    // Use a single-file fixture for deterministic search results
    TempDir single;
    single.write_file("main.go",
        "package main\n"
        "\n"
        "func greet(name string) string {\n"
        "    return \"Hello, \" + name\n"
        "}\n");

    Config cfg = make_default_config();
    cfg.project.root = single.path().string();

    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(single.path().string()));

    auto results = mi.search("greet", 0);
    EXPECT_GE(results.size(), 1u) << "Go function 'greet' not found";
}

TEST_F(PipelineIntegrationTest, IndexesAllLanguages) {
    Config cfg = make_default_config();
    cfg.project.root = dir_.path().string();

    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir_.path().string()));

    EXPECT_GE(mi.file_count(), 7);
}

TEST_F(PipelineIntegrationTest, RespectGitignore) {
    dir_.write_file(".gitignore", "build/\n*.log\n");
    dir_.write_file("build/output.js", "console.log('built');");
    dir_.write_file("debug.log", "log data here");

    Config cfg = make_default_config();
    cfg.project.root = dir_.path().string();
    cfg.index.respect_gitignore = true;

    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(dir_.path().string()));

    SearchEngine engine(mi);
    SearchOptions opts;

    auto log_results = engine.search("log data here", opts);
    for (const auto& r : log_results) {
        EXPECT_EQ(r.path.find("debug.log"), std::string::npos)
            << "Gitignored file appeared in search results: " << r.path;
    }
}

TEST_F(PipelineIntegrationTest, ReIndexingProducesSameResults) {
    Config cfg = make_default_config();
    cfg.project.root = dir_.path().string();

    MasterIndex mi1(cfg);
    ASSERT_TRUE(mi1.index_directory(dir_.path().string()));
    int file_count1 = mi1.file_count();
    int symbol_count1 = mi1.get_stats().total_symbols;

    MasterIndex mi2(cfg);
    ASSERT_TRUE(mi2.index_directory(dir_.path().string()));
    int file_count2 = mi2.file_count();
    int symbol_count2 = mi2.get_stats().total_symbols;

    EXPECT_EQ(file_count1, file_count2);
    EXPECT_EQ(symbol_count1, symbol_count2);
}

}  // namespace
}  // namespace lci
