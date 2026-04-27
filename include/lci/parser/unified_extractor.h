#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <lci/reference.h>
#include <lci/scope.h>
#include <lci/side_effects.h>
#include <lci/symbol.h>
#include <lci/types.h>

struct TSNode;
struct TSTree;

namespace lci::parser {

/// Zero-allocation key for line:column lookups.
/// Replaces string formatting for position-based maps.
struct PositionKey {
    int line{};
    int column{};

    bool operator==(const PositionKey& other) const {
        return line == other.line && column == other.column;
    }
};

/// Hash function for PositionKey to use in unordered containers.
struct PositionKeyHash {
    std::size_t operator()(const PositionKey& k) const {
        return static_cast<std::size_t>(k.line) * 131071 +
               static_cast<std::size_t>(k.column);
    }
};

/// Signature and doc comment extracted from a declaration node.
struct DeclarationInfo {
    std::string signature;
    std::string doc_comment;
};

/// Scope stack entry used during AST traversal.
struct ScopeStackEntry {
    ScopeType scope_type{};
    std::string name;
    int start_line{};
    int end_line{};
};

/// Results from a unified extraction pass.
struct ExtractionResults {
    std::vector<Symbol> symbols;
    std::vector<BlockBoundary> blocks;
    std::vector<Import> imports;
    std::vector<ScopeInfo> scopes;
    std::vector<Reference> references;
    std::vector<std::pair<std::string, DeclarationInfo>> declarations;
    std::vector<std::pair<PositionKey, int>> complexity;
};

/// Performs all AST extraction in a single tree walk.
///
/// Consolidates 6+ separate passes into one traversal:
/// 1. Scope hierarchy extraction
/// 2. Reference extraction (language-specific)
/// 3. Declaration metadata (signatures, doc comments)
/// 4. Complexity calculation per function
/// 5. Symbol, block, and import extraction
/// 6. Type relationship extraction
///
/// Mirrors Go's UnifiedExtractor in internal/parser/unified_extractor.go.
class UnifiedExtractor {
  public:
    UnifiedExtractor() = default;

    /// Initializes the extractor for a specific file.
    void init(std::string_view content, FileID file_id,
              std::string_view ext, std::string_view path);

    /// Resets state while keeping allocated capacity for reuse via pool.
    void reset();

    /// Performs the single-pass extraction on a parsed tree.
    /// Returns silently if tree is null (expected for unparseable files).
    void extract(TSTree* tree);

    /// Returns all extraction results.
    ExtractionResults get_results() const;

    /// Looks up declaration info (signature, doc comment) by 1-based position.
    /// Returns empty strings if not found.
    std::pair<std::string_view, std::string_view>
    lookup_declaration(int line, int column) const;

  private:
    // Core visitor that extracts all data types in a single pass.
    void visit_node(TSNode node);

    // Node type caching to reduce tree-sitter API calls.
    // Returns cached type or queries and caches it.
    std::string_view get_node_type(TSNode node);

    // Returns true if the node type represents a function definition.
    static bool is_function_node(std::string_view node_type);

    // Returns true if the node type represents a loop construct.
    static bool is_loop_node(std::string_view node_type);

    // Returns true if the node type represents a declaration.
    static bool is_declaration_node(std::string_view node_type);

    // Extracts the function name from a function node.
    std::string_view extract_function_name(TSNode node,
                                           std::string_view node_type);

    // Returns lazily-split content lines for context extraction.
    const std::vector<std::string_view>& get_lines();

    // Builds a fully qualified name from the current scope stack.
    std::string build_full_qualified_name(std::string_view name) const;

    // Extracts text from a node as a string_view into content_.
    std::string_view node_text(TSNode node) const;

    // --- Scope extraction ---
    // Returns a ScopeStackEntry if this node creates a new scope.
    // Returns has_value=true if a scope was pushed.
    bool process_scope_node(TSNode node, std::string_view node_type,
                            ScopeStackEntry& out);

    // Extracts Go type name from a type_declaration node.
    std::string_view extract_go_type_name(TSNode node);

    // --- Symbol/block/import extraction ---
    void process_symbol_node(TSNode node, std::string_view node_type);

    void extract_function(TSNode node, std::string_view node_type);
    void extract_method(TSNode node, std::string_view node_type);
    void extract_python_method(TSNode node);
    void extract_rust_method(TSNode node);
    void extract_arrow_function_dual(TSNode func_node, TSNode decl_node);
    void extract_class(TSNode node, std::string_view node_type);
    void extract_interface(TSNode node);
    void extract_type_declaration(TSNode node);
    void extract_type_alias(TSNode node);
    void extract_struct(TSNode node);
    void extract_enum(TSNode node);
    void extract_trait(TSNode node);
    void extract_impl(TSNode node);
    void extract_module(TSNode node);
    void extract_namespace(TSNode node);
    void extract_variable(TSNode node);
    void extract_go_variable(TSNode node, std::string_view node_type);
    void extract_constructor(TSNode node);
    void extract_property(TSNode node);
    void extract_field(TSNode node);

