#pragma once

#include <memory>

#include <re2/re2.h>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/types.h>

namespace lci {

struct Symbol;
class MasterIndex;

/// Structured metadata extracted from @lci: comments.
struct SemanticAnnotation {
    std::vector<std::string> labels;
    std::string category;
    absl::flat_hash_map<std::string, std::string> tags;
    std::vector<std::string> excludes;
    std::vector<std::string> provides;
    absl::flat_hash_map<std::string, std::string> metrics;
    absl::flat_hash_map<std::string, std::string> attributes;

    // Memory analysis hints
    double loop_weight{};
    int loop_bounded{};
    std::string call_frequency;
    double propagation_weight{};
    bool has_memory_hints{};

    // Source location
    FileID file_id{};
    SymbolID symbol_id{};
    int start_line{};
    int end_line{};
    std::string raw_text;
    double confidence{1.0};
};

/// A symbol with its associated annotation.
struct AnnotatedSymbol {
    FileID file_id{};
    SymbolID symbol_id{};
    std::string name;
    int line{};
    SemanticAnnotation annotation;
    std::string file_path;
};

/// Extracts and manages semantic annotations from code comments.
///
/// Supports the @lci: prefix for structured metadata:
///   @lci:labels[api,public]
///   @lci:category[endpoint]
///   @lci:tags[team=backend]
///   @lci:exclude[memory]
///   @lci:loop-weight[0.1]
///   @lci:loop-bounded[3]
///   @lci:call-frequency[hot-path]
///   @lci:propagation-weight[0.5]
class SemanticAnnotator {
  public:
    SemanticAnnotator();

    /// Extracts annotations from file content associated with symbols.
    void extract_annotations(FileID file_id, std::string_view file_path,
                             std::string_view content,
                             const std::vector<Symbol>& symbols);

    /// Returns the annotation for a specific symbol, or nullptr.
    const SemanticAnnotation* get_annotation(FileID file_id,
                                             SymbolID symbol_id) const;

    /// Walks every indexed file's content + symbol list and extracts @lci:
    /// annotations into this annotator. Idempotent: callers should invoke
    /// once after the MasterIndex completes indexing. Returns the number of
    /// files processed (i.e., files with at least one symbol). Walking the
    /// index is a one-shot read-only pass; no locks held across files (the
    /// content store and reference tracker are both lock-free read snapshots).
    int populate_from_index(const MasterIndex& index);

    /// Returns all symbols with a specific label.
    std::vector<const AnnotatedSymbol*> get_symbols_by_label(
        std::string_view label) const;

    /// Returns all symbols whose annotation category matches `category`.
    /// Ports Go's SemanticAnnotator.GetSymbolsByCategory. O(1) lookup
    /// because we maintain a category→symbol index alongside the label
    /// index. Empty vector when no symbols match.
    std::vector<const AnnotatedSymbol*> get_symbols_by_category(
        std::string_view category) const;

    /// Checks if a symbol is excluded from a given analysis type.
    bool is_excluded(FileID file_id, SymbolID symbol_id,
                     std::string_view analysis_type) const;

    /// Returns statistics about extracted annotations.
    int total_annotations() const;
    int unique_labels() const;

  private:
    // Annotation patterns: compiled ONCE at construction (RE2 instances are
    // immutable + thread-safe for matching). Held by unique_ptr because RE2
    // is non-copyable/non-movable. Karpathy: zero compile cost per annotation
    // line; matching is linear-time vs std::regex backtracking.
    struct Patterns {
        std::unique_ptr<RE2> labels;
        std::unique_ptr<RE2> category;
        std::unique_ptr<RE2> tags;
        std::unique_ptr<RE2> deps;
        std::unique_ptr<RE2> provides;
        std::unique_ptr<RE2> metrics;
        std::unique_ptr<RE2> attr;
        std::unique_ptr<RE2> exclude;
        std::unique_ptr<RE2> loop_weight;
        std::unique_ptr<RE2> loop_bounded;
        std::unique_ptr<RE2> call_frequency;
        std::unique_ptr<RE2> propagation_weight;
    };

    static Patterns make_patterns();
    bool is_annotation_line(std::string_view line) const;
    SemanticAnnotation* extract_symbol_annotation(
        FileID file_id, const Symbol& symbol,
        const std::vector<std::string_view>& lines);
    void parse_annotation_line(std::string_view line,
                               SemanticAnnotation& annotation);
    void parse_memory_hints(std::string_view line,
                            SemanticAnnotation& annotation);
    std::vector<std::string> parse_comma_separated(std::string_view input) const;
    absl::flat_hash_map<std::string, std::string> parse_key_value_pairs(
        std::string_view input) const;

    static bool is_valid_call_frequency(std::string_view freq);

    Patterns patterns_;
    absl::flat_hash_map<FileID,
        absl::flat_hash_map<SymbolID, SemanticAnnotation>> annotations_;
    absl::flat_hash_map<std::string, std::vector<AnnotatedSymbol>> label_index_;
    // Category→symbol index parallels label_index_. Built incrementally as
    // each annotation lands; never read concurrently with writes (populated
    // once at startup by populate_from_index, then read-only).
    absl::flat_hash_map<std::string, std::vector<AnnotatedSymbol>> category_index_;
};

}  // namespace lci
