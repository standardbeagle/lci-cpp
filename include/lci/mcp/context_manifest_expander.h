#pragma once

#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include <lci/context_manifest.h>
#include <lci/types.h>

namespace lci {

class MasterIndex;
class ReferenceTracker;
class SideEffectAnalyzer;
struct EnhancedSymbol;

namespace mcp {

/// Metadata extracted from a symbol lookup.
struct SymbolInfo {
    std::string symbol_type;
    std::string signature;
    bool is_exported{};
    int start_line{};
    int end_line{};
};

/// Parses an expansion directive like "callers:2" into type and depth.
/// Returns the directive type and depth (default 1).
std::pair<std::string, int> parse_expansion_directive(std::string_view directive);

/// Validates an expansion directive. Returns an empty string when the
/// directive names a supported expansion (with a well-formed, in-range
/// optional depth); otherwise a human-readable reason. Unknown types,
/// malformed depths (empty/non-integer/less than 1), a depth on a directive
/// that does not walk depth, and a depth beyond a direct-only directive's
/// single hop are all refused — never silently defaulted or skipped.
std::string expansion_directive_error(std::string_view directive);

/// Per-load traversal accounting for one hydration working set. Owned by the
/// loader (the MCP `context` handler, and later the CLI/composites) and threaded
/// through every expansion so identity deduplication and traversal bounds are
/// enforced ONCE across the whole set — primaries plus every expansion — rather
/// than per directive. The token budget is NOT here: it is the single
/// serialized-JSON budget owned by the caller.
struct ExpansionTally {
    /// Canonical identity keys already present in the working set (a primary or
    /// an earlier expansion). An expansion target matching one is skipped — its
    /// source is never repeated — and recorded in `deduped`.
    absl::flat_hash_set<std::string>* emitted_keys = nullptr;
    /// Clamps every directive's requested depth. INT_MAX for an unlimited load.
    int max_depth = std::numeric_limits<int>::max();
    /// Caps total NEW expansion targets hydrated across the whole load.
    int visited_cap = std::numeric_limits<int>::max();
    int visited_used = 0;
    /// Set when a directive's depth was clamped or the visited cap was reached.
    bool traversal_truncated = false;
    /// The directive type currently being applied (e.g. "callers"), read by the
    /// shared hydrate helper to label a deduplicated target's relationship.
    std::string relationship;
    /// Targets whose identity was already emitted: (canonical key, relationship
    /// directive, e.g. "callers"). The caller folds the relationship into the
    /// surviving entry's provenance instead of repeating the source.
    std::vector<std::pair<std::string, std::string>> deduped;
};

/// Resolves context references into hydrated source code.
///
/// Given a ContextRef (file + symbol + optional line range), loads the actual
/// source code, resolves symbol definitions, and applies expansion directives
/// (callers, callees, implementations, etc.).
///
/// Thread safety: not thread-safe. Intended for single-threaded use within
/// an MCP handler.
class ExpansionEngine {
  public:
    /// Creates an engine backed by the given index for symbol resolution.
    explicit ExpansionEngine(MasterIndex& index);

    /// Canonical identity key for a hydrated ref: the resolved file path
    /// (normalized against `project_root` so a relative primary path and the
    /// absolute path an expansion resolves to collapse together) plus the
    /// resolved symbol and line range. Two refs naming the same file+symbol, or
    /// an expansion landing on an already-admitted identity, share a key and so
    /// hydrate their source exactly once across the working set.
    std::string identity_key(const HydratedRef& hr,
                             const std::string& project_root) const;

    /// Hydrates a single reference into source code.
    /// Returns the hydrated ref, approximate token count, error string
    /// (empty on success), and the identity-resolution outcome.
    struct HydrateResult {
        HydratedRef ref;
        int tokens{};
        std::string error;
        RefResolution reason{RefResolution::Resolved};
    };
    HydrateResult hydrate_reference(const ContextRef& ref, FormatType format,
                                    const std::string& project_root);

    /// Hydrates a reference already identified by its SymbolID, using that
    /// symbol's own current file+line range. Identity is exact (never re-
    /// resolved by name), so an expansion target cannot be substituted by, or
    /// made ambiguous against, a same-name sibling.
    HydrateResult hydrate_symbol_id(SymbolID id, FormatType format);

    /// Applies expansion directives (callers, callees, etc.) to a reference.
    /// Returns the newly-emitted hydrated expansion refs (identities not
    /// already in `tally.emitted_keys`) and the directives that were refused.
    /// Deduplication and traversal bounds are applied through the shared
    /// `tally`; the token budget is owned by the caller's serialized-JSON
    /// accounting.
    struct DirectiveError {
        std::string directive;
        std::string reason;
    };
    struct ExpansionResult {
        std::vector<HydratedRef> expanded;
        std::vector<DirectiveError> errors;
    };
    ExpansionResult apply_expansions(const ContextRef& ref,
                                     HydratedRef& hydrated,
                                     FormatType format,
                                     const std::string& project_root,
                                     ExpansionTally& tally);

