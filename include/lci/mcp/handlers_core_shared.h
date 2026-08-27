#pragma once

// Small helpers shared by the core MCP tool handlers (search, get_context,
// find_files, info) after their split into per-tool modules. Internal to
// src/mcp — not part of the public tool interface.

#include <algorithm>
#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

#include <lci/core/reference_tracker.h>

namespace lci {
namespace mcp {

inline int clamp_int(int value, int min_val, int max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// True if the comma-separated `list` contains `item` (trimmed, exact).
bool comma_list_contains(const std::string& list, const std::string& item);

/// Fuzzy near-miss suggestions for an empty symbol lookup (cold path).
nlohmann::json similar_symbol_suggestions(
    const ReferenceTracker::Snapshot& rt_snap, const std::string& query);

/// get_context param canonicalization, shared with tool registration.
bool normalize_context_params(nlohmann::json& params);
void apply_context_lookup_mode(nlohmann::json& params);

}  // namespace mcp
}  // namespace lci
