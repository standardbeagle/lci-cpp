#pragma once

#include <lci/analysis/codebase_intelligence.h>
#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_analysis.h>
#include <lci/mcp/handlers_core.h>
#include <lci/mcp/handlers_context.h>
#include <lci/search/search_engine.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lci {
namespace testing {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

/// Finds the lci-cpp project root by walking up from the current path.
/// Looks for markers like CMakeLists.txt, .git, or .lci.kdl.
inline std::optional<fs::path> find_project_root() {
    fs::path cwd = fs::current_path();
    fs::path candidate = cwd;

    // Walk up from current directory
    for (int i = 0; i < 6; ++i) {
        if (fs::exists(candidate / "CMakeLists.txt") ||
            fs::exists(candidate / ".git") ||
            fs::exists(candidate / ".lci.kdl")) {
            return candidate;
        }
        fs::path parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }

    return std::nullopt;
}

/// Locates a real project directory under real_projects/<language>/<name>.
/// Returns std::nullopt if the project is not found.
inline std::optional<fs::path> find_real_project(
    const std::string& language,
    const std::string& project_name) {
    auto root = find_project_root();
    if (!root) {
        return std::nullopt;
    }

    fs::path project_path = *root / "real_projects" / language / project_name;
    if (!fs::exists(project_path) || !fs::is_directory(project_path)) {
        return std::nullopt;
    }

    // Verify it's not an empty submodule (has at least one file)
    bool has_files = false;
    for (const auto& entry : fs::recursive_directory_iterator(
             project_path,
             fs::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file()) {
            has_files = true;
            break;
        }
    }
    if (!has_files) {
        return std::nullopt;
    }

    return project_path;
}

/// Returns a list of all available real projects.
/// Each entry is a pair of (language, project_name).
inline std::vector<std::pair<std::string, std::string>> list_available_real_projects() {
    std::vector<std::pair<std::string, std::string>> result;

    auto root = find_project_root();
    if (!root) {
        return result;
    }

    fs::path rp = *root / "real_projects";
    if (!fs::exists(rp)) {
        return result;
    }

    for (const auto& lang_entry : fs::directory_iterator(rp)) {
        if (!lang_entry.is_directory()) {
            continue;
        }
        std::string lang = lang_entry.path().filename().string();
        for (const auto& proj_entry : fs::directory_iterator(lang_entry.path())) {
            if (!proj_entry.is_directory()) {
                continue;
            }
            // Skip empty submodules
            bool has_files = false;
            for (const auto& f : fs::recursive_directory_iterator(
                     proj_entry.path(),
                     fs::directory_options::skip_permission_denied)) {
                if (f.is_regular_file()) {
                    has_files = true;
                    break;
                }
            }
            if (has_files) {
                result.emplace_back(lang, proj_entry.path().filename().string());
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Config helpers
// ---------------------------------------------------------------------------

/// Creates a test configuration optimized for workflow testing on real projects.
inline Config make_real_project_config(const fs::path& project_path,
                                       const std::string& project_name) {
    Config cfg = make_default_config();
    cfg.project.root = project_path.string();
    cfg.project.name = project_name;

    cfg.index.max_file_size = 10 * 1024 * 1024;  // 10MB
    cfg.index.max_total_size_mb = 500;
    cfg.index.max_file_count = 10000;
    cfg.index.follow_symlinks = false;
    cfg.index.smart_size_control = true;
    cfg.index.priority_mode = "recent";

    cfg.performance.max_memory_mb = 500;
    cfg.performance.debounce_ms = 0;

    cfg.search.max_results = 1000;
    cfg.search.max_context_lines = 100;
    cfg.search.enable_fuzzy = true;
    cfg.search.merge_file_results = true;
    cfg.search.ensure_complete_stmt = false;

    cfg.include.clear();  // Include everything by default
    cfg.exclude = {
        "**/node_modules/**",
        "**/vendor/**",
        "**/.git/**",
        "**/dist/**",
        "**/build/**",
        "**/__pycache__/**",
    };

    return cfg;
}

// ---------------------------------------------------------------------------
// Workflow test context
// ---------------------------------------------------------------------------

/// Provides context for workflow integration tests on real codebases.
/// Mirrors the Go WorkflowTestContext from internal/mcp/workflow_helpers.go.
struct RealProjectContext {
    fs::path project_path;
    std::string project_name;
    Config config;
    std::unique_ptr<MasterIndex> indexer;
    std::unique_ptr<SearchEngine> search_engine;
    std::unique_ptr<CodebaseIntelligenceEngine> ci_engine_;

    RealProjectContext() = default;

    // Non-copyable, movable
    RealProjectContext(const RealProjectContext&) = delete;
    RealProjectContext& operator=(const RealProjectContext&) = delete;
    RealProjectContext(RealProjectContext&&) = default;
    RealProjectContext& operator=(RealProjectContext&&) = default;

    /// Returns true if the context is valid (indexer was created successfully).
    bool valid() const { return indexer != nullptr; }

    /// Searches for a pattern across the indexed project.
    ///
    /// Forces case-insensitive matching so real-project tests are
    /// resilient to corpus capitalization variations (e.g. lowercase
    /// "servehttp" finding "ServeHTTP"). config.search.enable_fuzzy
    /// describes a different feature (fuzzy substring scoring) that is
    /// not currently wired into the C++ search engine; keeping it
    /// uncoupled from case-insensitivity avoids the misleading conflation
    /// previously here.
    std::vector<SearchResult> search(const std::string& pattern,
                                     int max_results = 100,
                                     int max_context_lines = 5) const {
        if (!indexer) {
            return {};
        }
        SearchOptions opts;
        opts.max_results = max_results;
        opts.max_context_lines = max_context_lines;
        opts.merge_file_results = true;
        opts.case_insensitive = true;
        return indexer->search_with_options(pattern, opts);
    }

    /// Returns current index statistics.
    MasterIndexStats stats() const {
        if (!indexer) {
            return {};
        }
        return indexer->get_stats();
    }

    /// Returns the number of indexed files.
    int file_count() const {
        if (!indexer) {
            return 0;
        }
        return indexer->file_count();
    }

    /// Returns the number of symbols.
    int symbol_count() const {
        if (!indexer) {
            return 0;
        }
        return indexer->get_stats().total_symbols;
    }

    /// Calls the code_insight MCP handler.
    nlohmann::json code_insight(nlohmann::json params) const {
        if (!indexer || !ci_engine_) {
            return nlohmann::json{{"error", "Context not initialized"}};
        }
        try {
            auto result = lci::mcp::handle_code_insight(params, *ci_engine_, *indexer);
            return nlohmann::json::parse(result.text);
        } catch (const std::exception& e) {
            return nlohmann::json{{"error", e.what()}};
        } catch (...) {
            return nlohmann::json{{"error", "Unknown error in code_insight"}};
        }
    }

    /// Calls the get_context MCP handler.
    nlohmann::json get_context(nlohmann::json params) const {
        if (!indexer) {
            return nlohmann::json{{"error", "Context not initialized"}};
        }
        try {
            auto result = lci::mcp::handle_get_context(params, *indexer);
            return nlohmann::json::parse(result.text);
        } catch (const std::exception& e) {
            return nlohmann::json{{"error", e.what()}};
        } catch (...) {
            return nlohmann::json{{"error", "Unknown error in get_context"}};
        }
    }

    /// Calls the find_files MCP handler.
    nlohmann::json find_files(nlohmann::json params) const {
        if (!indexer) {
            return nlohmann::json{{"error", "Context not initialized"}};
        }
        try {
            auto result = lci::mcp::handle_find_files(params, *indexer);
            return nlohmann::json::parse(result.text);
        } catch (const std::exception& e) {
            return nlohmann::json{{"error", e.what()}};
        } catch (...) {
            return nlohmann::json{{"error", "Unknown error in find_files"}};
        }
    }

    /// Calls the context MCP handler.
    nlohmann::json context_manifest(nlohmann::json params) const {
        if (!indexer) {
            return nlohmann::json{{"error", "Context not initialized"}};
        }
        try {
            auto result = lci::mcp::handle_context(params, *indexer, project_path.string());
            return nlohmann::json::parse(result.text);
        } catch (const std::exception& e) {
            return nlohmann::json{{"error", e.what()}};
        } catch (...) {
            return nlohmann::json{{"error", "Unknown error in context_manifest"}};
        }
    }
};

/// Sets up a real project for testing by creating a config and indexing it.
/// Returns an invalid context (indexer == nullptr) if setup fails.
inline RealProjectContext setup_real_project(const fs::path& project_path,
                                             const std::string& project_name) {
    RealProjectContext ctx;
    ctx.project_path = project_path;
    ctx.project_name = project_name;
    ctx.config = make_real_project_config(project_path, project_name);

    try {
        ctx.indexer = std::make_unique<MasterIndex>(ctx.config);
        if (!ctx.indexer->index_directory(project_path.string())) {
            ctx.indexer.reset();
            return ctx;
        }
        ctx.search_engine = std::make_unique<SearchEngine>(*ctx.indexer);
        ctx.ci_engine_ = std::make_unique<CodebaseIntelligenceEngine>();
    } catch (...) {
        ctx.indexer.reset();
        ctx.search_engine.reset();
        ctx.ci_engine_.reset();
    }

    return ctx;
}

}  // namespace testing
}  // namespace lci
