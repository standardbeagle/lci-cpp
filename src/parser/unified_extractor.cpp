#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

namespace lci::parser {

namespace {

bool is_cpp_family_extension(std::string_view ext) {
    return ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
           ext == ".h" || ext == ".hpp";
}

TSNode pick_cpp_reference_leaf(TSNode node) {
    if (ts_node_is_null(node)) return node;

    const char* raw_type = ts_node_type(node);
    std::string_view node_type(raw_type ? raw_type : "");

    if (node_type == "field_expression") {
        TSNode field = ts_node_child_by_field_name(
            node, "field", static_cast<uint32_t>(std::strlen("field")));
        if (!ts_node_is_null(field)) return field;
    }

    if (node_type == "qualified_identifier" || node_type == "scoped_identifier" ||
        node_type == "template_function" || node_type == "template_method") {
        for (const char* field_name : {"name", "field"}) {
            TSNode child = ts_node_child_by_field_name(
                node, field_name,
                static_cast<uint32_t>(std::strlen(field_name)));
            if (!ts_node_is_null(child)) return pick_cpp_reference_leaf(child);
        }
    }

    uint32_t child_count = ts_node_named_child_count(node);
    for (uint32_t i = child_count; i > 0; --i) {
        TSNode child = ts_node_named_child(node, i - 1);
        if (ts_node_is_null(child)) continue;
        std::string_view child_type(ts_node_type(child));
        if (child_type == "identifier" || child_type == "field_identifier" ||
            child_type == "type_identifier" ||
            child_type == "namespace_identifier" ||
            child_type == "destructor_name") {
            return child;
        }
    }

    return node;
}

bool is_cpp_type_declaration_name_context(TSNode node) {
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent)) return false;

    std::string_view parent_type(ts_node_type(parent));
    return parent_type == "class_specifier" || parent_type == "struct_specifier" ||
           parent_type == "enum_specifier" ||
           parent_type == "namespace_definition";
}

}  // namespace

// ---------------------------------------------------------------------------
// UnifiedExtractor: init / reset / extract
// ---------------------------------------------------------------------------

void UnifiedExtractor::init(std::string_view content, FileID file_id,
                            std::string_view ext, std::string_view path) {
    content_ = content;
    file_id_ = file_id;
    ext_ = ext;
    path_ = path;
    ref_id_ = 1;
    current_level_ = 0;
    in_import_context_ = false;
    in_trait_or_impl_body_ = false;
    in_class_body_ = false;
    has_current_func_ = false;
    lines_initialized_ = false;
}

void UnifiedExtractor::reset() {
    content_ = {};
    file_id_ = 0;
    ext_ = {};
    path_ = {};

    symbols_.clear();
    blocks_.clear();
    imports_.clear();
    scopes_.clear();
    references_.clear();
    declarations_.clear();
    complexity_.clear();

    lines_.clear();
    lines_initialized_ = false;

    scope_stack_.clear();
    ref_id_ = 1;
    current_level_ = 0;
    in_import_context_ = false;
    in_trait_or_impl_body_ = false;
    in_class_body_ = false;

    complexity_stack_.clear();
    has_current_func_ = false;

    handled_nodes_.clear();
}

void UnifiedExtractor::extract(TSTree* tree) {
    if (!tree) return;

    TSNode root = ts_tree_root_node(tree);
    if (ts_node_is_null(root)) return;

    // Add file-level scope
    std::filesystem::path file_path(path_);
    ScopeInfo file_scope;
    file_scope.type = ScopeType::File;
    file_scope.name = file_path.filename().string();
    file_scope.full_path = std::string(path_);
    file_scope.start_line = 0;
    file_scope.end_line =
        static_cast<int>(ts_node_end_point(root).row) + 1;
    file_scope.level = 0;

    Language lang{};
    if (language_from_extension(ext_, lang)) {
        file_scope.language = std::string(to_string(lang));
    }
    scopes_.push_back(std::move(file_scope));

    // Add folder-level scope
    std::string dir = file_path.parent_path().string();
    if (!dir.empty() && dir != "." && dir != "/") {
        ScopeInfo folder_scope;
        folder_scope.type = ScopeType::Folder;
        folder_scope.name = file_path.parent_path().filename().string();
        folder_scope.full_path = dir;
        folder_scope.start_line = 0;
        folder_scope.end_line = 0;
        folder_scope.level = -1;
        scopes_.insert(scopes_.begin(), std::move(folder_scope));
    }

    current_level_ = 1;
    visit_node(root);
}

ExtractionResults UnifiedExtractor::get_results() const {
    return {symbols_,      blocks_,  imports_,     scopes_,
            references_,   declarations_, complexity_};
}

std::pair<std::string_view, std::string_view>
UnifiedExtractor::lookup_declaration(int line, int column) const {
    // Convert 1-based to 0-based for lookup key
    std::string key =
        std::to_string(line - 1) + ":" + std::to_string(column - 1);
    for (const auto& [k, info] : declarations_) {
        if (k == key) {
            return {info.signature, info.doc_comment};
        }
    }
    return {{}, {}};
}

// ---------------------------------------------------------------------------
// Core visitor
// ---------------------------------------------------------------------------

void UnifiedExtractor::visit_node(TSNode node) {
    if (ts_node_is_null(node)) return;

    // get_node_type returns a view into tree-sitter's interned static
    // symbol table — stable for the lifetime of the parser/language.
    // No copy needed; the previous std::string here was a per-visit
    // heap alloc accommodating the prior (since-removed) vector cache.
    std::string_view node_type = get_node_type(node);

    // === COMPLEXITY TRACKING ===
    PositionKey func_key{};
    bool is_func = false;
    if (is_function_node(node_type)) {
        is_func = true;
        TSPoint start = ts_node_start_point(node);
        func_key = {static_cast<int>(start.row) + 1,
                     static_cast<int>(start.column) + 1};
        complexity_stack_.push_back(1);  // base complexity = 1
    }

    if (!complexity_stack_.empty()) {
        count_complexity_point(node, node_type);
    }

    // === SYMBOL/BLOCK/IMPORT EXTRACTION ===
    process_symbol_node(node, node_type);

    // === SCOPE EXTRACTION ===
    ScopeStackEntry scope_entry;
    bool pushed_scope = process_scope_node(node, node_type, scope_entry);
    if (pushed_scope) {
        scope_stack_.push_back(scope_entry);
        current_level_++;
    }

    // === DECLARATION EXTRACTION ===
    process_declaration_node(node, node_type);

    // === REFERENCE EXTRACTION ===
    process_reference_node(node, node_type);

    // === TYPE RELATIONSHIP EXTRACTION ===
    process_type_relationships(node, node_type);

    // Track import context
    bool was_import = in_import_context_;
    if (node_type == "import_statement") {
        in_import_context_ = true;
    }

    // Track trait/impl context (Rust)
    bool was_trait_impl = in_trait_or_impl_body_;
    if (node_type == "trait_item" || node_type == "impl_item") {
        in_trait_or_impl_body_ = true;
    }

    // Track class body context
    bool was_class = in_class_body_;
    if (node_type == "class_definition" || node_type == "class_declaration" ||
        node_type == "class_body" || node_type == "class") {
        in_class_body_ = true;
    }

    // === RECURSE INTO CHILDREN ===
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        visit_node(ts_node_child(node, i));
    }

    // Restore context flags
    if (node_type == "import_statement") {
        in_import_context_ = was_import;
    }
    if (node_type == "trait_item" || node_type == "impl_item") {
        in_trait_or_impl_body_ = was_trait_impl;
    }
    if (node_type == "class_definition" || node_type == "class_declaration" ||
        node_type == "class_body" || node_type == "class") {
        in_class_body_ = was_class;
    }

    // Pop scope
    if (pushed_scope) {
        scope_stack_.pop_back();
        current_level_--;
    }

    // Finalize complexity for function exit
    if (is_func && !complexity_stack_.empty()) {
        int cx = complexity_stack_.back();
        complexity_stack_.pop_back();
        complexity_.push_back({func_key, cx});
    }
}

