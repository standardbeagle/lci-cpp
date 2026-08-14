#include "../../src/cli/query_parser.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

/// libFuzzer target: the `lci search` advanced-query parser plus the
/// file:-directive glob matcher. Query strings arrive verbatim from the
/// command line and from MCP clients, so tokenization (file:/kind:/symbol:
/// directives, `-` exclusions) and the backtracking glob matcher must never
/// crash or hang on arbitrary bytes. The input is split at the first NUL:
/// the left half fuzzes parse(), and the halves together fuzz
/// path_matches_glob(pattern, path) so the matcher's `*` backtracking sees
/// adversarial pattern/text pairs.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 4096) {
        return 0;
    }

    std::string_view whole(reinterpret_cast<const char*>(data), size);

    auto parsed = lci::cli::query_parser::parse(whole);
    // parse() guards against pushing empty directive values; an empty entry
    // would later match everything (kind:) or nothing (symbol:) silently.
    for (const auto& k : parsed.kinds) {
        if (k.empty()) std::abort();
    }
    for (const auto& s : parsed.symbols) {
        if (s.empty()) std::abort();
    }

    const size_t split = whole.find('\0');
    if (split != std::string_view::npos) {
        std::string_view pattern = whole.substr(0, split);
        std::string_view path = whole.substr(split + 1);
        (void)lci::cli::query_parser::path_matches_glob(pattern, path);
        (void)lci::cli::query_parser::glob_match(pattern, path);

        // Correctness properties of the matcher, checked on every input:
        // a metachar-free pattern is exact equality, and "*" matches all.
        if (pattern.find('*') == std::string_view::npos &&
            pattern.find('?') == std::string_view::npos) {
            if (lci::cli::query_parser::glob_match(pattern, path) !=
                (pattern == path)) {
                std::abort();
            }
        }
        if (!lci::cli::query_parser::glob_match("*", path)) std::abort();
        // Matching is reflexive for patterns without metachars.
        if (pattern.find('*') == std::string_view::npos &&
            pattern.find('?') == std::string_view::npos &&
            !lci::cli::query_parser::glob_match(pattern, pattern)) {
            std::abort();
        }
    }

    return 0;
}
