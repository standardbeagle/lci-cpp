#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lci {

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
struct PurityInfo {
    bool is_pure{};
    std::string purity_level;
    std::vector<std::string> categories;
    double purity_score{};
    std::vector<std::string> reasons;
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
};

/// Outcome of resolving a reference's identity against the current index.
/// A saved file+symbol resolves only inside that file; a saved object id is
/// never persistent identity.
enum class RefResolution : uint8_t {
    Resolved = 0,
    InvalidRef,       // no file, symbol, or line range to select on
    MissingFile,      // named file is not in the index
    MissingSymbol,    // file present, symbol absent (never cross-file match)
    AmbiguousSymbol,  // >1 same-name symbol in the named file
};

inline const char* to_string(RefResolution r) {
    switch (r) {
        case RefResolution::Resolved: return "resolved";
        case RefResolution::InvalidRef: return "invalid_ref";
        case RefResolution::MissingFile: return "missing_file";
        case RefResolution::MissingSymbol: return "missing_symbol";
        case RefResolution::AmbiguousSymbol: return "ambiguous_symbol";
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

/// Expanded context with full source code and relationships.
struct HydratedContext {
    std::string task;
    std::vector<HydratedRef> refs;
    std::vector<UnresolvedRef> unresolved;
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
