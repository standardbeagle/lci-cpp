#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include <lci/mcp/server.h>

namespace lci {

class MasterIndex;

namespace mcp {

/// Registers the 3 index management tool handlers (index_stats, debug_info,
/// git_analysis) on the given server, replacing stub handlers.
///
/// Requires a valid MasterIndex. Handlers return errors if index is null.
void register_index_handlers(McpServer& server, MasterIndex* indexer);

// -- Handler functions (exposed for testing) ----------------------------------

/// Handles "index_stats": index statistics and health monitoring.
ToolResult handle_index_stats(const nlohmann::json& params,
                              MasterIndex& indexer);

/// Handles "debug_info": diagnostic information about indexed data.
ToolResult handle_debug_info(const nlohmann::json& params,
                             MasterIndex& indexer);

/// Handles "git_analysis": real git-change analysis against the live index.
/// Builds a git::Provider from the project root, runs git::Analyzer, and emits
/// the canonical report shape (summary/metrics_issues/.../metadata). Fails fast
/// with an error response when the project root is not a git repository.
ToolResult handle_git_analysis(const nlohmann::json& params,
                               MasterIndex& indexer);

}  // namespace mcp
}  // namespace lci