// ---------------------------------------------------------------------------
// Node type caching
// ---------------------------------------------------------------------------

std::string_view UnifiedExtractor::get_node_type(TSNode node) {
    // ts_node_type returns a pointer into tree-sitter's interned, static
    // language-symbol table — stable for the lifetime of the parser.
    // No copy, no cache: just return the view. The prior cache
    // implementation linear-scanned a vector<NodeTypeEntry> on every
    // call AND copied each uncached type into a std::string (heap
    // alloc per uncached node) AND grew unbounded past the "max size"
    // gate — a single hot-path optimization undid all three.
    // perf record on BM_RealProjectIndexFastapi pinned get_node_type at
    // 20.36% of CPU before this fix.
    const char* raw = ts_node_type(node);
    return raw ? std::string_view(raw) : std::string_view{};
}

// ---------------------------------------------------------------------------
// Node classification helpers
// ---------------------------------------------------------------------------

bool UnifiedExtractor::is_function_node(std::string_view t) {
    return t == "function_declaration" || t == "function_definition" ||
           t == "function_item" || t == "method_definition" ||
           t == "method_declaration" || t == "method" ||
           t == "arrow_function" ||
           t == "function_expression" || t == "generator_function" ||
           t == "generator_function_declaration" || t == "func_literal" ||
           t == "constructor_definition";
}

bool UnifiedExtractor::is_loop_node(std::string_view t) {
    return t == "for_statement" || t == "for_range_statement" ||
           t == "for_in_statement" || t == "for_of_statement" ||
           t == "while_statement" || t == "do_while_statement" ||
           t == "do_statement" || t == "loop_expression" ||
           t == "while_expression" || t == "for_expression" ||
           t == "for_each_statement" || t == "enhanced_for_statement" ||
           t == "foreach_statement";
}

bool UnifiedExtractor::is_declaration_node(std::string_view t) {
    return t == "function_declaration" || t == "method_declaration" ||
           t == "function_definition" || t == "method_definition" ||
           t == "class_declaration" || t == "class_definition" ||
           t == "type_declaration" || t == "interface_declaration" ||
           t == "struct_declaration" || t == "enum_declaration" ||
           t == "const_declaration" || t == "var_declaration";
}

