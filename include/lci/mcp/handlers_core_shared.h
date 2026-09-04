#pragma once

// Small helpers shared by the core MCP tool handlers (search, get_context,
// find_files, info) after their split into per-tool modules. Internal to
// src/mcp — not part of the public tool interface.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

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

/// Language-name → file-extension table (lowercase keys + aliases), shared
/// by search's `languages[]` / `filter` translation and find_files' `filter`.
/// Defined in handlers_search.cpp.
const std::map<std::string, std::vector<std::string>>& language_ext_table();

/// True if the comma-separated `list` contains `item` (trimmed, exact).
bool comma_list_contains(const std::string& list, const std::string& item);

/// Fuzzy near-miss suggestions for an empty symbol lookup (cold path).
nlohmann::json similar_symbol_suggestions(
    const ReferenceTracker::Snapshot& rt_snap, const std::string& query);

/// True when a caller-supplied path scope is absolute, in either spelling.
///
/// Both tests are needed. std::filesystem::path::is_absolute() is false on
/// Windows for "/foo" — no drive letter makes it root-relative to the current
/// drive, not absolute — so a POSIX-spelled scope would slip through as a
/// relative path and match nothing instead of erroring. A bare front()=='/'
/// misses "C:/repo". Scopes reach this already folded to '/'.
inline bool is_absolute_scope(const std::string& p) {
    if (p.empty()) return false;
    if (p.front() == '/') return true;
    return std::filesystem::path(p).is_absolute();
}

/// get_context param canonicalization, shared with tool registration.
bool normalize_context_params(nlohmann::json& params);
void apply_context_lookup_mode(nlohmann::json& params);

}  // namespace mcp
}  // namespace lci
