#include <lci/mcp/pagination.h>

#include <algorithm>

namespace lci {
namespace mcp {

// -- PaginationConfig ---------------------------------------------------------

PaginationConfig default_pagination_config() {
    return {20000, 5, 1000, 0.9, true};
}

// -- TokenEstimator -----------------------------------------------------------

int TokenEstimator::estimate_tokens(const nlohmann::json& value) const {
    // Lossy UTF-8: this only needs a character count for the token estimate,
    // and a strict dump would throw on non-UTF-8 content in the payload.
    auto s = value.dump(-1, ' ', /*ensure_ascii=*/false,
                        nlohmann::json::error_handler_t::replace);
    int char_count = static_cast<int>(s.size());
    return static_cast<int>(static_cast<double>(char_count) / kCharsPerToken *
                            kJsonOverhead);
}

// -- PaginationResult ---------------------------------------------------------

nlohmann::json PaginationResult::to_json() const {
    nlohmann::json j;
    j["query"] = query;
    j["time_ms"] = time_ms;
    j["page"] = page;
    j["page_size"] = page_size;
    j["count"] = count;
    j["total_count"] = total_count;
    j["has_more"] = has_more;
    j["token_count"] = token_count;
    j["max_tokens"] = max_tokens;
    j["enhanced"] = enhanced;
    j["results"] = results;

    if (suggested_page_size > 0) {
        j["suggested_page_size"] = suggested_page_size;
    }
    if (auto_truncated) {
        j["auto_truncated"] = true;
    }
    if (next_page) {
        j["next_page"] = *next_page;
    }
    if (prev_page) {
        j["prev_page"] = *prev_page;
    }

    return j;
}

// -- AdaptivePaginator --------------------------------------------------------

AdaptivePaginator::AdaptivePaginator()
    : config_(default_pagination_config()) {}

int AdaptivePaginator::calculate_optimal_page_size(
    int max_tokens, const nlohmann::json& sample,
    const std::string& output_mode) const {
    int tokens_per_result = kPaginationBaseTokens;
    if (!sample.is_null()) {
        tokens_per_result = estimator_.estimate_tokens(sample);
        if (tokens_per_result < 1) tokens_per_result = 1;
    }

    int available =
        static_cast<int>(static_cast<double>(max_tokens) *
                         config_.token_safety_margin) -
        kPaginationMetadataTokens;
    if (available < 1) available = 1;

    int optimal = available / tokens_per_result;

    optimal = std::max(optimal, config_.min_page_size);
    optimal = std::min(optimal, config_.max_page_size);

    if (config_.smart_limit_enabled) {
        int limit = smart_result_limit(output_mode);
        optimal = std::min(optimal, limit);
    }

    return optimal;
}

PaginationResult AdaptivePaginator::apply_pagination(
    const nlohmann::json& results, int max_results, int total_count,
    const std::string& query, double time_ms,
    const std::string& output_mode) const {
    int max_tokens = kDefaultMaxTokens;
    if (output_mode == "single-line") {
        max_tokens = 4000;
    } else if (output_mode == "full") {
        max_tokens = 12000;
    }

    if (!results.is_array()) {
        return PaginationResult{query, time_ms, 0, 0, 0, total_count,
                                false, 0, max_tokens, false, results};
    }

    int result_count = static_cast<int>(results.size());
    if (result_count == 0) {
        return PaginationResult{query, time_ms, 0, 0, 0, total_count,
                                false, 0, max_tokens, false, results};
    }

    int page_size = max_results;
    if (page_size <= 0) {
        nlohmann::json sample = results[0];
        page_size = calculate_optimal_page_size(max_tokens, sample,
                                                 output_mode);
    }

    int end = std::min(page_size, result_count);
    bool has_more = end < result_count;
    nlohmann::json page_results = nlohmann::json::array();
    for (int i = 0; i < end; ++i) {
        page_results.push_back(results[i]);
    }

    auto [truncated, was_truncated] =
        truncate_by_tokens(page_results, max_tokens);
    has_more = has_more || was_truncated;

    int token_count = kPaginationMetadataTokens;
    for (const auto& r : truncated) {
        token_count += estimator_.estimate_tokens(r);
    }

    PaginationResult pr;
    pr.query = query;
    pr.time_ms = time_ms;
    pr.page = 0;
    pr.page_size = page_size;
    pr.count = static_cast<int>(truncated.size());
    pr.total_count = total_count;
    pr.has_more = has_more;
    pr.token_count = token_count;
    pr.max_tokens = max_tokens;
    pr.enhanced = (output_mode != "single-line");
    pr.results = truncated;

    if (has_more) {
        pr.next_page = 1;
    }

    nlohmann::json sample_for_suggestion =
        truncated.empty() ? nlohmann::json(nullptr) : truncated[0];
    pr.suggested_page_size = calculate_optimal_page_size(
        max_tokens, sample_for_suggestion, output_mode);

    return pr;
}

nlohmann::json AdaptivePaginator::group_results(
    const nlohmann::json& results,
    const std::string& group_by) const {
    nlohmann::json grouped = nlohmann::json::object();
    if (!results.is_array()) return grouped;

    for (const auto& r : results) {
        std::string key;
        if (group_by == "file" && r.contains("file")) {
            key = r["file"].get<std::string>();
        } else if (group_by == "directory" && r.contains("file")) {
            auto path = r["file"].get<std::string>();
            auto pos = path.rfind('/');
            key = (pos != std::string::npos) ? path.substr(0, pos) : "root";
        } else {
            continue;
        }

        if (!grouped.contains(key)) {
            grouped[key] = {{"count", 0}, {"results", nlohmann::json::array()}};
        }
        grouped[key]["count"] = grouped[key]["count"].get<int>() + 1;
        grouped[key]["results"].push_back(r);
    }

    return grouped;
}

nlohmann::json AdaptivePaginator::generate_summary(
    const nlohmann::json& results) const {
    nlohmann::json summary;
    if (!results.is_array()) return summary;

    std::unordered_map<std::string, bool> files;
    std::unordered_map<std::string, int> directories;

    for (const auto& r : results) {
        if (r.contains("file")) {
            auto path = r["file"].get<std::string>();
            files[path] = true;
            auto pos = path.rfind('/');
            auto dir = (pos != std::string::npos) ? path.substr(0, pos) : "root";
            directories[dir]++;
        }
    }

    summary["total_matches"] = results.size();
    summary["unique_files"] = files.size();
    summary["directories"] = directories;

    return summary;
}

std::pair<nlohmann::json, bool> AdaptivePaginator::truncate_by_tokens(
    const nlohmann::json& results, int max_tokens) const {
    int token_count = kPaginationMetadataTokens;
    nlohmann::json truncated = nlohmann::json::array();

    int total = static_cast<int>(results.size());
    int min_results = std::min(kMinGuaranteedResults, total);

    for (int i = 0; i < total; ++i) {
        int result_tokens = estimator_.estimate_tokens(results[i]);

        if (i < min_results) {
            truncated.push_back(results[i]);
            token_count += result_tokens;
            continue;
        }

        if (token_count + result_tokens > max_tokens) {
            return {truncated, true};
        }
        truncated.push_back(results[i]);
        token_count += result_tokens;
    }

    return {truncated, false};
}

int AdaptivePaginator::smart_result_limit(
    const std::string& output_mode) const {
    if (output_mode == "single-line" || output_mode == "files" ||
        output_mode == "count") {
        return 20;
    }
    return 10;
}

}  // namespace mcp
}  // namespace lci
