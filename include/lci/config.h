#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
    bool smart_size_control = true;
    std::string priority_mode = "recent";
    bool respect_gitignore = true;
    bool watch_mode = true;
    int watch_debounce_ms = 300;
};

struct PerformanceConfig {
    int max_memory_mb = 500;
    int max_goroutines = 0;                          // 0 = auto-detect
    int debounce_ms = 100;
    int parallel_file_workers = 0;                   // 0 = auto-detect
    int indexing_timeout_sec = 120;
    int startup_delay_ms = 1500;
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

struct SemanticConfig {
    int batch_size = 100;
    int channel_size = 1000;
    int min_stem_length = 3;
    int cache_size = 1000;
};

struct SemanticScoringConfig {
    double exact_weight = 1.0;
    double substring_weight = 0.9;
    double annotation_weight = 0.85;
    double fuzzy_weight = 0.70;
    double stemming_weight = 0.55;
    double name_split_weight = 0.40;
    double abbreviation_weight = 0.25;
    double fuzzy_threshold = 0.7;
    int stem_min_length = 3;
    int max_results = 10;
    double min_score = 0.2;
};

inline constexpr double kDefaultCodeFileBoost = 50.0;
inline constexpr double kDefaultDocFilePenalty = -20.0;
inline constexpr double kDefaultConfigFileBoost = 10.0;
inline constexpr double kDefaultNonSymbolPenalty = -30.0;

struct SearchRankingConfig {
    bool enabled = true;
    double code_file_boost = kDefaultCodeFileBoost;
    double doc_file_penalty = kDefaultDocFilePenalty;
    double config_file_boost = kDefaultConfigFileBoost;
    bool require_symbol = false;
    double non_symbol_penalty = kDefaultNonSymbolPenalty;
};

struct SearchConfig {
    int default_context_lines = 0;
    int max_results = 100;
    // NOTE: parsed and stored for parity with Go's config schema
    // (internal/config/config.go field EnableFuzzy), but NOT currently
    // consumed by the C++ search engine. Setting it has no behavioral
    // effect today. Tracked under Dart task qkbC8BBuW14H — either wire
    // through to a real fuzzy-scoring path or remove from both schemas.
    bool enable_fuzzy = true;
    int max_context_lines = 100;
    bool merge_file_results = true;
    bool ensure_complete_stmt = false;
    bool include_leading_comments = true;
    SearchRankingConfig ranking;
};

struct FeatureFlagsConfig {
    bool enable_memory_limits = true;
    bool enable_graceful_degradation = true;
    bool enable_relationship_analysis = false;
    bool enable_performance_monitoring = true;
    bool enable_detailed_error_logging = true;
    bool enable_feature_flag_logging = true;
};

/// Complete LCI configuration.
struct Config {
    int version = 1;
    ProjectConfig project;
    IndexConfig index;
    PerformanceConfig performance;
    ServerConfig server;
    SemanticConfig semantic;
    SemanticScoringConfig semantic_scoring;
    SearchConfig search;
    FeatureFlagsConfig feature_flags;
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::string propagation_config_dir;
    /// Synonym groups for semantic search. Defaults to the built-in curated
    /// dev-verb set (SynonymTable::build_default); a `.lci.kdl` `synonyms`
    /// block can add/override/clear groups.
    SynonymTable synonyms{SynonymTable::build_default()};
};

// -- Config loading -----------------------------------------------------------

/// Creates a Config with all default values.
/// The project root is set to the current working directory.
Config make_default_config();

/// Result of loading configuration. Holds either a Config or an error message.
struct ConfigResult {
    Config config;
    std::string error;
    bool ok() const { return error.empty(); }
};

/// Loads configuration from a .lci.kdl file in the given directory.
/// If no .lci.kdl file exists, returns defaults for that directory.
/// Sets result.error on parse failure.
ConfigResult load_config(const std::string& project_root);

// -- Config validation --------------------------------------------------------

/// Validates configuration and applies smart defaults.
/// Modifies the config in place (e.g., setting worker counts based on CPU).
/// Returns an error description on validation failure, or empty string on success.
std::string validate_config(Config& cfg);

}  // namespace lci
