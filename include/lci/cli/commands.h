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
    // Hidden Go-parity globals — write CPU/memory profiles via gperftools.
    // Empty = disabled. When non-empty without gperftools at build time, the
    // runtime path fails fast with a clear error (Karpathy rule 6).
    std::string profile_cpu_path;
    std::string profile_memory_path;
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

struct SearchCommandOptions {
    std::string pattern;
    int max_lines{};
    bool case_insensitive{};
    bool json_output{};
    bool light{};
    bool compact{};
    bool use_regex{};
    std::string exclude_pattern;
    std::string include_pattern;
    bool invert_match{};
    std::vector<std::string> extra_patterns;
    bool count_per_file{};
    bool files_only{};
    bool word_boundary{};
    int max_count_per_file{};
    bool include_ids{};
    bool no_ids{};
    bool comments_only{};
    bool code_only{};
    bool strings_only{};
    std::string rank_by;
    std::string context_filter;
    bool template_strings{};
    bool verbose{};
    bool compare_search{};
    std::string cpu_profile_path;
    std::string mem_profile_path;
};

/// Search subcommand. Returns 0 on success, non-zero on error.
int run_search(const GlobalFlags& flags, const SearchCommandOptions& options);

/// grep subcommand. Returns 0 on success, non-zero on error.
///
/// Grep-compatible filter flags (all post-process matches returned by the
/// server; the server itself only honors `case_insensitive`):
///   - `invert_match`     : grep -v, list lines that do NOT match.
///   - `extra_patterns`   : grep -e, OR additional patterns into a single query.
///   - `count_per_file`   : grep -c, emit "path: N" per file.
///   - `files_only`       : grep -l, emit only paths with at least one match.
///   - `max_count_per_file`: grep -m, cap matches per file (0 = unlimited).
int run_grep(const GlobalFlags& flags, const std::string& pattern,
             int max_results, int context_lines, bool case_insensitive,
             bool json_output, const std::string& exclude_pattern,
             const std::string& include_pattern, bool exclude_tests,
             bool exclude_comments, bool use_regex,
             bool invert_match,
             const std::vector<std::string>& extra_patterns,
             bool count_per_file, bool files_only,
             int max_count_per_file, bool verbose);

/// status subcommand. Returns 0 on success, non-zero on error.
int run_status(const GlobalFlags& flags, bool json_output, bool verbose);

/// server start subcommand. Returns 0 on success, non-zero on error.
///
/// `daemon` requests a forked background process (current-process exits 0
/// after spawn). `foreground` forces in-process operation regardless of
/// daemon and is the default for `lci server` (matches Go cmd/lci/main.go:808
/// `--foreground` default true). When both flags are set, `foreground` wins
/// and a warning is emitted — explicit foreground always overrides daemon
/// for the debug path.
int run_server(const GlobalFlags& flags, bool daemon, bool foreground);

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
/// `incremental` switches to incremental-index introspection (matches Go's
/// `--incremental`/`--inc` flag). When true, the output's "Incremental Mode"
/// header is set to true and the file listing is filtered to files that
/// changed (added/modified/removed) relative to the persisted snapshot
/// manifest at `<root>/.lci/debug-snapshot.json`; the manifest is then
/// refreshed to the current on-disk state. The first-ever incremental run
/// (no prior manifest) establishes the baseline and reports every file as
/// new. The default (non-incremental) invocation is a pure read and does not
/// touch the snapshot.
int run_debug_info(const GlobalFlags& flags, bool verbose, bool incremental);

/// debug validate subcommand. Returns 0 on success, non-zero on error.
/// `incremental` runs incremental-mode consistency checks (matches Go).
int run_debug_validate(const GlobalFlags& flags, bool incremental);

/// debug deps subcommand. Returns 0 on success, non-zero on error.
int run_debug_deps(const GlobalFlags& flags, bool verbose);

/// debug export subcommand. Returns 0 on success, non-zero on error.
/// `incremental` exports the incremental-index delta instead of the full
/// snapshot (matches Go).
int run_debug_export(const GlobalFlags& flags, const std::string& output,
                     bool verbose, bool incremental);

/// debug graph subcommand. Returns 0 on success, non-zero on error.
int run_debug_graph(const GlobalFlags& flags, const std::string& output);

/// update subcommand: self-update the lci binary from the latest GitHub
/// release. `check_only` reports current vs latest without writing; `force`
/// reinstalls even when already current; `version` (empty = latest) pins a
/// specific release tag. Returns 0 on success, non-zero on error.
int run_update(bool check_only, bool force, const std::string& version);

// -- Formatting helpers -------------------------------------------------------

std::string format_bytes(int64_t bytes);
std::string format_milliseconds(int64_t ms);
std::string format_seconds(double seconds);

}  // namespace cli
}  // namespace lci