    /// Wires the shared SideEffectAnalyzer used by the `side_effects`
    /// directive. Null means no analyzer is available, and a requested
    /// `side_effects` expansion reports evidence as unavailable rather than
    /// pure. Never takes ownership.
    void set_side_effect_analyzer(const SideEffectAnalyzer* analyzer) {
        analyzer_ = analyzer;
    }

  private:
    MasterIndex& index_;
    const SideEffectAnalyzer* analyzer_{};

    /// Resolves a saved file+symbol to exactly one symbol in the named file.
    /// Never substitutes a same-name symbol from another file, and reports a
    /// same-file overload set as ambiguous rather than picking the first
    /// candidate. Empty `file_path` falls back to a global name match
    /// (legacy symbol-only refs); a same-name collision then resolves to the
    /// first indexed candidate by (file, line) order.
    struct ResolveStart {
        SymbolID id{};
        RefResolution status{RefResolution::Resolved};
    };
    ResolveStart resolve_start(const std::string& file_path,
                               const std::string& symbol_name);

    /// Extracts source for a symbol resolved by identity within its file.
    struct ExtractResult {
        std::string source;
        LineRange lines;
        SymbolInfo info;
        std::string error;
        RefResolution reason{RefResolution::Resolved};
        SymbolID id{};
    };
    ExtractResult extract_symbol_source(const std::string& file_path,
                                        const std::string& symbol_name,
                                        FormatType format);

    /// Extracts source lines from a file by line range.
    struct LinesResult {
        std::string source;
        std::string error;
    };
    LinesResult extract_source_by_lines(const std::string& file_path,
                                         int start_line, int end_line);

    /// Resolves a file path relative to the project root.
    std::string resolve_path(const std::string& file,
                             const std::string& project_root) const;

    /// Gets the file path for a symbol's file ID.
    std::string get_file_path(FileID file_id);

    // -- Expansion methods ---------------------------------------------------

    std::vector<HydratedRef> expand_callers(
        const ContextRef& ref, int depth, const std::string& project_root,
        FormatType format, ExpansionTally& tally);

    std::vector<HydratedRef> expand_callees(
        const ContextRef& ref, int depth, const std::string& project_root,
        FormatType format, ExpansionTally& tally);

    /// Reference SITES of the exact selected symbol: each incoming reference
    /// resolves to the source location (file + line) of the enclosing symbol
    /// that references it, hydrated by that location and labelled with the
    /// reference kind. Reuses ReferenceTracker evidence; never a new analyzer.
    std::vector<HydratedRef> expand_references(
        const ContextRef& ref, const std::string& project_root,
        FormatType format, ExpansionTally& tally);

    /// Direct, explicitly typed dependencies of the exact selected symbol:
    /// its resolved outgoing calls and imports, hydrated by exact SymbolID
    /// and labelled with the reference type. Direct only by construction —
    /// there is no transitive walk here.
    std::vector<HydratedRef> expand_dependencies(
        const ContextRef& ref, const std::string& project_root,
        FormatType format, ExpansionTally& tally);

    /// Populates the (not-yet-serialized) purity state for a hydrated symbol:
    /// available evidence from the analyzer's record, otherwise an
    /// unavailability reason. Never claims pure on absence.
    void populate_purity(HydratedRef& hr, const EnhancedSymbol* sym);

    /// Marks a side_effects expansion requested. Preserves any purity state
    /// populated during hydration; a symbol-less ref is unavailable by name.
    void request_side_effects(HydratedRef& hr);

    std::vector<HydratedRef> expand_implementations(
        const ContextRef& ref, const std::string& project_root,
        FormatType format, ExpansionTally& tally);

    std::vector<HydratedRef> expand_interface(
        const ContextRef& ref, const std::string& project_root,
        FormatType format, ExpansionTally& tally);

    std::vector<HydratedRef> expand_siblings(
        const ContextRef& ref, const std::string& project_root,
        FormatType format, ExpansionTally& tally);

    /// Related tests by HEURISTIC match only: symbols named `Test<Symbol>` in a
    /// test-path file (`_test.`, `test_`), plus callers of the selected symbol
    /// whose name starts with `Test` in a test-path file. This is the current
    /// supported naming/caller rule, NOT a claim of complete test coverage; a
    /// project with a different test convention yields an empty result rather
    /// than a guess.
    std::vector<HydratedRef> expand_tests(
        const ContextRef& ref, const std::string& project_root,
        FormatType format, ExpansionTally& tally);

    /// Extracts only the doc comments from source.
    void extract_documentation(HydratedRef& ref);

    /// Replaces source with just the signature line.
    void extract_signature_only(HydratedRef& ref);
};

}  // namespace mcp
}  // namespace lci
