#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include <lci/context_manifest.h>
#include <lci/mcp/server.h>

namespace lci {

class MasterIndex;

namespace mcp {

/// Registers the context manifest tool handler on the given server,
/// replacing the stub handler with a real implementation.
void register_context_handlers(McpServer& server, MasterIndex* indexer);

// -- Handler functions (exposed for testing) ----------------------------------

/// Handles the "context" tool: save and load context manifests.
/// Dispatches to save or load based on the "operation" parameter.
ToolResult handle_context(const nlohmann::json& params,
                          MasterIndex& indexer,
                          const std::string& project_root);

// -- JSON serialization helpers (exposed for testing) -------------------------

/// Serializes a ContextManifest to JSON.
nlohmann::json manifest_to_json(const ContextManifest& manifest);

/// Deserializes a ContextManifest from JSON. Returns empty error on success.
std::string manifest_from_json(const nlohmann::json& j,
                               ContextManifest& out);

/// Validates a ContextManifest. Returns empty string if valid.
std::string validate_manifest(const ContextManifest& manifest);

/// Computes stats for a manifest.
ManifestStats compute_manifest_stats(const ContextManifest& manifest);

/// Serializes a HydratedContext to JSON.
nlohmann::json hydrated_context_to_json(const HydratedContext& ctx);

}  // namespace mcp
}  // namespace lci
