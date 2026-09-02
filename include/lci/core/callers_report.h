#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <lci/core/reference_tracker.h>

namespace lci {

/// Builds the shared callers-of JSON report consumed by the /callers HTTP
/// endpoint (CLI `lci callers`) and the MCP `callers` tool. One builder so
/// the two surfaces cannot drift.
///
/// Shape:
///   {
///     "symbol": "<name>",
///     "definitions": [{"name","type","file_path","line"}...],
///     "callers": [{"caller","caller_type","file_path","line",
///                  "call_lines":[...], "call_count":N}...],
///     "total_callers": N, "total_call_sites": N, "truncated": bool,
///     "dynamic_call_sites":    [{"caller","file_path","line"}...],
///     "unresolved_call_sites": [{"caller","file_path","line"}...]
///   }
/// `callers` groups confirmed sites by enclosing symbol, sorted by
/// (file_path, line) — deterministic across machines. Dynamic/unresolved
/// sites are listed separately and NEVER counted among confirmed callers
/// (consistent with the dynamic-dispatch marking in code_insight).
/// `max_callers` caps the confirmed caller groups (<=0 = unlimited);
/// dynamic/unresolved lists are capped at the same bound.
/// `path_of` maps a FileID to the path to emit (typically root-relative).
nlohmann::json build_callers_report(
    const ReferenceTracker::Snapshot& snap, std::string_view name,
    int max_callers, const std::function<std::string(FileID)>& path_of);

}  // namespace lci
