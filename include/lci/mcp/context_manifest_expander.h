#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lci/context_manifest.h>
#include <lci/types.h>

namespace lci {

class MasterIndex;
class ReferenceTracker;

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

    /// Hydrates a single reference into source code.
    /// Returns the hydrated ref, approximate token count, and error string
    /// (empty on success).
    struct HydrateResult {
        HydratedRef ref;
        int tokens{};
        std::string error;
    };
    HydrateResult hydrate_reference(const ContextRef& ref, FormatType format,
                                    const std::string& project_root);

    /// Applies expansion directives (callers, callees, etc.) to a reference.
    /// Returns additional token count consumed, and error string.
    struct ExpansionResult {
        int tokens{};
        std::string error;
    };
    ExpansionResult apply_expansions(const ContextRef& ref,
                                     HydratedRef& hydrated,
                                     FormatType format,
                                     int remaining_tokens,
                                     const std::string& project_root);

  private:
    MasterIndex& index_;

    /// Extracts source for a symbol by name, optionally using a line hint.
    struct ExtractResult {
        std::string source;
        LineRange lines;
        SymbolInfo info;
        std::string error;
    };
    ExtractResult extract_symbol_source(const std::string& file_path,
                                        const std::string& symbol_name,
                                        const LineRange* line_hint,
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
                             const std::string& project_root);

    /// Gets the file path for a symbol's file ID.
    std::string get_file_path(FileID file_id);

    // -- Expansion methods ---------------------------------------------------

    std::vector<HydratedRef> expand_callers(
        const ContextRef& ref, int depth, int remaining_tokens,
        const std::string& project_root, FormatType format);

    std::vector<HydratedRef> expand_callees(
        const ContextRef& ref, int depth, int remaining_tokens,
        const std::string& project_root, FormatType format);

    std::vector<HydratedRef> expand_implementations(
        const ContextRef& ref, int remaining_tokens,
        const std::string& project_root, FormatType format);

    std::vector<HydratedRef> expand_interface(
        const ContextRef& ref, int remaining_tokens,
        const std::string& project_root, FormatType format);

    std::vector<HydratedRef> expand_siblings(
        const ContextRef& ref, int remaining_tokens,
        const std::string& project_root, FormatType format);

    std::vector<HydratedRef> expand_tests(
        const ContextRef& ref, int remaining_tokens,
        const std::string& project_root, FormatType format);

    /// Extracts only the doc comments from source.
    void extract_documentation(HydratedRef& ref);

    /// Replaces source with just the signature line.
    void extract_signature_only(HydratedRef& ref);
};

}  // namespace mcp
}  // namespace lci
