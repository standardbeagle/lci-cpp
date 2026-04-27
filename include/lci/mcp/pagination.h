#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci {
namespace mcp {

// -- Pagination constants -----------------------------------------------------

inline constexpr int kPaginationDefaultContextLines = 3;
inline constexpr int kPaginationBaseTokens = 50;
inline constexpr int kPaginationMetadataTokens = 100;
inline constexpr int kDefaultMaxTokens = 8000;
inline constexpr int kMinGuaranteedResults = 3;

// -- TokenEstimator -----------------------------------------------------------

/// Estimates token counts for search results using character-based heuristics.
class TokenEstimator {
  public:
    TokenEstimator() = default;

    /// Estimates token count for an arbitrary JSON-serializable value.
    int estimate_tokens(const nlohmann::json& value) const;

  private:
    static constexpr double kCharsPerToken = 4.0;
    static constexpr double kJsonOverhead = 1.2;
};

// -- PaginationConfig ---------------------------------------------------------

struct PaginationConfig {
    int default_max_tokens{20000};
    int min_page_size{5};
    int max_page_size{1000};
    double token_safety_margin{0.9};
    bool smart_limit_enabled{true};
};

PaginationConfig default_pagination_config();

// -- PaginationResult ---------------------------------------------------------

/// Holds the paginated slice of results plus metadata.
struct PaginationResult {
    std::string query;
    double time_ms{0};
    int page{0};
    int page_size{0};
    int count{0};
    int total_count{-1};
    bool has_more{false};
    int token_count{0};
    int max_tokens{0};
    bool enhanced{false};
    nlohmann::json results;

    int suggested_page_size{0};
    bool auto_truncated{false};
    std::optional<int> next_page;
    std::optional<int> prev_page;

    /// Serialises the pagination result to JSON.
    nlohmann::json to_json() const;
};

// -- AdaptivePaginator --------------------------------------------------------

/// Token-aware paginator that truncates result sets to fit within budgets.
class AdaptivePaginator {
  public:
    AdaptivePaginator();

    /// Calculates the optimal page size given a token budget and sample result.
    int calculate_optimal_page_size(int max_tokens,
                                    const nlohmann::json& sample,
                                    const std::string& output_mode) const;

    /// Paginates a JSON array of results with token-aware truncation.
    PaginationResult apply_pagination(const nlohmann::json& results,
                                      int max_results, int total_count,
                                      const std::string& query, double time_ms,
                                      const std::string& output_mode) const;

    /// Groups results by a field (e.g. "file" or "directory").
    nlohmann::json group_results(const nlohmann::json& results,
                                 const std::string& group_by) const;

    /// Generates summary statistics for a result set.
    nlohmann::json generate_summary(const nlohmann::json& results) const;

  private:
    /// Truncates a JSON array to fit within the token budget, guaranteeing
    /// at least kMinGuaranteedResults entries when available.
    std::pair<nlohmann::json, bool> truncate_by_tokens(
        const nlohmann::json& results, int max_tokens) const;

    int smart_result_limit(const std::string& output_mode) const;

    TokenEstimator estimator_;
    PaginationConfig config_;
};

}  // namespace mcp
}  // namespace lci
