#pragma once

#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Svelte single-file components: HTML markup wrapping one or two <script>
// blocks (instance + context="module") whose content is plain JS or TS.
//
// LCI has no dedicated Svelte grammar. Instead the component is reduced to
// its script code by masking: every byte OUTSIDE the <script> block bodies
// is replaced with a space (newlines/carriage returns preserved), so the
// masked buffer has the exact byte/line/column geometry of the original
// file and parses with the existing tree-sitter JS/TS grammar. All symbol
// and reference positions therefore point at the real .svelte file.
//
// This deliberately skips markup expressions ({...}), style blocks, and
// component tags — the script block carries the functions, props, stores,
// and imports that navigation needs. See the port-choice rationale in the
// introducing commit.
// ---------------------------------------------------------------------------

namespace lci::parser {

struct SvelteScriptInfo {
    /// At least one <script> block was found.
    bool has_script = false;
    /// Any script block declares lang="ts" / lang="typescript".
    /// The whole file is then parsed with the TypeScript grammar.
    bool typescript = false;
};

/// Masks everything outside <script>...</script> content with spaces into
/// `masked` (same size as `content`, newlines preserved) so the result can
/// be parsed by the JS/TS grammar with positions valid against the
/// original file. Tag matching is case-insensitive and quote-aware inside
/// the open tag. With no script block the whole buffer is blanked.
SvelteScriptInfo mask_svelte_script(std::string_view content,
                                    std::string& masked);

}  // namespace lci::parser
