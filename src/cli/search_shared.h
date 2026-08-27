#pragma once

// Internals shared between run_search (search.cpp) and run_grep (grep.cpp)
// after the per-command split. Not part of any public API.

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <lci/cli/commands.h>

#include <nlohmann/json.hpp>

namespace lci {

class Client;

namespace cli {

/// Runs the positional pattern OR'd with each --patterns entry and merges.
std::optional<nlohmann::json> search_union_patterns(
    Client& client, const std::vector<std::string>& patterns, int max_results,
    bool case_insensitive, std::string& error,
    const std::vector<std::string>& paths = {});

/// Renders search/grep results (text or --json) and returns the exit code.
int render_search_output(const SearchCommandOptions& options,
                         nlohmann::json& response, double elapsed_ms,
                         std::chrono::steady_clock::time_point verbose_start);

}  // namespace cli
}  // namespace lci
