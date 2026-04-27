#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace lci::parity {

struct CanonicalizeOptions {
    // JSONPath-lite expressions to strip before comparison.
    // Supports literal field paths like "results[].file" or "server_pid".
    std::vector<std::string> ignore_paths;

    // Paths whose numeric values must be preserved as numbers (not
    // stringified). Typically the union of ranked + timed paths from
    // the descriptor's tier map, so the diff engine can apply tolerances.
    std::vector<std::string> preserve_number_paths;

    // Absolute corpus prefix to rewrite to "${CORPUS}".
    // Empty string disables rewriting.
    std::string corpus_prefix;
};

// Canonicalize a JSON value:
//   - Object keys recursively sorted (handled implicitly by nlohmann::json::dump
//     with no flags — but we walk and re-emit explicitly to be deterministic).
//   - Floats normalized to "%.6g" string form (stored as JSON string).
//   - String values inside objects/arrays get corpus-prefix rewrite if non-empty.
//   - JSONPath-lite ignore_paths are stripped.
nlohmann::json canonicalize_json(const nlohmann::json& in,
                                 const CanonicalizeOptions& opts);

// Canonicalize plain text: trim trailing whitespace per line. Preserve the
// trailing newline policy of the input (no newline added or removed at EOF).
std::string canonicalize_text(std::string_view in);

} // namespace lci::parity