    // --- Language-specific extraction (in parse_methods.cpp) ---
    // Java
    void extract_java_constructor(TSNode node);
    void extract_annotation_type(TSNode node);
    void extract_java_import(TSNode node);
    void extract_package_declaration(TSNode node);
    // C#
    void extract_record(TSNode node);
    void extract_csharp_property(TSNode node);
    void extract_using_directive(TSNode node);
    void extract_delegate(TSNode node);
    void extract_event(TSNode node);
    void extract_csharp_field(TSNode node);
    // PHP
    void extract_php_trait(TSNode node);
    void extract_php_namespace(TSNode node);
    void extract_php_use(TSNode node);
    void extract_php_const(TSNode node);
    // C/C++
    void extract_struct_specifier(TSNode node);
    void extract_enum_specifier(TSNode node);
    void extract_cpp_namespace(TSNode node);
    void extract_preproc_include(TSNode node);
    void extract_using_declaration(TSNode node);
    // Rust
    void extract_rust_use(TSNode node);
    // Kotlin
    void extract_kotlin_object(TSNode node);
    void extract_kotlin_import(TSNode node);
    // Zig
    void extract_zig_struct(TSNode node);
    // Ruby
    void extract_ruby_module(TSNode node);

    // Import extraction
    void extract_js_import(TSNode node);
    void extract_python_import(TSNode node);
    void extract_go_import(TSNode node, std::string_view node_type);

    bool is_arrow_function_declarator(TSNode node);

    // --- Declaration metadata extraction ---
    void process_declaration_node(TSNode node, std::string_view node_type);
    std::string extract_signature(TSNode node, std::string_view node_type);
    std::string extract_doc_comment(TSNode node);

    // --- Reference extraction ---
    void process_reference_node(TSNode node, std::string_view node_type);
    void process_go_reference(TSNode node, std::string_view node_type);
    void process_js_reference(TSNode node, std::string_view node_type);
    void process_python_reference(TSNode node, std::string_view node_type);
    Reference create_reference(TSNode node, ReferenceType ref_type,
                               RefStrength strength);

    // --- Complexity tracking ---
    void count_complexity_point(TSNode node, std::string_view node_type);

    // --- Type relationship extraction (in unified_extractor_types.cpp) ---
    void process_type_relationships(TSNode node, std::string_view node_type);
    void process_go_type_relationships(TSNode node, std::string_view node_type);
    void process_js_type_relationships(TSNode node, std::string_view node_type);
    void process_python_type_relationships(TSNode node,
                                           std::string_view node_type);

    // --- Side effect tracking (in unified_extractor_side_effects.cpp) ---
    void process_side_effect_node(TSNode node, std::string_view node_type);

    // Input data
    std::string_view content_;
    FileID file_id_{};
    std::string_view ext_;
    std::string_view path_;

    // Output collections (pre-allocated for reuse)
    std::vector<Symbol> symbols_;
    std::vector<BlockBoundary> blocks_;
    std::vector<Import> imports_;
    std::vector<ScopeInfo> scopes_;
    std::vector<Reference> references_;
    std::vector<std::pair<std::string, DeclarationInfo>> declarations_;
    std::vector<std::pair<PositionKey, int>> complexity_;

    // Cached content lines (lazily initialized)
    std::vector<std::string_view> lines_;
    bool lines_initialized_{};

    // Context tracking during traversal
    std::vector<ScopeStackEntry> scope_stack_;
    uint64_t ref_id_{1};
    int current_level_{};
    bool in_import_context_{};
    bool in_trait_or_impl_body_{};
    bool in_class_body_{};

    // Complexity tracking (stack for nested functions)
    std::vector<int> complexity_stack_;
    PositionKey current_func_key_{};
    bool has_current_func_{};

    // Node type cache (bounded to prevent memory accumulation)
    static constexpr std::size_t kNodeTypeCacheMaxSize = 10000;
    struct NodeTypeEntry {
        uintptr_t id{};
        std::string type;
    };
    std::vector<NodeTypeEntry> node_type_cache_;

    // Handled nodes set (for JS context-aware extraction)
    struct HandledEntry {
        uintptr_t id{};
    };
    std::vector<HandledEntry> handled_nodes_;
};

/// Thread-local pool of UnifiedExtractor instances.
/// Reduces allocation overhead by reusing extractors with pre-allocated buffers.
class ExtractorPool {
  public:
    ExtractorPool() = default;

    /// Gets an extractor from the pool, initializing it for the given file.
    /// Creates a new extractor if the pool is empty.
    UnifiedExtractor* acquire(std::string_view content, FileID file_id,
                              std::string_view ext, std::string_view path);

    /// Returns an extractor to the pool after resetting its state.
    void release(UnifiedExtractor* extractor);

  private:
    std::vector<std::unique_ptr<UnifiedExtractor>> pool_;
};

/// Returns the thread-local extractor pool.
ExtractorPool& thread_extractor_pool();

}  // namespace lci::parser
