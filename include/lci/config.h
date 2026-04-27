#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lci {

// -- Config section structs ---------------------------------------------------

struct ProjectConfig {
    std::string root;
    std::string name;
};

struct IndexConfig {
    int64_t max_file_size = 10 * 1024 * 1024;      // 10 MB
    int64_t max_total_size_mb = 500;
    int max_file_count = 10000;
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
    SemanticConfig semantic;
    SemanticScoringConfig semantic_scoring;
    SearchConfig search;
    FeatureFlagsConfig feature_flags;
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::string propagation_config_dir;
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
