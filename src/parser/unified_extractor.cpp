#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <lci/analysis/side_effect_analyzer.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

namespace lci::parser {

namespace {

// First named child of the given type, or a null node. Used to recover names
// from fieldless grammars (tree-sitter-kotlin exposes no `name` field).
TSNode first_named_child_typed(TSNode node, std::string_view type) {
    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        TSNode c = ts_node_named_child(node, i);
        if (std::string_view(ts_node_type(c)) == type) return c;
    }
    return TSNode{};
}

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
    se_func_depth_ = 0;
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

    side_effects_ = nullptr;
    se_func_depth_ = 0;

    handled_nodes_.clear();
    local_var_types_.clear();
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

        // Side-effect lifecycle: open a per-function context on entry to the
        // outermost tracked function. Nested functions/closures fold their
        // effects into the enclosing context (analyzer holds one context).
        if (side_effects_) {
            if (se_func_depth_ == 0) {
                std::string_view fname = extract_function_name(node, node_type);
                int start_line = static_cast<int>(start.row) + 1;
                int end_line = static_cast<int>(ts_node_end_point(node).row) + 1;
                side_effects_->begin_function(fname, path_, start_line,
                                              end_line);
                register_function_signature(node, node_type);
            }
            ++se_func_depth_;
        }
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

    // === SIDE EFFECT EXTRACTION ===
    // Only when a sink is attached (mcp side-effect pass). Zero cost on the
    // hot indexing path where side_effects_ is null.
    if (side_effects_) {
        process_side_effect_node(node, node_type);
    }

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

    // Close the side-effect context when the outermost tracked function exits.
    if (is_func && side_effects_ && se_func_depth_ > 0) {
        if (--se_func_depth_ == 0) {
            side_effects_->end_function();
        }
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
        // Kotlin class_declaration is fieldless (type_identifier child).
        if (ts_node_is_null(n) && ext_ == ".kt")
            n = first_named_child_typed(node, "type_identifier");
        if (!ts_node_is_null(n)) name = std::string(node_text(n));

    } else if (node_type == "type_declaration") {
        scope_type = ScopeType::Class;
        name = std::string(extract_go_type_name(node));

    } else if (node_type == "struct_specifier" ||
               node_type == "class_specifier" ||
               node_type == "union_specifier") {
        // C/C++ aggregate definition. Only a named specifier *with a body*
        // (field_declaration_list) opens a class scope — forward decls and
        // `struct A a;` uses carry no body and must not nest the surrounding
        // scope. Giving member methods a Class scope named after the aggregate
        // is what lets the resolver match a scope-typed `T.m` ref to the method
        // whose owning type is `T`. Fields are located by child iteration: the
        // grammar exposes `name` but the body has no stable field name here.
        std::string_view aggr_name;
        bool has_body = false;
        uint32_t cc = ts_node_child_count(node);
        for (uint32_t i = 0; i < cc; ++i) {
            TSNode c = ts_node_child(node, i);
            std::string_view ct(ts_node_type(c));
            if (ct == "type_identifier" && aggr_name.empty())
                aggr_name = node_text(c);
            else if (ct == "field_declaration_list")
                has_body = true;
        }
        if (!has_body || aggr_name.empty()) return false;
        scope_type = ScopeType::Class;
        name = std::string(aggr_name);

    } else if (node_type == "impl_item") {
        // Rust: `impl T { ... }`. Methods live in the impl block, and `self`
        // types to T, so the impl opens a Class scope named after the impl type
        // — giving each method an owning-type entry the resolver can match.
        TSNode ty = ts_node_child_by_field_name(node, "type",
                                                static_cast<uint32_t>(4));
        if (ts_node_is_null(ty)) return false;
        scope_type = ScopeType::Class;
        name = go_bare_type(node_text(ty));

    } else if (node_type == "struct_item") {
        // Rust struct definition (the type itself; methods are in impl_item).
        TSNode n = ts_node_child_by_field_name(node, "name",
                                               static_cast<uint32_t>(4));
        if (ts_node_is_null(n)) return false;
        scope_type = ScopeType::Class;
        name = std::string(node_text(n));

    } else if (ext_ == ".zig" && node_type == "variable_declaration") {
        // Zig: `const A = struct { ... };`. The container is an initializer of a
        // variable_declaration; name the Class scope after the const identifier
        // so member fns get an owning-type entry. Plain vars (no struct/union
        // child) are left to fall through as non-scoping.
        std::string_view zname;
        bool is_container = false;
        uint32_t cc = ts_node_child_count(node);
        for (uint32_t i = 0; i < cc; ++i) {
            TSNode c = ts_node_child(node, i);
            std::string_view ct(ts_node_type(c));
            if (ct == "identifier" && zname.empty())
                zname = node_text(c);
            else if (ct == "struct_declaration" || ct == "union_declaration")
                is_container = true;
        }
        if (!is_container || zname.empty()) return false;
        scope_type = ScopeType::Class;
        name = std::string(zname);

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

    // Kotlin function_declaration is fieldless: the name is a simple_identifier
    // child rather than a `name` field.
    if (name.empty() && ext_ == ".kt") {
        TSNode n = first_named_child_typed(node, "simple_identifier");
        if (!ts_node_is_null(n)) name = node_text(n);
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
    // Kotlin class_declaration is fieldless: the name is a type_identifier child.
    if (ts_node_is_null(name_node) && ext_ == ".kt")
        name_node = first_named_child_typed(node, "type_identifier");
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
