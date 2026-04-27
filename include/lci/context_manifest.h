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
    PurityInfo purity;
    bool has_purity{};
};

/// Statistics about the hydration process.
struct HydrationStats {
    int refs_loaded{};
    int symbols_hydrated{};
    int tokens_approx{};
    int expansions_applied{};
    bool truncated{};
};

/// Expanded context with full source code and relationships.
struct HydratedContext {
    std::string task;
    std::vector<HydratedRef> refs;
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
