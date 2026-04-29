// tests/lib/spec_diff/include/spec_diff/canonicalize.h
//
// Canonicalization pipeline for both structured (JSON) and unstructured
// (text) outputs prior to spec_diff comparison. Pure functions: data in,
// canonical form out. No subprocess, no binary paths, no parity-runner
// concepts.
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spec_diff {

struct CanonicalizeOptions {
    // JSONPath-lite expressions to strip before comparison.
    // Supports literal field paths like "results[].file" or "server_pid".
    std::vector<std::string> ignore_paths;

    // Paths whose numeric values must be preserved as numbers (not
    // stringified). Typically the union of ranked + timed paths from the
    // tier map, so the diff engine can apply tolerances.
    std::vector<std::string> preserve_number_paths;

    // Array paths whose elements should be sorted (by their canonical
    // JSON dump) before comparison. Used when the producer is allowed
    // to emit elements in any order and the diff should verify multiset
    // content rather than positional equality. Path syntax matches
    // `ignore_paths` (e.g. "symbols", "results").
    std::vector<std::string> sort_array_paths;

    // Absolute corpus prefix to rewrite to "${CORPUS}".
    // Empty string disables rewriting.
    std::string corpus_prefix;
};

// Options controlling text-mode normalization.  The defaults always trim
// trailing whitespace and scrub timing tokens, since both are universally
// safe for parity comparison.  Per-spec knobs let callers enable path
// rewriting and surgical line/regex strips so that two producers emitting
// semantically equivalent text — but with cosmetic divergence (debug
// preambles, banner mode strings, leading emoji) — can be compared
// without weakening the failure signal.
struct TextCanonicalizeOptions {
    // Replace any occurrence of "<n>ms" or "<n.n>ms" with "<MS>".
    // Default true: timing values are non-deterministic.
    bool scrub_timing = true;

    // Rewrite occurrences of `corpus_prefix` (and its trailing "/") to
    // "${CORPUS}" within text output.  Empty string disables rewriting.
    std::string corpus_prefix;

    // Drop any line whose content contains one of these substrings.
    // Used to strip DEBUG/verbose preamble lines, intermediate-progress
    // chatter, and section headers that exist on only one side.
    std::vector<std::string> strip_lines;

    // Strip a leading emoji + (optional) variation-selector + whitespace
    // from each line.
    bool strip_emoji_prefix = false;

    // Per-spec regex replacements applied to each line in order.
    // Each pair is {ECMAScript regex pattern, replacement template}.
    std::vector<std::pair<std::string, std::string>> replace;
};

// Canonicalize a JSON value:
//   - Object keys recursively sorted.
//   - Floats normalized to "%.6g" string form (stored as JSON string)
//     unless their path is in preserve_number_paths.
//   - Strings get corpus-prefix rewrite if non-empty.
//   - JSONPath-lite ignore_paths are stripped.
//   - sort_array_paths sorted last so the canonical dump is stable.
nlohmann::json canonicalize_json(const nlohmann::json& in,
                                 const CanonicalizeOptions& opts);

// Canonicalize plain text:
//   - Trim trailing whitespace per line (always).
//   - Apply optional timing scrub, corpus-prefix rewrite, line strip,
//     emoji-prefix strip, and regex replacements.
//   - Preserve the trailing newline policy of the input.
std::string canonicalize_text(std::string_view in,
                              const TextCanonicalizeOptions& opts);

// Convenience wrapper: drop any line whose content contains one of the
// strip patterns, while leaving every other text feature intact (no
// timing scrub, no path rewrite, no emoji strip, no regex). Useful for
// pre-cleaning stdout BEFORE structured JSON parsing when one producer
// emits debug preamble lines that the other does not. No-op when
// patterns is empty.
std::string strip_preamble_lines(const std::string& in,
                                 const std::vector<std::string>& patterns);

} // namespace spec_diff