std::string_view UnifiedExtractor::extract_function_name(
    TSNode node, std::string_view node_type) {
    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (!ts_node_is_null(name_node)) {
        return node_text(name_node);
    }

    // C/C++ style: declarator -> declarator
    TSNode decl = ts_node_child_by_field_name(
        node, "declarator", static_cast<uint32_t>(std::strlen("declarator")));
    if (!ts_node_is_null(decl)) {
        TSNode inner = ts_node_child_by_field_name(
            decl, "declarator",
            static_cast<uint32_t>(std::strlen("declarator")));
        if (!ts_node_is_null(inner)) {
            return node_text(inner);
        }
        return node_text(decl);
    }

    // Arrow functions assigned to variables
    if (node_type == "arrow_function" || node_type == "function_expression") {
        TSNode parent = ts_node_parent(node);
        if (!ts_node_is_null(parent) &&
            get_node_type(parent) == "variable_declarator") {
            TSNode n = ts_node_child_by_field_name(
                parent, "name",
                static_cast<uint32_t>(std::strlen("name")));
            if (!ts_node_is_null(n)) {
                return node_text(n);
            }
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// Content line splitting (lazy)
// ---------------------------------------------------------------------------

const std::vector<std::string_view>& UnifiedExtractor::get_lines() {
    if (lines_initialized_) return lines_;
    lines_initialized_ = true;

    if (content_.empty()) return lines_;

    std::size_t start = 0;
    for (std::size_t i = 0; i < content_.size(); ++i) {
        if (content_[i] == '\n') {
            lines_.push_back(content_.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < content_.size()) {
        lines_.push_back(content_.substr(start));
    }
    return lines_;
}

std::string UnifiedExtractor::build_full_qualified_name(
    std::string_view name) const {
    if (scope_stack_.empty()) return std::string(name);

    std::string result;
    for (const auto& entry : scope_stack_) {
        if (!entry.name.empty() && entry.name != "block") {
            if (!result.empty()) result += '.';
            result += entry.name;
        }
    }
    if (!result.empty()) result += '.';
    result += name;
    return result;
}

std::string_view UnifiedExtractor::node_text(TSNode node) const {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= content_.size() || end > content_.size() || start >= end) {
        return {};
    }
    return content_.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// Scope extraction
// ---------------------------------------------------------------------------

bool UnifiedExtractor::process_scope_node(TSNode node,
                                          std::string_view node_type,
                                          ScopeStackEntry& out) {
    ScopeType scope_type{};
    std::string name;

    if (node_type == "class_declaration" || node_type == "class_definition" ||
        node_type == "class") {
        scope_type = ScopeType::Class;
        TSNode n = ts_node_child_by_field_name(
            node, "name", static_cast<uint32_t>(std::strlen("name")));
        if (!ts_node_is_null(n)) name = std::string(node_text(n));

    } else if (node_type == "type_declaration") {
        scope_type = ScopeType::Class;
        name = std::string(extract_go_type_name(node));

    } else if (node_type == "function_declaration" ||
               node_type == "function_definition") {
        scope_type = ScopeType::Function;
        TSNode n = ts_node_child_by_field_name(
            node, "name", static_cast<uint32_t>(std::strlen("name")));
        if (!ts_node_is_null(n)) name = std::string(node_text(n));

    } else if (node_type == "method_definition" ||
               node_type == "method_declaration" ||
               node_type == "method") {
        scope_type = ScopeType::Method;
        TSNode n = ts_node_child_by_field_name(
            node, "name", static_cast<uint32_t>(std::strlen("name")));
        if (!ts_node_is_null(n)) name = std::string(node_text(n));

    } else if (node_type == "interface_declaration") {
        scope_type = ScopeType::Interface;
        TSNode n = ts_node_child_by_field_name(
            node, "name", static_cast<uint32_t>(std::strlen("name")));
        if (!ts_node_is_null(n)) name = std::string(node_text(n));

    } else if (node_type == "block_statement" ||
               node_type == "compound_statement") {
        scope_type = ScopeType::Block;
        name = "block";

    } else {
        return false;
    }

    if (name.empty() && scope_type != ScopeType::Block) {
        return false;
    }

    int start_line = static_cast<int>(ts_node_start_point(node).row) + 1;
    int end_line = static_cast<int>(ts_node_end_point(node).row) + 1;

    ScopeInfo scope;
    scope.type = scope_type;
    scope.name = name;
    scope.full_path = build_full_qualified_name(name);
    scope.start_line = start_line;
    scope.end_line = end_line;
    scope.level = current_level_;
    scopes_.push_back(std::move(scope));

    out.scope_type = scope_type;
    out.name = name;
    out.start_line = start_line;
    out.end_line = end_line;
    return true;
}

std::string_view UnifiedExtractor::extract_go_type_name(TSNode node) {
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (get_node_type(child) == "type_spec") {
            uint32_t spec_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < spec_count; ++j) {
                TSNode spec_child = ts_node_child(child, j);
                if (get_node_type(spec_child) == "type_identifier") {
                    return node_text(spec_child);
                }
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Symbol / block / import extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::process_symbol_node(TSNode node,
                                           std::string_view node_type) {
    // === FUNCTIONS ===
    if (node_type == "function_declaration" ||
        node_type == "generator_function_declaration" ||
        node_type == "func_literal") {
        extract_function(node, node_type);

    } else if (node_type == "function_definition") {
        if (in_class_body_) {
            extract_python_method(node);
        } else {
            extract_function(node, node_type);
        }

    } else if (node_type == "function_item") {
        if (in_trait_or_impl_body_) {
            extract_rust_method(node);
        } else {
            extract_function(node, node_type);
        }

    } else if (node_type == "method_definition" ||
               node_type == "method_declaration" ||
               node_type == "method") {
        extract_method(node, node_type);

    } else if (node_type == "arrow_function" ||
               node_type == "function_expression" ||
               node_type == "generator_function") {
        TSNode parent = ts_node_parent(node);
        if (!ts_node_is_null(parent) &&
            get_node_type(parent) == "variable_declarator") {
            extract_arrow_function_dual(node, parent);
        }

    // === CLASSES ===
    } else if (node_type == "class_declaration" ||
               node_type == "class_definition" ||
               node_type == "class_specifier" ||
               node_type == "class") {
        extract_class(node, node_type);

    // === INTERFACES ===
    } else if (node_type == "interface_declaration") {
        extract_interface(node);

    // === TYPES ===
    } else if (node_type == "type_declaration") {
        extract_type_declaration(node);

    } else if (node_type == "type_alias_declaration") {
        extract_type_alias(node);

    // === STRUCTS ===
    } else if (node_type == "struct_item" ||
               node_type == "struct_expression" ||
               node_type == "struct_declaration") {
        extract_struct(node);

    } else if (node_type == "struct_specifier") {
        extract_struct_specifier(node);

    // === ENUMS ===
    } else if (node_type == "enum_declaration" ||
               node_type == "enum_item") {
        extract_enum(node);

    } else if (node_type == "enum_specifier") {
        extract_enum_specifier(node);

    // === RECORDS (C#, Java) ===
    } else if (node_type == "record_declaration") {
        extract_record(node);

    // === TRAITS/IMPLS (Rust) ===
    } else if (node_type == "trait_item") {
        extract_trait(node);

    // === TRAITS (PHP) ===
    } else if (node_type == "trait_declaration") {
        extract_php_trait(node);

    } else if (node_type == "impl_item") {
        extract_impl(node);

    // === MODULES/NAMESPACES ===
    } else if (node_type == "module" || node_type == "mod_item") {
        extract_module(node);

    } else if (node_type == "namespace_declaration") {
        extract_namespace(node);

    } else if (node_type == "namespace_definition") {
        if (ext_ == ".php" || ext_ == ".phtml") {
            extract_php_namespace(node);
        } else {
            extract_cpp_namespace(node);
        }

    // === OBJECTS (Kotlin) ===
    } else if (node_type == "object_declaration") {
        extract_kotlin_object(node);

    // === RUBY modules ===
    } else if (node_type == "module" && ext_ == ".rb") {
        extract_ruby_module(node);

    // === ANNOTATION TYPES (Java) ===
    } else if (node_type == "annotation_type_declaration") {
        extract_annotation_type(node);

    // === DELEGATES (C#) ===
    } else if (node_type == "delegate_declaration") {
        extract_delegate(node);

    // === EVENTS (C#) ===
    } else if (node_type == "event_field_declaration") {
        extract_event(node);

    // === VARIABLES ===
    } else if (node_type == "variable_declarator") {
        if (!is_arrow_function_declarator(node)) {
            extract_variable(node);
        }

    } else if (node_type == "short_var_declaration" ||
               node_type == "var_declaration") {
        extract_go_variable(node, node_type);

    } else if (node_type == "const_declaration") {
        if (ext_ == ".go") {
            extract_go_variable(node, node_type);
        } else if (ext_ == ".php" || ext_ == ".phtml") {
            extract_php_const(node);
        }

    // === ZIG STRUCTS ===
    } else if (node_type == "variable_declaration" && ext_ == ".zig") {
        extract_zig_struct(node);

    // === IMPORTS ===
    } else if (node_type == "import_statement") {
        if (ext_ == ".py") {
            extract_python_import(node);
        } else {
            extract_js_import(node);
        }

    } else if (node_type == "import_from_statement") {
        extract_python_import(node);

    } else if (node_type == "import_spec") {
        extract_go_import(node, node_type);

    } else if (node_type == "import_declaration") {
        if (ext_ == ".java") {
            extract_java_import(node);
        } else if (ext_ == ".go") {
            extract_go_import(node, node_type);
        } else if (ext_ == ".kt" || ext_ == ".kts") {
            extract_kotlin_import(node);
        }

    } else if (node_type == "package_declaration") {
        extract_package_declaration(node);

    // === C/C++ IMPORTS ===
    } else if (node_type == "preproc_include") {
        extract_preproc_include(node);

    } else if (node_type == "using_declaration") {
        extract_using_declaration(node);

    // === C# IMPORTS ===
    } else if (node_type == "using_directive") {
        extract_using_directive(node);

    // === RUST IMPORTS ===
    } else if (node_type == "use_declaration") {
        extract_rust_use(node);

    // === PHP IMPORTS ===
    } else if (node_type == "namespace_use_declaration") {
        extract_php_use(node);

    // === CONSTRUCTORS ===
    } else if (node_type == "constructor_definition") {
        extract_constructor(node);

    } else if (node_type == "constructor_declaration") {
        extract_java_constructor(node);

    // === PROPERTIES/FIELDS ===
    } else if (node_type == "property_definition" ||
               node_type == "public_field_definition") {
        extract_property(node);

    } else if (node_type == "property_declaration") {
        extract_csharp_property(node);

    } else if (node_type == "field_declaration") {
        if (ext_ == ".cs") {
            extract_csharp_field(node);
        } else {
            extract_field(node);
        }
    }
}

// ---------------------------------------------------------------------------
// Individual symbol extractors
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_function(TSNode node,
                                        std::string_view node_type) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    std::string_view name;
    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (!ts_node_is_null(name_node)) {
        name = node_text(name_node);
    }

    // C++ function_definition: declarator -> declarator
    if (name.empty()) {
        TSNode decl = ts_node_child_by_field_name(
            node, "declarator",
            static_cast<uint32_t>(std::strlen("declarator")));
        if (!ts_node_is_null(decl)) {
            TSNode inner = ts_node_child_by_field_name(
                decl, "declarator",
                static_cast<uint32_t>(std::strlen("declarator")));
            if (!ts_node_is_null(inner)) {
                name = node_text(inner);
            }
        }
    }

    if (name.empty() && node_type != "func_literal" &&
        node_type != "arrow_function") {
        return;
    }

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Function;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Function;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_method(TSNode node,
                                      [[maybe_unused]] std::string_view node_type) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Method;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Method;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_python_method(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Method;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Method;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_rust_method(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Method;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Method;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_arrow_function_dual(TSNode func_node,
                                                   TSNode decl_node) {
    TSPoint start = ts_node_start_point(decl_node);
    TSPoint end = ts_node_end_point(func_node);

    TSNode name_node = ts_node_child_by_field_name(
        decl_node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Function;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    // Function symbol
    Symbol func_sym;
    func_sym.name = std::string(name);
    func_sym.type = SymbolType::Function;
    func_sym.file_id = file_id_;
    func_sym.line = static_cast<int>(start.row) + 1;
    func_sym.column = static_cast<int>(start.column) + 1;
    func_sym.end_line = static_cast<int>(end.row) + 1;
    func_sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(func_sym));

    // Variable symbol (dual nature)
    Symbol var_sym;
    var_sym.name = std::string(name);
    var_sym.type = SymbolType::Variable;
    var_sym.file_id = file_id_;
    var_sym.line = static_cast<int>(start.row) + 1;
    var_sym.column = static_cast<int>(start.column) + 1;
    var_sym.end_line = static_cast<int>(end.row) + 1;
    var_sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(var_sym));
}

void UnifiedExtractor::extract_class(TSNode node,
                                     [[maybe_unused]] std::string_view node_type) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Class;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Class;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_interface(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Interface;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Interface;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_type_declaration(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    std::string name;
    SymbolType sym_type = SymbolType::Type;
    BlockType blk_type = BlockType::Other;

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (get_node_type(child) == "type_spec") {
            uint32_t spec_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < spec_count; ++j) {
                TSNode sc = ts_node_child(child, j);
                std::string_view sct = get_node_type(sc);
                if (sct == "type_identifier") {
                    name = std::string(node_text(sc));
                } else if (sct == "struct_type") {
                    sym_type = SymbolType::Struct;
                    blk_type = BlockType::Struct;
                } else if (sct == "interface_type") {
                    sym_type = SymbolType::Interface;
                    blk_type = BlockType::Interface;
                }
            }
            break;
        }
    }

    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = blk_type;
    block.name = name;
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::move(name);
    sym.type = sym_type;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_type_alias(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Other;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Type;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_struct(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Struct;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Struct;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_enum(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Enum;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Enum;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_trait(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Trait;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Trait;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_impl(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    std::string name;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view ct = get_node_type(child);
        if (ct == "type_identifier" || ct == "generic_type") {
            name = std::string(node_text(child));
            break;
        }
    }
    if (name.empty()) name = "impl";

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Impl;
    block.name = name;
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::move(name);
    sym.type = SymbolType::Impl;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_module(TSNode node) {
    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Module;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Module;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_namespace(TSNode node) {
    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Namespace;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Namespace;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_variable(TSNode node) {
    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Variable;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_go_variable(TSNode node,
                                           std::string_view node_type) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    std::string_view spec_type = "var_spec";
    if (node_type == "const_declaration") spec_type = "const_spec";

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (get_node_type(child) == spec_type) {
            uint32_t spec_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < spec_count; ++j) {
                TSNode sc = ts_node_child(child, j);
                if (get_node_type(sc) == "identifier") {
                    SymbolType st = (node_type == "const_declaration")
                                        ? SymbolType::Constant
                                        : SymbolType::Variable;
                    Symbol sym;
                    sym.name = std::string(node_text(sc));
                    sym.type = st;
                    sym.file_id = file_id_;
                    sym.line = static_cast<int>(start.row) + 1;
                    sym.column = static_cast<int>(start.column) + 1;
                    sym.end_line = static_cast<int>(end.row) + 1;
                    sym.end_column = static_cast<int>(end.column) + 1;
                    symbols_.push_back(std::move(sym));
                }
            }
        }
    }
}

void UnifiedExtractor::extract_constructor(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    BlockBoundary block;
    block.start = static_cast<int>(start.row);
    block.end = static_cast<int>(end.row);
    block.type = BlockType::Constructor;
    block.name = "constructor";
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = "constructor";
    sym.type = SymbolType::Constructor;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_property(TSNode node) {
    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Property;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_field(TSNode node) {
    std::string_view name;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view ct = get_node_type(child);
        if (ct == "field_identifier" || ct == "identifier") {
            name = node_text(child);
            break;
        }
    }
    if (name.empty()) return;

    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Field;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

bool UnifiedExtractor::is_arrow_function_declarator(TSNode node) {
    TSNode value = ts_node_child_by_field_name(
        node, "value", static_cast<uint32_t>(std::strlen("value")));
    if (ts_node_is_null(value)) return false;
    std::string_view vt = get_node_type(value);
    return vt == "arrow_function" || vt == "function_expression" ||
           vt == "generator_function";
}

// ---------------------------------------------------------------------------
// Import extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_js_import(TSNode node) {
    TSNode source = ts_node_child_by_field_name(
        node, "source", static_cast<uint32_t>(std::strlen("source")));
    if (ts_node_is_null(source)) return;

    std::string path(node_text(source));
    // Remove surrounding quotes
    if (path.size() >= 2 &&
        (path.front() == '"' || path.front() == '\'')) {
        path = path.substr(1, path.size() - 2);
    }

    Import imp;
    imp.path = std::move(path);
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

void UnifiedExtractor::extract_python_import(TSNode node) {
    Import imp;
    imp.path = std::string(node_text(node));
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

void UnifiedExtractor::extract_go_import(TSNode node,
                                         std::string_view node_type) {
    if (node_type != "import_spec") return;

    TSNode path_node = ts_node_child_by_field_name(
        node, "path", static_cast<uint32_t>(std::strlen("path")));
    if (ts_node_is_null(path_node)) return;

    std::string path(node_text(path_node));
    // Remove surrounding quotes
    if (path.size() >= 2 && path.front() == '"') {
        path = path.substr(1, path.size() - 2);
    }

    Import imp;
    imp.path = std::move(path);
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

// ---------------------------------------------------------------------------
// Declaration metadata extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::process_declaration_node(TSNode node,
                                                std::string_view node_type) {
    if (!is_declaration_node(node_type)) return;

    std::string signature = extract_signature(node, node_type);
    std::string doc_comment = extract_doc_comment(node);

    TSPoint start = ts_node_start_point(node);
    std::string key =
        std::to_string(start.row) + ":" + std::to_string(start.column);

    DeclarationInfo info;
    info.signature = std::move(signature);
    info.doc_comment = std::move(doc_comment);
    declarations_.emplace_back(std::move(key), std::move(info));
}

std::string UnifiedExtractor::extract_signature(TSNode node,
                                                std::string_view node_type) {
    if (node_type != "function_declaration" &&
        node_type != "method_declaration") {
        return {};
    }

    uint32_t start_byte = ts_node_start_byte(node);
    uint32_t end_byte = ts_node_end_byte(node);

    // Exclude the body
    TSNode body = ts_node_child_by_field_name(
        node, "body", static_cast<uint32_t>(std::strlen("body")));
    if (!ts_node_is_null(body)) {
        end_byte = ts_node_start_byte(body);
    }

    if (start_byte >= content_.size() || end_byte > content_.size()) {
        return {};
    }

    std::string sig(content_.substr(start_byte, end_byte - start_byte));
    // Trim whitespace and trailing {
    while (!sig.empty() && (sig.back() == ' ' || sig.back() == '\t' ||
                             sig.back() == '\n' || sig.back() == '\r')) {
        sig.pop_back();
    }
    if (!sig.empty() && sig.back() == '{') {
        sig.pop_back();
        while (!sig.empty() && (sig.back() == ' ' || sig.back() == '\t')) {
            sig.pop_back();
        }
    }
    return sig;
}

std::string UnifiedExtractor::extract_doc_comment(TSNode node) {
    TSNode prev = ts_node_prev_sibling(node);
    if (ts_node_is_null(prev)) return {};

    std::string_view prev_type = get_node_type(prev);
    if (prev_type == "comment" || prev_type == "line_comment" ||
        prev_type == "block_comment") {
        return std::string(node_text(prev));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Reference extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::process_reference_node(TSNode node,
                                              std::string_view node_type) {
    if (ext_ == ".go") {
        process_go_reference(node, node_type);
    } else if (ext_ == ".js" || ext_ == ".jsx" || ext_ == ".ts" ||
               ext_ == ".tsx") {
        process_js_reference(node, node_type);
    } else if (ext_ == ".py") {
        process_python_reference(node, node_type);
    } else if (is_cpp_family_extension(ext_)) {
        if (node_type == "call_expression") {
            TSNode func = ts_node_child_by_field_name(
                node, "function",
                static_cast<uint32_t>(std::strlen("function")));
            if (!ts_node_is_null(func)) {
                references_.push_back(create_reference(
                    pick_cpp_reference_leaf(func), ReferenceType::Call,
                    RefStrength::Tight));
            }
        } else if ((node_type == "type_identifier" ||
                    node_type == "qualified_identifier" ||
                    node_type == "scoped_identifier") &&
                   !is_cpp_type_declaration_name_context(node)) {
            references_.push_back(create_reference(
                pick_cpp_reference_leaf(node), ReferenceType::Usage,
                RefStrength::Loose));
        }
    }
}

namespace {
// Bare Go type name from a decorated type token: "*chi.Mux"/"[]Mux" -> "Mux".
std::string go_bare_type(std::string_view t) {
    size_t i = 0;
    while (i < t.size() &&
           (t[i] == '*' || t[i] == '&' || t[i] == '[' || t[i] == ']'))
        ++i;
    t = t.substr(i);
    if (auto d = t.rfind('.'); d != std::string_view::npos) t = t.substr(d + 1);
    return std::string(t);
}

// Bare JS/TS type from an annotation/type node: strips ": ", generics
// (Foo<Bar> -> Foo), array suffix (Foo[] -> Foo), and qualifier (ns.Foo -> Foo).
std::string js_bare_type(std::string_view t) {
    while (!t.empty() && (t.front() == ':' || t.front() == ' '))
        t.remove_prefix(1);
    if (auto a = t.find('<'); a != std::string_view::npos) t = t.substr(0, a);
    if (auto b = t.find('['); b != std::string_view::npos) t = t.substr(0, b);
    while (!t.empty() && (t.back() == ' ')) t.remove_suffix(1);
    if (auto d = t.rfind('.'); d != std::string_view::npos) t = t.substr(d + 1);
    return std::string(t);
}

// Bare Python type from an annotation: strips quotes (string annotations),
// subscripts (List[Foo] -> List), and module qualifier (mod.Foo -> Foo).
std::string py_bare_type(std::string_view t) {
    while (!t.empty() && (t.front() == '"' || t.front() == '\'' || t.front() == ' '))
        t.remove_prefix(1);
    while (!t.empty() && (t.back() == '"' || t.back() == '\'' || t.back() == ' '))
        t.remove_suffix(1);
    if (auto b = t.find('['); b != std::string_view::npos) t = t.substr(0, b);
    if (auto d = t.rfind('.'); d != std::string_view::npos) t = t.substr(d + 1);
    while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
    return std::string(t);
}
}  // namespace

std::string UnifiedExtractor::enclosing_class_name() const {
    for (auto it = scope_stack_.rbegin(); it != scope_stack_.rend(); ++it) {
        if (it->scope_type == ScopeType::Class) return it->name;
    }
    return {};
}

// Seed the per-function local type env from the receiver (methods) and typed
// parameters. Cleared each function/method so types don't leak across funcs.
void UnifiedExtractor::seed_go_local_types(TSNode fn, bool is_method) {
    local_var_types_.clear();
    auto add_plist = [&](TSNode plist) {
        if (ts_node_is_null(plist)) return;
        uint32_t n = ts_node_named_child_count(plist);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode pd = ts_node_named_child(plist, i);
            const char* pt = ts_node_type(pd);
            if (!pt || std::string_view(pt) != "parameter_declaration") continue;
            TSNode ty = ts_node_child_by_field_name(pd, "type", 4);
            if (ts_node_is_null(ty)) continue;
            std::string tn = go_bare_type(node_text(ty));
            if (tn.empty()) continue;
            uint32_t cc = ts_node_named_child_count(pd);
            for (uint32_t j = 0; j < cc; ++j) {
                TSNode c = ts_node_named_child(pd, j);
                const char* ct = ts_node_type(c);
                if (ct && std::string_view(ct) == "identifier")
                    local_var_types_[std::string(node_text(c))] = tn;
            }
        }
    };
    if (is_method)
        add_plist(ts_node_child_by_field_name(fn, "receiver",
                                              static_cast<uint32_t>(8)));
    add_plist(ts_node_child_by_field_name(fn, "parameters",
                                          static_cast<uint32_t>(10)));
}

// Record `var x T` and `x := T{}` / `x := &T{}` into the local type env.
void UnifiedExtractor::record_go_local_var(TSNode decl) {
    const char* dt = ts_node_type(decl);
    std::string_view t(dt ? dt : "");
    if (t == "var_declaration") {
        uint32_t n = ts_node_named_child_count(decl);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode spec = ts_node_named_child(decl, i);
            TSNode ty = ts_node_child_by_field_name(spec, "type",
                                                    static_cast<uint32_t>(4));
            if (ts_node_is_null(ty)) continue;
            std::string tn = go_bare_type(node_text(ty));
            if (tn.empty()) continue;
            uint32_t cc = ts_node_named_child_count(spec);
            for (uint32_t j = 0; j < cc; ++j) {
                TSNode c = ts_node_named_child(spec, j);
                const char* ct = ts_node_type(c);
                if (ct && std::string_view(ct) == "identifier")
                    local_var_types_[std::string(node_text(c))] = tn;
            }
        }
    } else if (t == "short_var_declaration") {
        TSNode left = ts_node_child_by_field_name(decl, "left",
                                                  static_cast<uint32_t>(4));
        TSNode right = ts_node_child_by_field_name(decl, "right",
                                                   static_cast<uint32_t>(5));
        if (ts_node_is_null(left) || ts_node_is_null(right)) return;
        if (ts_node_named_child_count(left) != 1 ||
            ts_node_named_child_count(right) != 1)
            return;  // base case: single binding
        TSNode lid = ts_node_named_child(left, 0);
        TSNode rex = ts_node_named_child(right, 0);
        const char* lt = ts_node_type(lid);
        if (!lt || std::string_view(lt) != "identifier") return;
        const char* rt = ts_node_type(rex);
        std::string_view r(rt ? rt : "");
        if (r == "unary_expression") {  // &T{}
            TSNode op = ts_node_named_child(rex, 0);
            if (!ts_node_is_null(op)) {
                rex = op;
                rt = ts_node_type(rex);
                r = rt ? rt : "";
            }
        }
        if (r == "composite_literal") {  // T{...}
            TSNode ty = ts_node_child_by_field_name(rex, "type",
                                                    static_cast<uint32_t>(4));
            if (!ts_node_is_null(ty)) {
                std::string tn = go_bare_type(node_text(ty));
                if (!tn.empty())
                    local_var_types_[std::string(node_text(lid))] = tn;
            }
        }
    }
}

void UnifiedExtractor::process_go_reference(TSNode node,
                                            std::string_view node_type) {
    auto is_handled = [&](TSNode n) {
        uintptr_t id = reinterpret_cast<uintptr_t>(n.id);
        for (const auto& h : handled_nodes_) {
            if (h.id == id) return true;
        }
        return false;
    };

    // Maintain the local type env (SCIP base case). func_literal (closures)
    // deliberately does NOT clear — it inherits the enclosing function's types.
    if (node_type == "function_declaration") {
        seed_go_local_types(node, false);
        return;
    }
    if (node_type == "method_declaration") {
        seed_go_local_types(node, true);
        return;
    }
    if (node_type == "short_var_declaration" ||
        node_type == "var_declaration") {
        record_go_local_var(node);
        return;
    }

    if (node_type == "call_expression") {
        TSNode func = ts_node_child_by_field_name(
            node, "function",
            static_cast<uint32_t>(std::strlen("function")));
        if (ts_node_is_null(func)) return;

        // For a method / qualified call `x.M(...)` (or `pkg.F(...)`) the call
        // target is the method name M, not the whole `x.M` selector — and
        // create_reference names a ref by node text, so a Call ref on the
        // selector is named "x.M" and never resolves to the symbol M. Tag the
        // FIELD (M) as the Call so referenced_name == "M" resolves to the
        // method/function symbol and the call site shows up as a CALLER. Mark
        // the field handled so the selector_expression / field_identifier
        // branches below don't also emit a Usage ref for it (double-count, and
        // — the original bug — the only ref that resolved to M was that Usage,
        // which get_caller_names filters out, so methods had zero callers).
        const char* ftype = ts_node_type(func);
        if (ftype && std::string_view(ftype) == "selector_expression") {
            TSNode field = ts_node_child_by_field_name(
                func, "field", static_cast<uint32_t>(std::strlen("field")));
            if (!ts_node_is_null(field)) {
                handled_nodes_.push_back(
                    {reinterpret_cast<uintptr_t>(field.id)});
                Reference cref =
                    create_reference(field, ReferenceType::Call,
                                     RefStrength::Tight);
                // Receiver-type qualification (SCIP base case): if the receiver
                // is a local identifier whose type we know, emit "Type.M" so the
                // resolver selects the exact method among same-named candidates.
                TSNode operand = ts_node_child_by_field_name(
                    func, "operand", static_cast<uint32_t>(7));
                if (!ts_node_is_null(operand)) {
                    const char* ot = ts_node_type(operand);
                    if (ot && std::string_view(ot) == "identifier") {
                        auto it = local_var_types_.find(
                            std::string(node_text(operand)));
                        if (it != local_var_types_.end() &&
                            !it->second.empty()) {
                            cref.referenced_name =
                                it->second + "." +
                                std::string(node_text(field));
                        }
                    }
                }
                references_.push_back(std::move(cref));
                return;
            }
        }
        references_.push_back(
            create_reference(func, ReferenceType::Call, RefStrength::Tight));
    } else if (node_type == "selector_expression") {
        TSNode field = ts_node_child_by_field_name(
            node, "field", static_cast<uint32_t>(std::strlen("field")));
        if (!ts_node_is_null(field) && !is_handled(field)) {
            references_.push_back(
                create_reference(field, ReferenceType::Usage, RefStrength::Loose));
        }
    } else if (node_type == "type_identifier" ||
               node_type == "field_identifier") {
        if (!is_handled(node)) {
            references_.push_back(
                create_reference(node, ReferenceType::Usage, RefStrength::Loose));
        }
    }
}

void UnifiedExtractor::process_js_reference(TSNode node,
                                            std::string_view node_type) {
    uintptr_t node_id = reinterpret_cast<uintptr_t>(node.id);
    auto is_handled = [&](TSNode n) {
        uintptr_t id = reinterpret_cast<uintptr_t>(n.id);
        for (const auto& h : handled_nodes_) {
            if (h.id == id) return true;
        }
        return false;
    };

    // Local type env (SCIP base case). this -> enclosing class; TS-annotated
    // params/vars (`x: T`, `(x: T)`) and `new T()` constructions.
    if (node_type == "method_definition" ||
        node_type == "function_declaration" ||
        node_type == "function_expression" || node_type == "arrow_function" ||
        node_type == "generator_function_declaration") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) local_var_types_["this"] = cls;
        TSNode params = ts_node_child_by_field_name(
            node, "parameters", static_cast<uint32_t>(10));
        if (!ts_node_is_null(params)) {
            uint32_t n = ts_node_named_child_count(params);
            for (uint32_t i = 0; i < n; ++i) {
                TSNode p = ts_node_named_child(params, i);
                TSNode ty = ts_node_child_by_field_name(
                    p, "type", static_cast<uint32_t>(4));
                TSNode pat = ts_node_child_by_field_name(
                    p, "pattern", static_cast<uint32_t>(7));
                if (!ts_node_is_null(ty) && !ts_node_is_null(pat)) {
                    const char* pt = ts_node_type(pat);
                    if (pt && std::string_view(pt) == "identifier") {
                        std::string tn = js_bare_type(node_text(ty));
                        if (!tn.empty())
                            local_var_types_[std::string(node_text(pat))] = tn;
                    }
                }
            }
        }
        // fall through (arrow/function bodies still get their refs walked)
    } else if (node_type == "variable_declarator") {
        TSNode nm = ts_node_child_by_field_name(node, "name",
                                                static_cast<uint32_t>(4));
        if (!ts_node_is_null(nm)) {
            const char* nt = ts_node_type(nm);
            if (nt && std::string_view(nt) == "identifier") {
                std::string ty;
                TSNode tann = ts_node_child_by_field_name(
                    node, "type", static_cast<uint32_t>(4));
                if (!ts_node_is_null(tann)) {
                    ty = js_bare_type(node_text(tann));
                } else {
                    TSNode val = ts_node_child_by_field_name(
                        node, "value", static_cast<uint32_t>(5));
                    if (!ts_node_is_null(val)) {
                        const char* vt = ts_node_type(val);
                        if (vt && std::string_view(vt) == "new_expression") {
                            TSNode ctor = ts_node_child_by_field_name(
                                val, "constructor", static_cast<uint32_t>(11));
                            if (!ts_node_is_null(ctor))
                                ty = js_bare_type(node_text(ctor));
                        }
                    }
                }
                if (!ty.empty())
                    local_var_types_[std::string(node_text(nm))] = ty;
            }
        }
    }

    if (node_type == "call_expression") {
        TSNode func = ts_node_child_by_field_name(
            node, "function",
            static_cast<uint32_t>(std::strlen("function")));
        if (ts_node_is_null(func)) return;
        // Method call obj.M(...): tag the PROPERTY (method name) as the Call so
        // it resolves to the method symbol (not the un-resolvable "obj.M"); the
        // member_expression branch then skips it. Qualify "Type.M" when obj's
        // type is known (this -> class, typed local).
        const char* ftype = ts_node_type(func);
        if (ftype && std::string_view(ftype) == "member_expression") {
            TSNode prop = ts_node_child_by_field_name(
                func, "property", static_cast<uint32_t>(std::strlen("property")));
            if (!ts_node_is_null(prop)) {
                handled_nodes_.push_back({reinterpret_cast<uintptr_t>(prop.id)});
                Reference cref = create_reference(prop, ReferenceType::Call,
                                                  RefStrength::Tight);
                TSNode obj = ts_node_child_by_field_name(
                    func, "object", static_cast<uint32_t>(6));
                if (!ts_node_is_null(obj)) {
                    auto it = local_var_types_.find(
                        std::string(node_text(obj)));  // "this" or ident
                    if (it != local_var_types_.end() && !it->second.empty())
                        cref.referenced_name =
                            it->second + "." + std::string(node_text(prop));
                }
                references_.push_back(std::move(cref));
                return;
            }
        }
        handled_nodes_.push_back({reinterpret_cast<uintptr_t>(func.id)});
        references_.push_back(
            create_reference(func, ReferenceType::Call, RefStrength::Tight));
    } else if (node_type == "member_expression") {
        TSNode prop = ts_node_child_by_field_name(
            node, "property",
            static_cast<uint32_t>(std::strlen("property")));
        if (!ts_node_is_null(prop) && !is_handled(prop)) {
            handled_nodes_.push_back(
                {reinterpret_cast<uintptr_t>(prop.id)});
            references_.push_back(
                create_reference(prop, ReferenceType::Usage, RefStrength::Loose));
        }
    } else if (node_type == "import_statement") {
        TSNode source = ts_node_child_by_field_name(
            node, "source", static_cast<uint32_t>(std::strlen("source")));
        if (!ts_node_is_null(source)) {
            references_.push_back(
                create_reference(source, ReferenceType::Import,
                                 RefStrength::Tight));
        }
    } else if (node_type == "identifier") {
        // Skip if already handled or in import context
        for (const auto& h : handled_nodes_) {
            if (h.id == node_id) return;
        }
        if (in_import_context_) return;
        references_.push_back(
            create_reference(node, ReferenceType::Usage, RefStrength::Loose));
    }
}

void UnifiedExtractor::process_python_reference(TSNode node,
                                                std::string_view node_type) {
    auto is_handled = [&](TSNode n) {
        uintptr_t id = reinterpret_cast<uintptr_t>(n.id);
        for (const auto& h : handled_nodes_) {
            if (h.id == id) return true;
        }
        return false;
    };

    // Local type env (SCIP base case). Python uses only UNAMBIGUOUS type
    // sources: self/cls -> enclosing class, and annotated params/vars
    // (`x: T`). `x = Foo()` is intentionally skipped — constructor vs factory
    // call is syntactically identical in Python, so it would mis-type.
    if (node_type == "function_definition") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) {
            local_var_types_["self"] = cls;
            local_var_types_["cls"] = cls;
        }
        TSNode params = ts_node_child_by_field_name(
            node, "parameters", static_cast<uint32_t>(10));
        if (!ts_node_is_null(params)) {
            uint32_t n = ts_node_named_child_count(params);
            for (uint32_t i = 0; i < n; ++i) {
                TSNode p = ts_node_named_child(params, i);
                const char* pt = ts_node_type(p);
                if (!pt || std::string_view(pt) != "typed_parameter") continue;
                TSNode ty = ts_node_child_by_field_name(
                    p, "type", static_cast<uint32_t>(4));
                TSNode nm = ts_node_named_child(p, 0);
                if (!ts_node_is_null(ty) && !ts_node_is_null(nm)) {
                    std::string tn = py_bare_type(node_text(ty));
                    if (!tn.empty())
                        local_var_types_[std::string(node_text(nm))] = tn;
                }
            }
        }
        return;
    }
    if (node_type == "assignment") {
        TSNode ty = ts_node_child_by_field_name(node, "type",
                                                static_cast<uint32_t>(4));
        TSNode lhs = ts_node_child_by_field_name(node, "left",
                                                 static_cast<uint32_t>(4));
        if (!ts_node_is_null(ty) && !ts_node_is_null(lhs)) {
            const char* lt = ts_node_type(lhs);
            if (lt && std::string_view(lt) == "identifier") {
                std::string tn = py_bare_type(node_text(ty));
                if (!tn.empty())
                    local_var_types_[std::string(node_text(lhs))] = tn;
            }
        }
        return;
    }

    if (node_type == "call") {
        TSNode func = ts_node_child_by_field_name(
            node, "function",
            static_cast<uint32_t>(std::strlen("function")));
        if (ts_node_is_null(func)) return;
        // Method call obj.M(...): tag the attribute (method name) as the Call
        // (was a Call on the un-resolvable "obj.M" + a Usage on "M", so methods
        // had no callers). Qualify "Type.M" when obj's type is known.
        const char* ftype = ts_node_type(func);
        if (ftype && std::string_view(ftype) == "attribute") {
            TSNode attr = ts_node_child_by_field_name(
                func, "attribute", static_cast<uint32_t>(std::strlen("attribute")));
            if (!ts_node_is_null(attr)) {
                handled_nodes_.push_back({reinterpret_cast<uintptr_t>(attr.id)});
                Reference cref = create_reference(attr, ReferenceType::Call,
                                                  RefStrength::Tight);
                TSNode obj = ts_node_child_by_field_name(
                    func, "object", static_cast<uint32_t>(6));
                if (!ts_node_is_null(obj)) {
                    const char* ot = ts_node_type(obj);
                    if (ot && std::string_view(ot) == "identifier") {
                        auto it = local_var_types_.find(
                            std::string(node_text(obj)));
                        if (it != local_var_types_.end() && !it->second.empty())
                            cref.referenced_name =
                                it->second + "." + std::string(node_text(attr));
                    }
                }
                references_.push_back(std::move(cref));
                return;
            }
        }
        references_.push_back(
            create_reference(func, ReferenceType::Call, RefStrength::Tight));
    } else if (node_type == "attribute") {
        TSNode attr = ts_node_child_by_field_name(
            node, "attribute",
            static_cast<uint32_t>(std::strlen("attribute")));
        if (!ts_node_is_null(attr) && !is_handled(attr)) {
            references_.push_back(
                create_reference(attr, ReferenceType::Usage, RefStrength::Loose));
        }
    } else if (node_type == "identifier") {
        if (!is_handled(node)) {
            references_.push_back(
                create_reference(node, ReferenceType::Usage, RefStrength::Loose));
        }
    }
}

Reference UnifiedExtractor::create_reference(TSNode node,
                                             ReferenceType ref_type,
                                             RefStrength strength) {
    TSPoint start = ts_node_start_point(node);

    Reference ref;
    ref.id = ref_id_++;
    ref.file_id = file_id_;
    ref.line = static_cast<int>(start.row) + 1;
    ref.column = static_cast<int>(start.column) + 1;
    ref.type = ref_type;
    ref.strength = strength;
    ref.referenced_name = std::string(node_text(node));
    return ref;
}

// ---------------------------------------------------------------------------
// Complexity tracking
// ---------------------------------------------------------------------------

void UnifiedExtractor::count_complexity_point(TSNode node,
                                              std::string_view node_type) {
    if (complexity_stack_.empty()) return;
    int& top = complexity_stack_.back();

    if (node_type == "if_statement" || node_type == "if_expression") {
        ++top;
    } else if (node_type == "for_statement" ||
               node_type == "for_range_statement" ||
               node_type == "for_in_statement" ||
               node_type == "while_statement" ||
               node_type == "do_while_statement") {
        ++top;
    } else if (node_type == "case_clause" || node_type == "case_statement" ||
               node_type == "expression_case" ||
               node_type == "type_case") {
        ++top;
    } else if (node_type == "conditional_expression" ||
               node_type == "ternary_expression") {
        ++top;
    } else if (node_type == "catch_clause" ||
               node_type == "except_clause") {
        ++top;
    } else if (node_type == "binary_expression") {
        if (ts_node_child_count(node) >= 3) {
            TSNode op = ts_node_child(node, 1);
            if (!ts_node_is_null(op)) {
                std::string_view op_type = get_node_type(op);
                if (op_type == "&&" || op_type == "||" ||
                    op_type == "and" || op_type == "or") {
                    ++top;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ExtractorPool
// ---------------------------------------------------------------------------

UnifiedExtractor* ExtractorPool::acquire(std::string_view content,
                                         FileID file_id,
                                         std::string_view ext,
                                         std::string_view path) {
    UnifiedExtractor* ue = nullptr;
    if (!pool_.empty()) {
        ue = pool_.back().release();
        pool_.pop_back();
    } else {
        ue = new UnifiedExtractor();
    }
    ue->init(content, file_id, ext, path);
    return ue;
}

void ExtractorPool::release(UnifiedExtractor* extractor) {
    if (!extractor) return;
    extractor->reset();
    pool_.emplace_back(extractor);
}

ExtractorPool& thread_extractor_pool() {
    static thread_local ExtractorPool pool;
    return pool;
}

}  // namespace lci::parser
