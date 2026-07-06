#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <lci/core/reference_tracker.h>
#include <lci/mcp/server.h>

namespace lci {

class MasterIndex;
class SearchEngine;
class SideEffectAnalyzer;

namespace mcp {

/// Fuzzy near-miss symbol suggestions for empty lookups (search,
/// get_context, inspect_symbol). Substring containment scores 0.9; otherwise
/// normalized Levenshtein similarity, threshold 0.7. Cold path — call only
/// after a query already returned nothing. Returns up to 3 {name, id}
/// entries (may be empty).
nlohmann::json similar_symbol_suggestions(
    const ReferenceTracker::Snapshot& rt_snap, const std::string& query);

/// Registers the 4 core tool handlers (search, get_context, info, find_files)
/// on the given server, replacing stub handlers with real implementations.
///
/// Requires that the server was constructed with a valid MasterIndex and
/// optionally a SearchEngine. If the index is null, handlers return errors.
/// `analyzer` (optional) supplies side-effect/purity data to get_context; when
/// null, purity is omitted (Go nil-propagator parity).
void register_core_handlers(McpServer& server, MasterIndex* indexer,
                            SearchEngine* search_engine,
                            SideEffectAnalyzer* analyzer = nullptr);

// -- Handler functions (exposed for testing) ----------------------------------

/// Handles the "info" tool: returns help text for a specific tool or
/// a general overview of all available tools.
ToolResult handle_info(const nlohmann::json& params);

/// Handles the "search" tool: queries the index for pattern matches
/// and returns results with optional context lines.
ToolResult handle_search(const nlohmann::json& params,
                         MasterIndex& indexer,
                         SearchEngine* search_engine);

/// Handles the "get_context" tool: returns call hierarchy and symbol
/// details for a given symbol name or file+line location. When `analyzer` is
/// non-null, function/method contexts gain a `purity` block (Go getPurityInfo).
ToolResult handle_get_context(const nlohmann::json& params,
                              MasterIndex& indexer,
                              const SideEffectAnalyzer* analyzer = nullptr);

/// Handles the "find_files" tool: searches file paths in the index
/// using substring and case-insensitive matching.
ToolResult handle_find_files(const nlohmann::json& params,
                             MasterIndex& indexer);

}  // namespace mcp
}  // namespace lci
