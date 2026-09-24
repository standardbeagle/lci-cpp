#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lci {

// -- Shared hydration budget --------------------------------------------------
//
// ONE budget governs a hydrated working set (the `context` loader and, later,
// the CLI/composites that reuse it). For a positive budget the charge is
// ceil(UTF-8 bytes of the final serialized context JSON / 4) — a byte-based
// ESTIMATE of context size, deliberately NOT a model-tokenizer count. A budget
// of 0 preserves the legacy unlimited behavior.

/// Default token budget for CLI/composite callers that hydrate a working set
/// under a bound. An MCP caller that omits max_tokens keeps unlimited (0).
constexpr int kHydrationDefaultMaxTokens = 8000;

/// Traversal bounds applied independently of the token budget, and only to a
/// bounded load (max_tokens > 0). They cap the request shape and the graph walk
/// so a hostile or oversized manifest cannot drive unbounded hydration.
constexpr int kHydrationMaxInputRefs = 128;
constexpr int kHydrationMaxExpansionDepth = 5;
constexpr int kHydrationMaxVisitedTargets = 512;

/// The shared token estimate: ceil(UTF-8 bytes / 4). An estimate of serialized
/// context size, not a model-tokenizer count.
inline int hydration_token_estimate(size_t utf8_bytes) {
    return static_cast<int>((utf8_bytes + 3) / 4);
}

/// A range of lines in a file (1-indexed, inclusive).
struct LineRange {
    int start{};
    int end{};
};

/// A compact reference to code with optional expansion directives.
struct ContextRef {
    std::string file;
    std::string symbol;
    LineRange line_range;
    bool has_line_range{};
    std::vector<std::string> expansions;
    std::string role;
    std::string note;
};

/// Statistics about a context manifest.
struct ManifestStats {
    int ref_count{};
    int total_lines{};
    int file_count{};
    int size_bytes{};
};

/// A compact, serializable representation of code context for transfer between
/// AI agent sessions. Stores symbol references, not source code.
struct ContextManifest {
    std::string task;
    std::string version;
    std::string project_root;
    std::vector<ContextRef> refs;
    ManifestStats stats;
};

/// Purity analysis summary for a hydrated reference.
///
/// `available` distinguishes real evidence from its absence: a record that
/// exists is available and carries `is_pure` (true or false); a ref with no
/// analysis — no analyzer wired, no function/method, or no record — is
/// available=false with `unavailability_reason` set, never a silent is_pure.
struct PurityInfo {
    bool is_pure{};
    std::string purity_level;
    std::vector<std::string> categories;
    std::vector<std::string> transitive_categories;
    double purity_score{};
    std::vector<std::string> reasons;
    bool available{};
    std::string unavailability_reason;
};

/// A single reference with resolved source code and expanded relationships.
struct HydratedRef {
    std::string file;
    std::string symbol;
    LineRange lines;
    std::string role;
    std::string note;
    std::string source;
    std::string symbol_type;
    std::string signature;
    bool is_exported{};
    bool is_generated{};
    bool is_external{};
    // A ref resolved purely from a literal line range (no symbol identity):
    // the line numbers are current-index positions and carry no semantic
    // stability guarantee across edits.
    bool is_line_range_literal{};
    // Roles / expansion relationships that resolved to this same canonical
    // identity. When several requested refs or an expansion target collapse
    // onto one hydrated source, the source is emitted once and every
    // requested role / relationship survives here — provenance is never lost
    // by deduplication. Empty when this entry was requested under a single
    // role and no expansion target collapsed onto it.
    std::vector<std::string> provenance;
    PurityInfo purity;
    bool has_purity{};
};

/// Statistics about the hydration process.
struct HydrationStats {
    int refs_loaded{};
    int symbols_hydrated{};
    int tokens_approx{};
    int expansions_applied{};
    int unresolved_count{};
    bool truncated{};
    // Primary refs (targeted source) omitted because the shared budget could
    // not admit them, counted separately from expansion refs omitted.
    int refs_omitted{};
    int expansions_omitted{};
    // Traversal (input-ref / depth / visited-target) was bounded, independent
    // of the token budget. Reported so a large graph is never silently clipped.
    bool traversal_truncated{};
    // Expansion directives refused as unknown / malformed / depth-exceeding.
    int expansion_errors{};
};

/// Outcome of resolving a reference's identity against the current index.
/// A saved file+symbol resolves only inside that file; a saved object id is
/// never persistent identity.
enum class RefResolution : uint8_t {
    Resolved = 0,
    InvalidRef,        // no file, symbol, or line range to select on, or a
                       // selector field of the wrong type
    MissingFile,       // named file is not in the index
    MissingSymbol,     // file present, symbol absent (never cross-file match)
    AmbiguousSymbol,   // >1 same-name symbol in the named file
    UnsupportedVersion,  // manifest schema version is neither empty nor 1.0
    FileNotIndexed,    // named file exists on disk but was never indexed
                       // (excluded, gitignored, or created after the index);
                       // distinct from MissingFile so a file-only outline ref
                       // is never answered by silently reading the whole file
};

inline const char* to_string(RefResolution r) {
    switch (r) {
        case RefResolution::Resolved: return "resolved";
        case RefResolution::InvalidRef: return "invalid_ref";
        case RefResolution::MissingFile: return "missing_file";
        case RefResolution::MissingSymbol: return "missing_symbol";
        case RefResolution::AmbiguousSymbol: return "ambiguous_symbol";
        case RefResolution::UnsupportedVersion: return "unsupported_version";
        case RefResolution::FileNotIndexed: return "not_indexed";
    }
    return "unknown";
}

/// A saved reference that could not be hydrated, reported with the original
/// selector and role so the caller knows exactly what was asked for and why
/// it failed — never a silent drop.
struct UnresolvedRef {
    std::string file;
    std::string symbol;
    std::string role;
    std::string note;
    LineRange lines;
    bool has_line_range{};
    RefResolution reason{};
};

/// An expansion directive that was refused (unknown, malformed, or exceeding
/// the directive's supported depth), reported with the ref it was attached to
/// so a caller sees exactly what was rejected and why — never a silent skip.
struct ExpansionError {
    std::string file;
    std::string symbol;
    std::string role;
    std::string directive;
    std::string reason;
};

/// Expanded context with full source code and relationships.
struct HydratedContext {
    std::string task;
    std::vector<HydratedRef> refs;
    std::vector<UnresolvedRef> unresolved;
    std::vector<ExpansionError> expansion_errors;
    HydrationStats stats;
    std::vector<std::string> warnings;
};

/// Output format for hydrated context.
enum class FormatType : uint8_t {
    Full = 0,
    Signatures,
    Outline,
};

}  // namespace lci
