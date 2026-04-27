#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <lci/mcp/server.h>

namespace lci {

class MasterIndex;

namespace mcp {

/// Registers the 3 explore tool handlers (list_symbols, inspect_symbol,
/// browse_file) on the given server, replacing stub handlers.
///
/// Requires a valid MasterIndex. Handlers return errors if index is null.
void register_explore_handlers(McpServer& server, MasterIndex* indexer);

// -- Handler functions (exposed for testing) ----------------------------------

/// Handles "list_symbols": paginated symbol listing with type/language filters.
ToolResult handle_list_symbols(const nlohmann::json& params,
                               MasterIndex& indexer);

/// Handles "inspect_symbol": detailed symbol info with references and call graph.
ToolResult handle_inspect_symbol(const nlohmann::json& params,
                                 MasterIndex& indexer);

/// Handles "browse_file": file content with symbol annotations.
ToolResult handle_browse_file(const nlohmann::json& params,
                              MasterIndex& indexer);

}  // namespace mcp
}  // namespace lci
