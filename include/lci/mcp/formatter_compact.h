#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci {
namespace mcp {

/// Produces ultra-compact LCF (LCI Compact Format) text from JSON responses.
///
/// The compact format uses single-character field codes (o=, t=, n=, s=, e=)
/// to minimise token usage when communicating with LLMs.
class CompactFormatter {
  public:
    bool include_context{false};
    bool include_metadata{false};
    bool include_breadcrumbs{false};

    /// Formats a search response (results array + totals).
    /// Input JSON: { "results": [...], "total_matches", "showing", "max_results" }
    std::string format_search_response(const nlohmann::json& response) const;

    /// Formats a files-only response.
    /// Input JSON: { "files": [...], "total_matches", "unique_files" }
    std::string format_files_only_response(const nlohmann::json& response) const;

    /// Formats a count-only response.
    /// Input JSON: { "total_matches", "unique_files" }
    std::string format_count_only_response(const nlohmann::json& response) const;

    /// Formats a context response (object contexts array).
    /// Input JSON: { "contexts": [...], "count" }
    std::string format_context_response(const nlohmann::json& response) const;

  private:
    std::string format_search_result(const nlohmann::json& result) const;
    std::string format_object_context(const nlohmann::json& ctx) const;
    std::string format_metadata(const nlohmann::json& result) const;
};

}  // namespace mcp
}  // namespace lci
