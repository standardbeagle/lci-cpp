#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
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

    // Array paths whose elements should be sorted (by their canonical
    // JSON dump) before comparison. Used when the producer is allowed
    // to emit elements in any order — e.g. Go's map-iteration order in
    // /list-symbols, /search, /references — and the parity test should
    // verify multiset content rather than positional equality. Path
    // syntax matches `ignore_paths` (e.g. "symbols", "results").
    std::vector<std::string> sort_array_paths;

    // Absolute corpus prefix to rewrite to "${CORPUS}".
    // Empty string disables rewriting.
    std::string corpus_prefix;
};

// Options controlling text-mode normalization.  The defaults always trim
// trailing whitespace and scrub timing tokens, since both are universally
// safe for text-mode parity comparison.  Per-descriptor knobs let the
// runner enable path rewriting and surgical line/regex strips so that two
// binaries emitting semantically equivalent text — but with cosmetic
// divergence (debug preambles, banner mode strings, leading emoji) — can
// be compared without weakening the failure signal.
struct TextCanonicalizeOptions {
    // Replace any occurrence of "<n>ms" or "<n.n>ms" with "<MS>".
    // Default true: timing values are non-deterministic and cannot be
    // compared verbatim.
    bool scrub_timing = true;

    // Rewrite occurrences of `corpus_prefix` (and its trailing "/") to
    // "${CORPUS}" within text output.  Empty string disables rewriting.
    std::string corpus_prefix;

    // Drop any line whose content contains one of these substrings.
    // Used to strip DEBUG/verbose preamble lines, intermediate-progress
    // chatter, and section headers that exist on only one side.
    std::vector<std::string> strip_lines;

    // Strip a leading emoji + (optional) variation-selector + whitespace
    // from each line.  Catches Go-side prefixes like "✅ ", "📍 ",
    // "📊 ", "⚠️  ", and "✓ " that C++ does not emit.
    bool strip_emoji_prefix = false;

    // Per-descriptor regex replacements applied to each line in order.
    // Each pair is {ECMAScript regex pattern, replacement template}.
    // Used to collapse banner/mode strings such as
    // "(integrated mode - no assembly matches)" and "(standard mode)"
    // to a common token like "(MODE)".
    std::vector<std::pair<std::string, std::string>> replace;
};

// Canonicalize a JSON value:
//   - Object keys recursively sorted (handled implicitly by nlohmann::json::dump
//     with no flags — but we walk and re-emit explicitly to be deterministic).
//   - Floats normalized to "%.6g" string form (stored as JSON string).
//   - String values inside objects/arrays get corpus-prefix rewrite if non-empty.
//   - JSONPath-lite ignore_paths are stripped.
nlohmann::json canonicalize_json(const nlohmann::json& in,
                                 const CanonicalizeOptions& opts);

// Canonicalize plain text:
//   - Trim trailing whitespace per line (always).
//   - Apply optional timing scrub, corpus-prefix rewrite, line strip,
//     emoji-prefix strip, and regex replacements.
//   - Preserve the trailing newline policy of the input.
std::string canonicalize_text(std::string_view in,
                              const TextCanonicalizeOptions& opts);

} // namespace lci::parity
