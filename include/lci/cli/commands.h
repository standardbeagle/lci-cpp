#pragma once

#include <string>
#include <vector>

#include <lci/config.h>
#include <lci/server/client.h>

namespace lci {
namespace cli {

// -- Global flags captured from CLI11 -----------------------------------------

struct GlobalFlags {
    std::string config_path = ".lci.kdl";
    bool daemon = false;
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::string root;
    bool test_run = false;
};

// -- Config loading with CLI overrides ----------------------------------------

/// Loads configuration from disk and applies global flag overrides.
/// Returns an error message on failure (empty on success).
std::string load_config_with_overrides(const GlobalFlags& flags, Config& out);

// -- Server management -------------------------------------------------------

/// Ensures the index server is running, starting it if necessary.
/// Returns a connected Client, or sets error and returns nullptr.
std::unique_ptr<Client> ensure_server_running(const Config& cfg,
                                              std::string& error);

// -- MCP auto-detection -------------------------------------------------------

/// Returns true if the process should enter MCP mode (piped stdin, env var,
/// or parent process hints).
bool is_mcp_mode();

// -- Subcommand entry points --------------------------------------------------

/// search/grep subcommand. Returns 0 on success, non-zero on error.
int run_search(const GlobalFlags& flags, const std::string& pattern,
               int max_lines, bool case_insensitive, bool json_output,
               bool light, bool compact_search, bool use_regex,
               const std::string& exclude_pattern,
               const std::string& include_pattern,
               bool invert_match, bool count_per_file,
               bool files_only, bool word_boundary,
               int max_count_per_file, bool include_ids, bool no_ids,
               bool comments_only, bool code_only,
               bool strings_only, const std::string& rank_by,
               const std::string& context_filter);

/// grep subcommand. Returns 0 on success, non-zero on error.
int run_grep(const GlobalFlags& flags, const std::string& pattern,
             int max_results, int context_lines, bool case_insensitive,
             bool json_output, const std::string& exclude_pattern,
             const std::string& include_pattern, bool exclude_tests,
             bool exclude_comments, bool use_regex);

/// status subcommand. Returns 0 on success, non-zero on error.
int run_status(const GlobalFlags& flags, bool json_output, bool verbose);

/// server start subcommand. Returns 0 on success, non-zero on error.
int run_server(const GlobalFlags& flags);

/// shutdown subcommand. Returns 0 on success, non-zero on error.
int run_shutdown(const GlobalFlags& flags, bool force);

/// MCP server subcommand. Returns 0 on success, non-zero on error.
int run_mcp(const GlobalFlags& flags);

/// def subcommand: find symbol definition. Returns 0 on success, non-zero on error.
int run_def(const GlobalFlags& flags, const std::string& symbol);

/// refs subcommand: find symbol references. Returns 0 on success, non-zero on error.
int run_refs(const GlobalFlags& flags, const std::string& symbol,
             bool json_output);

/// tree subcommand: display call hierarchy. Returns 0 on success, non-zero on error.
int run_tree(const GlobalFlags& flags, const std::string& function_name,
             int max_depth, bool show_lines, bool compact, bool json_output,
             bool agent_mode, bool metrics, const std::string& exclude_pattern);

/// list subcommand: list indexed files. Returns 0 on success, non-zero on error.
int run_list(const GlobalFlags& flags, bool verbose);

/// config init subcommand. Returns 0 on success, non-zero on error.
int run_config_init(const GlobalFlags& flags, const std::string& format,
                    const std::string& output, bool force, bool minimal);

/// config show subcommand. Returns 0 on success, non-zero on error.
int run_config_show(const GlobalFlags& flags, const std::string& format);

/// config validate subcommand. Returns 0 on success, non-zero on error.
int run_config_validate(const GlobalFlags& flags);

/// git-analyze subcommand. Returns 0 on success, non-zero on error.
int run_git_analyze(const GlobalFlags& flags, const std::string& scope,
                    const std::string& base_ref, const std::string& target_ref,
                    const std::vector<std::string>& focus, double threshold,
                    int max_findings, bool json_output);

/// symbols subcommand. Returns 0 on success, non-zero on error.
int run_symbols(const GlobalFlags& flags, const std::string& kind,
                bool exported, const std::string& file,
                const std::string& name, const std::string& receiver,
                int min_complexity, int max_complexity,
                const std::string& sort, int max_results, bool json_output);

/// inspect subcommand. Returns 0 on success, non-zero on error.
int run_inspect(const GlobalFlags& flags, const std::string& name,
                const std::string& type, const std::string& file,
                const std::string& include_sections, bool json_output);

/// browse subcommand. Returns 0 on success, non-zero on error.
int run_browse(const GlobalFlags& flags, const std::string& file_path,
               const std::string& kind, bool exported,
               const std::string& sort, bool show_imports, bool show_stats,
               bool json_output);

/// debug info subcommand. Returns 0 on success, non-zero on error.
int run_debug_info(const GlobalFlags& flags, bool verbose);

/// debug validate subcommand. Returns 0 on success, non-zero on error.
int run_debug_validate(const GlobalFlags& flags);

/// debug deps subcommand. Returns 0 on success, non-zero on error.
int run_debug_deps(const GlobalFlags& flags, bool verbose);

/// debug export subcommand. Returns 0 on success, non-zero on error.
int run_debug_export(const GlobalFlags& flags, const std::string& output,
                     bool verbose);

/// debug graph subcommand. Returns 0 on success, non-zero on error.
int run_debug_graph(const GlobalFlags& flags, const std::string& output);

// -- Formatting helpers -------------------------------------------------------

std::string format_bytes(int64_t bytes);
std::string format_milliseconds(int64_t ms);
std::string format_seconds(double seconds);

}  // namespace cli
}  // namespace lci
