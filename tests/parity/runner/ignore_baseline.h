// tests/parity/runner/ignore_baseline.h
//
// BASE_IGNORE_NON_DETERMINISM — the canonical set of JSON paths that are
// non-deterministic across runs / processes / wall-clock regardless of
// which lci surface (cli/mcp/http/index) produced the output:
//
//   * pid, start_time, uptime_ms, elapsed_ms      — per-process runtime
//   * version, schema_version, build_id           — build metadata
//   * request_id, timestamp                       — per-request volatile
//   * id, jsonrpc                                 — JSON-RPC envelope echo
//   * result.meta.elapsed_ms, result.meta.server_pid — MCP envelope meta
//
// Every JSON-mode descriptor implicitly inherits these in its
// `tiers.ignore` set when canonicalizing for diff.  Per-descriptor
// `tiers.ignore` only needs to list descriptor-specific drift (e.g.
// `total_size_bytes`, `progress.*`, schema-divergence fields).
//
// Perf note (karpathy-principles §1, §"O(1) hash set"):
// merged_ignore() builds the union via std::unordered_set so dedup is
// O(1) per element.  This runs once per descriptor at runner startup,
// not per-field per-result, but we keep the contract anyway because the
// parity_unit_test also calls merged_ignore() across 83 descriptors.

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lci::parity {

// Authoritative list.  Update only when a new non-determinism field
// appears that genuinely applies to every surface.  Per-descriptor
// schema drift goes in the descriptor's own tiers.ignore, NOT here.
inline constexpr std::array<std::string_view, 12> BASE_IGNORE_NON_DETERMINISM = {
    // Per-process runtime
    "pid",
    "start_time",
    "uptime_ms",
    "elapsed_ms",
    // Per-request volatile
    "request_id",
    "timestamp",
    // Build metadata
    "version",
    "schema_version",
    // JSON-RPC envelope (MCP)
    "id",
    "jsonrpc",
    // MCP result.meta envelope
    "result.meta.elapsed_ms",
    "result.meta.server_pid",
};

// O(1) membership check against BASE.
inline bool is_base_ignore(std::string_view path) {
    // 12 entries — linear scan is faster than building/hashing a set
    // for such small N (cache hits, branch predictor friendly). The
    // unordered_set form below is used for the merge path where we
    // hash N descriptor entries against BASE.
    for (auto p : BASE_IGNORE_NON_DETERMINISM) {
        if (p == path) return true;
    }
    return false;
}

// Build the effective ignore set for a descriptor: BASE ∪ descriptor's
// own ignore list.  Order: BASE entries first (stable across descriptors,
// helps cache locality in path_in linear scan), then descriptor extras
// in original order.  Duplicates de-duped via unordered_set (O(1) lookup).
inline std::vector<std::string>
merged_ignore(const std::vector<std::string>& descriptor_ignore) {
    std::vector<std::string> out;
    out.reserve(BASE_IGNORE_NON_DETERMINISM.size() + descriptor_ignore.size());

    std::unordered_set<std::string_view> seen;
    seen.reserve(BASE_IGNORE_NON_DETERMINISM.size() + descriptor_ignore.size());

    for (auto p : BASE_IGNORE_NON_DETERMINISM) {
        out.emplace_back(p);
        seen.insert(out.back());
    }
    for (const auto& p : descriptor_ignore) {
        if (seen.insert(std::string_view(p)).second) {
            out.push_back(p);
        }
    }
    return out;
}

} // namespace lci::parity
