#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <lci/path_classifier.h>
#include <lci/semantic/synonym_table.h>

namespace lci {

// -- Config section structs ---------------------------------------------------

struct ProjectConfig {
    std::string root;
    std::string name;
};

struct IndexConfig {
    int64_t max_file_size = 10 * 1024 * 1024;      // 10 MB
    // Files larger than this are still trigram-indexed for text search but skip
    // the tree-sitter parse + symbol extraction: a multi-MB source file is
    // almost always generated/minified, where the parse cost (parse is ~58% of
    // index CPU) buys little symbol value. 0 disables the cap.
    int64_t max_parse_file_size = 2 * 1024 * 1024;  // 2 MB
    // Unique-postings-token cap for DATA files (anything language_map marks
    // !is_code: json/csv/logs/word lists...). Their unique-token count is
    // unbounded — hex ids, word lists — and the postings maps retain each
    // token twice plus a per-token file map, ~10x the file's own bytes
    // (209 KB fixture -> 5.6 MB; the 26 GB err-lookup OOM indexed 543 MB of
    // ldjson). Capped files are recorded PARTIAL and self-nominate in every
    // postings lookup, so search stays exact: the filter over-approximates
    // and the content scan verifies. Code files are never capped. 0 = no cap.
    int data_file_token_cap = 4096;
    // Total-corpus budget, enforced by FileScanner in priority order. Sized
    // so the vast majority of repos index fully; what happens past the
    // budget is overflow_policy's call.
    int64_t max_total_size_mb = 500;
    int max_file_count = 50000;
    // "reduced": index the highest-priority files that fit the budget, skip
    // the rest (reported, never silent). "reject": refuse to index and say
    // which limit tripped — for callers that would rather raise the budget
    // or tighten excludes than run partial.
    std::string overflow_policy = "reduced";
    bool follow_symlinks = false;
    // Parsed and stored, but no production reader exists outside config
    // code itself (grep-confirmed) — kept rather than deleted because
    // tests/helpers/real_project_helpers.h (out of this slice's file
    // scope) still assigns them. Tracked as dead in docs/reviews.
    bool smart_size_control = true;
    std::string priority_mode = "recent";
    bool respect_gitignore = true;
    bool watch_mode = true;
    int watch_debounce_ms = 300;
};

struct PerformanceConfig {
    int max_memory_mb = 500;
    int max_goroutines = 0;                          // 0 = auto-detect
    // Parsed and stored, but no production reader outside config code
    // (grep-confirmed) — kept because tests/helpers/real_project_helpers.h
    // (out of this slice's file scope) still assigns it.
    int debounce_ms = 100;
    int parallel_file_workers = 0;                   // 0 = auto-detect
    int indexing_timeout_sec = 120;
};

struct ServerConfig {
    // Index server exits after this long with no requests (/ping excluded,
    // so liveness probes don't keep an unused server alive). 0 disables.
    // The client respawns on the next command, so idle exit is transparent.
    int idle_timeout_sec = 1800;
    // Per-user cap on concurrently resident per-root index servers. A newly
    // started server shuts down least-recently-active peers beyond the cap
    // (registry files in the temp dir carry the activity ordering).
    // 0 disables eviction.
    int max_instances = 8;
    // RSS self-cap. The err-lookup incident: one lci server reached 26 GB
    // RSS on a 2 GB corpus and took the host down -- an index server must
    // never be the process that kills the machine. Each reaper tick reads
    // VmRSS; over the cap it returns freed arena (malloc_trim) and, if
    // still over, exits LOUDLY. Exit is transparent (the client respawns
    // on the next command); a corpus that truly exceeds the cap shows up
    // as repeated exits -- a visible config decision, never silent
    // degradation or a dead host. 0 disables. Linux-only enforcement.
    int max_rss_mb = 4096;
};

// default_context_lines is the only field with a real consumer path
// (SearchEngine's context-line default). search.ranking.* (the former
// SearchRankingConfig), SemanticConfig, SemanticScoringConfig and
// FeatureFlagsConfig had no production reader anywhere outside config
// code and no test-file consumer either — deleted rather than kept "for
// later" (karpathy-principles.md: dead code is deleted, not stubbed).
// The remaining fields below (max_results, max_context_lines,
// enable_fuzzy, merge_file_results, ensure_complete_stmt) are equally
// unread in production, but tests/helpers/real_project_helpers.h and
// three other test files outside this slice's file scope still assign
// them, so deleting them here would break builds this slice cannot fix.
struct SearchConfig {
    int default_context_lines = 0;
    int max_results = 100;
    int max_context_lines = 100;
    bool enable_fuzzy = true;
    bool merge_file_results = true;
    bool ensure_complete_stmt = false;
};

/// code_insight analysis gates. The error-handling / resource-management
/// report is BETA and ships dark: `error_report` is "off" by default.
///   "off"     — the == ERROR HANDLING == / == RESOURCE MANAGEMENT ==
///               sections (and the SUMMARY headline scores) are not
///               computed or emitted.
///   "capture" — sections stay out of every report, but a server writes
///               the full untruncated report to
///               $XDG_STATE_HOME/lci/error-reports/<root-slug>.txt on
///               graceful shutdown. Generate-don't-publish.
///   "on"      — sections emitted in unified/overview/detailed output.
/// Env override: LCI_ERROR_REPORT=off|capture|on beats the file.
struct InsightConfig {
    std::string error_report = "off";
    // Author-declared entry points for code_insight's ENTRY POINTS section:
    //   insight { entry_points "NewRouter" "URLParam" ... }
    // Symbol names; when non-empty they are seated first and the section
    // reports confidence=annotated. Procedural ranking cannot know a
    // library's real front door — only its author can.
    std::vector<std::string> entry_points;
};

/// Complete LCI configuration.
struct Config {
    int version = 1;
    InsightConfig insight;
    ProjectConfig project;
    IndexConfig index;
    PerformanceConfig performance;
    ServerConfig server;
    SearchConfig search;
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    /// Synonym groups for semantic search. Defaults to the built-in curated
    /// dev-verb set (SynonymTable::build_default); a `.lci.kdl` `synonyms`
    /// block can add/override/clear groups.
    SynonymTable synonyms{SynonymTable::build_default()};
    /// Attribute definitions from the `.lci.kdl` `attributes` block — a
    /// project declaring its own attributes, or redefining a shipped one.
    std::vector<AttrDef> attribute_defs;
    /// Shorthand attribute patterns from the same block (`test "legacy/"`).
    /// Checked before the shipped patterns; see include/lci/path_classifier.h.
    std::vector<PathAttrRule> attributes;
};

// -- Config loading -----------------------------------------------------------

/// Creates a Config with all default values.
/// The project root is set to the current working directory.
Config make_default_config();

/// Result of loading configuration. Holds either a Config or an error message.
struct ConfigResult {
    Config config;
    std::string error;
    /// Non-fatal diagnostics: unrecognized keys, which are ignored rather
    /// than rejected so a newer config file still loads on an older binary.
    /// Callers that face a human should print these.
    std::vector<std::string> warnings;
    /// The file the config was loaded from; empty when the result came from
    /// defaults (no file on disk). Lets `lci config validate` name the file
    /// its verdict refers to.
    std::string source;
    bool ok() const { return error.empty(); }
};

/// Loads configuration from a .lci.kdl file in the given directory.
/// If no .lci.kdl file exists, returns defaults for that directory.
/// Sets result.error on parse failure OR validation failure — validation and
/// the 0-means-auto smart defaults run here, so every caller gets a config
/// that has been range-checked, not just `lci config show`.
ConfigResult load_config(const std::string& project_root);

/// Parses raw .lci.kdl content into a Config (the parse path load_config
/// takes for a project file, minus the filesystem). On a malformed document
/// returns defaults and sets `error`. Exposed for tests and fuzzing: the
/// content is untrusted input (cloned repos carry their own .lci.kdl).
Config parse_kdl_content(const std::string& content, std::string& error,
                         std::vector<std::string>* warnings = nullptr);
/// Loads configuration from a specific file, resolving relative paths inside
/// it against `project_root`. Unlike load_config, a missing file is an ERROR:
/// the caller named this file explicitly, so falling back to defaults would
/// apply settings they never wrote.
ConfigResult load_config_file(const std::string& config_path,
                              const std::string& project_root);

// -- Config validation --------------------------------------------------------

/// Validates configuration and applies smart defaults.
/// Modifies the config in place (e.g., setting worker counts based on CPU).
/// Returns an error description on validation failure, or empty string on success.
std::string validate_config(Config& cfg);

}  // namespace lci
