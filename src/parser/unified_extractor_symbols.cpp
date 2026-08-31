#include <lci/parser/parser.h>
#include "unified_extractor_internal.h"
#include <lci/parser/unified_extractor.h>
#include <lci/analysis/finding_suppressions.h>

#include <lci/language_map.h>

#include <lci/analysis/side_effect_analyzer.h>

#include <absl/container/flat_hash_set.h>
#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

namespace lci::parser {


uint8_t UnifiedExtractor::count_parameter_names(TSNode node) {
    // The "parameters" field name is shared by the Go / JS / TS / Python /
    // Java / C# / PHP / Rust / Kotlin function-and-method grammars (same
    // contract register_function_signature relies on); C++ nests it behind
    // the function_definition -> (nested) declarator chain.
    auto field = [](TSNode n, const char* name) {
        return ts_node_child_by_field_name(
            n, name, static_cast<uint32_t>(std::strlen(name)));
    };
    TSNode params = field(node, "parameters");
    if (ts_node_is_null(params)) {
        TSNode decl = field(node, "declarator");
        while (!ts_node_is_null(decl)) {
            params = field(decl, "parameters");
            if (!ts_node_is_null(params)) break;
            decl = field(decl, "declarator");
        }
    }
    if (ts_node_is_null(params)) {
        // Kotlin's grammar is fieldless: the container is a named CHILD of
        // type function_value_parameters. Without it every Kotlin function
        // counted 0 parameters, so arity-preferring resolution never fired.
        uint32_t n = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode c = ts_node_named_child(node, i);
            if (get_node_type(c) == "function_value_parameters") {
                params = c;
                break;
            }
        }
    }
    if (ts_node_is_null(params)) return 0;

    // One parameter node may declare several names (Go's `a, b int`), so
    // count `name`-field children per parameter when the grammar provides
    // them and fall back to one name per non-comment parameter node.
    int count = 0;
    uint32_t n = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < n; ++i) {
        TSNode p = ts_node_named_child(params, i);
        if (get_node_type(p) == "comment") continue;
        int names = 0;
        TSTreeCursor cursor = ts_tree_cursor_new(p);
        if (ts_tree_cursor_goto_first_child(&cursor)) {
            do {
                const char* f = ts_tree_cursor_current_field_name(&cursor);
                if (f != nullptr && std::strcmp(f, "name") == 0) ++names;
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
        }
        ts_tree_cursor_delete(&cursor);
        count += names > 0 ? names : 1;
    }
    return static_cast<uint8_t>(std::min(count, 255));
}

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

    // C++ function_definition: declarator -> declarator. Pointer/reference
    // return types wrap the function_declarator in pointer_declarator /
    // reference_declarator layers; without peeling them, `void* f(size_t)`
    // extracted the NAME "f(size_t bytes)" — a symbol no reference can
    // ever resolve to by name.
    if (name.empty()) {
        TSNode decl = ts_node_child_by_field_name(
            node, "declarator",
            static_cast<uint32_t>(std::strlen("declarator")));
        while (!ts_node_is_null(decl)) {
            std::string_view dt(ts_node_type(decl));
            if (dt != "pointer_declarator" && dt != "reference_declarator")
                break;
            decl = ts_node_child_by_field_name(
                decl, "declarator",
                static_cast<uint32_t>(std::strlen("declarator")));
        }
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
    sym.parameter_count = count_parameter_names(node);
    sym.visibility = effective_visibility(node);
    sym.test_scaffold = ext_ == ".rs" && is_rust_test_scaffold(node);
    symbols_.push_back(std::move(sym));
}

// Rust inline test scaffolding: an item under a `mod tests` ancestor, or
// carrying (or under a mod carrying) a #[cfg(test)] attribute. Preceding
// attribute_item siblings hold the attributes in tree-sitter-rust.
bool UnifiedExtractor::is_rust_test_scaffold(TSNode node) {
    auto has_cfg_test_attr = [&](TSNode item) {
        for (TSNode sib = ts_node_prev_named_sibling(item);
             !ts_node_is_null(sib);
             sib = ts_node_prev_named_sibling(sib)) {
            std::string_view st = get_node_type(sib);
            if (st != "attribute_item") break;
            if (node_text(sib).find("cfg(test") != std::string_view::npos)
                return true;
        }
        return false;
    };
    if (has_cfg_test_attr(node)) return true;
    for (TSNode p = ts_node_parent(node); !ts_node_is_null(p);
         p = ts_node_parent(p)) {
        if (get_node_type(p) != "mod_item") continue;
        TSNode nm =
            ts_node_child_by_field_name(p, "name", static_cast<uint32_t>(4));
        if (!ts_node_is_null(nm) && node_text(nm) == "tests") return true;
        if (has_cfg_test_attr(p)) return true;
    }
    return false;
}

// Declared visibility adjusted for Rust semantics: no `pub` means
// crate-private, and anything inside a `mod tests` block is test scaffolding
// (ripgrep audit: #[cfg(test)] helpers dominated entry points, smells, and
// vocabulary because they all counted as exported).
SymbolVisibility UnifiedExtractor::effective_visibility(TSNode node) {
    SymbolVisibility v = scan_declared_visibility(node);
    if (ext_ == ".rs") {
        if (is_rust_test_scaffold(node)) return SymbolVisibility::Private;
        if (v == SymbolVisibility::Default) return SymbolVisibility::Private;
    }
    return v;
}

// Declared visibility from modifier children (PHP visibility_modifier, C#/
// Java modifier tokens, TS accessibility_modifier, Kotlin modifiers). This
// field previously had NO writer anywhere, so every private method counted
// as exported API (guzzle claimed 360 exported vs ~202 actual public
// functions — 2026-08-26 re-panel finding).
SymbolVisibility UnifiedExtractor::scan_declared_visibility(TSNode node) {
    auto classify = [](std::string_view t) {
        if (t == "private") return SymbolVisibility::Private;
        if (t == "protected") return SymbolVisibility::Protected;
        if (t == "public") return SymbolVisibility::Public;
        if (t == "internal") return SymbolVisibility::Internal;
        // Rust: `pub` is public; `pub(crate)`/`pub(super)`/`pub(in ...)`
        // are crate-internal, not exported API (ripgrep surfaced a
        // pub(crate) parser as its top entry point).
        if (t == "pub") return SymbolVisibility::Public;
        if (t.rfind("pub(", 0) == 0) return SymbolVisibility::Internal;
        return SymbolVisibility::Default;
    };
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        TSNode c = ts_node_child(node, i);
        std::string_view ct = get_node_type(c);
        // Modifiers precede the body in every grammar we handle.
        if (ct == "block" || ct == "compound_statement" ||
            ct == "statement_block")
            break;
        // Wrapper nodes: the keyword is the node's TEXT (PHP
        // visibility_modifier) or a child token (Java/Kotlin modifiers).
        if (ct == "visibility_modifier" || ct == "accessibility_modifier") {
            auto v = classify(node_text(c));
            if (v != SymbolVisibility::Default) return v;
        } else if (ct == "modifiers" || ct == "modifier") {
            uint32_t m = ts_node_child_count(c);
            for (uint32_t j = 0; j < m; ++j) {
                auto v = classify(get_node_type(ts_node_child(c, j)));
                if (v != SymbolVisibility::Default) return v;
            }
        } else {
            // Bare keyword tokens (C# grammar emits them as direct children).
            auto v = classify(ct);
            if (v != SymbolVisibility::Default) return v;
        }
    }
    return SymbolVisibility::Default;
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
    sym.parameter_count = count_parameter_names(node);
    sym.visibility = effective_visibility(node);
    sym.test_scaffold = ext_ == ".rs" && is_rust_test_scaffold(node);
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
    sym.parameter_count = count_parameter_names(node);
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
    sym.parameter_count = count_parameter_names(node);
    sym.visibility = effective_visibility(node);
    sym.test_scaffold = ext_ == ".rs" && is_rust_test_scaffold(node);
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
    func_sym.parameter_count = count_parameter_names(func_node);
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
    // `type ( ... )` declares several types at once. The scan used to stop at
    // the first type_spec, so every member after it vanished, and the one it
    // kept was stamped with the DECLARATION's span — pointing a reader at
    // `type (` for a type well below it. Each spec is its own symbol with its
    // own span; a single-line `type Foo struct{}` has exactly one and is
    // unaffected.
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode spec = ts_node_child(node, i);
        if (get_node_type(spec) != "type_spec") continue;

        std::string name;
        SymbolType sym_type = SymbolType::Type;
        BlockType blk_type = BlockType::Other;
        uint32_t spec_count = ts_node_child_count(spec);
        for (uint32_t j = 0; j < spec_count; ++j) {
            TSNode sc = ts_node_child(spec, j);
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
        if (name.empty()) continue;

        TSPoint start = ts_node_start_point(spec);
        TSPoint end = ts_node_end_point(spec);

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
    const bool is_const = node_type == "const_declaration";
    const std::string_view spec_type = is_const ? "const_spec" : "var_spec";
    const SymbolType st =
        is_const ? SymbolType::Constant : SymbolType::Variable;

    // A grouped declaration — `var ( ... )` — nests its specs inside a list
    // node, so they are NOT direct children the way a single-line `var x = 1`
    // spec is. Scanning only depth 1 dropped every grouped name: gin's
    // binding/form_mapping.go indexed nothing before line 32, losing the
    // three package-level errors above it, while references to them still
    // resolved. Descend instead of matching the wrapper by name, which keeps
    // this working if the grammar renames or re-nests the list.
    std::vector<TSNode> stack{node};
    std::vector<TSNode> specs;
    while (!stack.empty()) {
        TSNode n = stack.back();
        stack.pop_back();
        uint32_t count = ts_node_child_count(n);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(n, i);
            std::string_view ct = get_node_type(child);
            if (ct == spec_type) {
                specs.push_back(child);
            } else if (!ts_node_is_named(child) || ct.find("list") !=
                                                       std::string_view::npos) {
                // Punctuation and the spec-list wrapper; nothing else can
                // hold a spec, so the walk stays shallow.
                stack.push_back(child);
            }
        }
    }
    // Source order: the stack walk visits children back to front.
    std::sort(specs.begin(), specs.end(), [](TSNode a, TSNode b) {
        return ts_node_start_byte(a) < ts_node_start_byte(b);
    });

    for (TSNode spec : specs) {
        // Each name carries its OWN span. The declaration's span covers the
        // whole block, which would point a reader at `var (` for a symbol
        // well below it.
        TSPoint start = ts_node_start_point(spec);
        TSPoint end = ts_node_end_point(spec);
        uint32_t spec_count = ts_node_child_count(spec);
        for (uint32_t j = 0; j < spec_count; ++j) {
            TSNode sc = ts_node_child(spec, j);
            if (get_node_type(sc) != "identifier") continue;
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
    PositionKey key{static_cast<int>(start.row),
                    static_cast<int>(start.column)};

    DeclarationInfo info;
    info.signature = std::move(signature);
    info.doc_comment = std::move(doc_comment);
    // try_emplace keeps first-wins, matching the old front-to-back scan.
    declarations_.try_emplace(key, std::move(info));
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

    // The bare names (if, while, when, rescue, conditional, binary, …) are
    // tree-sitter-ruby's; no other bundled grammar emits them, so they cannot
    // double-count elsewhere.
    if (node_type == "if_statement" || node_type == "if_expression" ||
        node_type == "if" || node_type == "unless" || node_type == "elsif" ||
        node_type == "if_modifier" || node_type == "unless_modifier") {
        ++top;
    } else if (node_type == "for_statement" ||
               node_type == "for_range_statement" ||
               node_type == "for_in_statement" ||
               node_type == "while_statement" ||
               node_type == "do_while_statement" ||
               node_type == "for" || node_type == "while" ||
               node_type == "until" ||
               node_type == "while_modifier" ||
               node_type == "until_modifier") {
        ++top;
    } else if (node_type == "case_clause" || node_type == "case_statement" ||
               node_type == "expression_case" ||
               node_type == "type_case" ||
               node_type == "when" || node_type == "in_clause") {
        ++top;
    } else if (node_type == "conditional_expression" ||
               node_type == "ternary_expression" ||
               node_type == "conditional") {
        ++top;
    } else if (node_type == "catch_clause" ||
               node_type == "except_clause" ||
               node_type == "rescue" || node_type == "rescue_modifier") {
        ++top;
    } else if (node_type == "binary_expression" ||
               node_type == "binary") {
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
// C/C++ locals + parameters
// ---------------------------------------------------------------------------

bool UnifiedExtractor::is_c_family() const {
    return ext_ == ".c" || ext_ == ".h" || ext_ == ".cpp" || ext_ == ".hpp" ||
           ext_ == ".cc" || ext_ == ".hh" || ext_ == ".cxx" || ext_ == ".hxx";
}

TSNode UnifiedExtractor::declarator_identifier(TSNode declarator) {
    TSNode n = declarator;
    for (int depth = 0; depth < 8 && !ts_node_is_null(n); ++depth) {
        std::string_view t = get_node_type(n);
        if (t == "identifier" || t == "field_identifier") return n;
        // A function_declarator here means a prototype/function, not a
        // variable — never dig through it.
        if (t == "function_declarator") return TSNode{};
        TSNode inner = ts_node_child_by_field_name(
            n, "declarator", static_cast<uint32_t>(std::strlen("declarator")));
        if (ts_node_is_null(inner)) {
            // reference_declarator has no field in some grammar versions;
            // fall back to the first named child.
            if (t == "reference_declarator" || t == "pointer_declarator" ||
                t == "parenthesized_declarator") {
                if (ts_node_named_child_count(n) == 0) return TSNode{};
                inner = ts_node_named_child(n, 0);
            } else {
                return TSNode{};
            }
        }
        n = inner;
    }
    return TSNode{};
}

namespace {

// True when `node` sits inside a function DEFINITION (or lambda) body /
// signature rather than a bare prototype or field declaration. Prototype
// parameters must not become symbols: a header full of declarations would
// scatter Variable symbols through class scopes, where the class_variables
// getter would misread them as members.
bool inside_function_definition(TSNode node) {
    TSNode p = ts_node_parent(node);
    for (int depth = 0; depth < 24 && !ts_node_is_null(p); ++depth) {
        std::string_view t = ts_node_type(p);
        // A body reached from below: we are inside real function code.
        // (Locals sit in declaration -> compound_statement, so plain
        // "declaration" is NOT decisive on its own.)
        if (t == "compound_statement" || t == "function_definition" ||
            t == "lambda_expression") {
            return true;
        }
        // File / namespace / class level reached without crossing a body:
        // prototype parameter or global — not this extractor's business.
        if (t == "translation_unit" || t == "namespace_definition" ||
            t == "field_declaration_list") {
            return false;
        }
        p = ts_node_parent(p);
    }
    return false;
}

}  // namespace

void UnifiedExtractor::extract_cpp_parameter(TSNode node) {
    if (!inside_function_definition(node)) return;
    TSNode decl = ts_node_child_by_field_name(
        node, "declarator", static_cast<uint32_t>(std::strlen("declarator")));
    if (ts_node_is_null(decl)) return;  // unnamed parameter
    TSNode id = declarator_identifier(decl);
    if (ts_node_is_null(id)) return;
    std::string_view name = node_text(id);
    if (name.empty()) return;

    TSPoint start = ts_node_start_point(id);
    TSPoint end = ts_node_end_point(id);
    int line = static_cast<int>(start.row) + 1;

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Variable;
    sym.file_id = file_id_;
    sym.line = line;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));

    // Parameter marker scope (context_lookup_variables trap 6d): a
    // Variable-type scope whose name equals the symbol's own name marks it
    // as a parameter. This was previously producer-less in the port, so
    // only same-line-as-function parameters classified; wrapped signatures
    // mis-bucketed parameters as locals.
    ScopeInfo scope;
    scope.type = ScopeType::Variable;
    scope.name = std::string(name);
    scope.full_path = build_full_qualified_name(scope.name);
    scope.start_line = line;
    scope.end_line = line;
    scope.level = current_level_;
    scopes_.push_back(std::move(scope));
}

void UnifiedExtractor::extract_cpp_init_declarator(TSNode node) {
    if (!inside_function_definition(node)) return;
    TSNode decl = ts_node_child_by_field_name(
        node, "declarator", static_cast<uint32_t>(std::strlen("declarator")));
    if (ts_node_is_null(decl)) return;
    TSNode id = declarator_identifier(decl);
    if (ts_node_is_null(id)) return;
    std::string_view name = node_text(id);
    if (name.empty()) return;

    TSPoint start = ts_node_start_point(id);
    TSPoint end = ts_node_end_point(id);
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

void UnifiedExtractor::extract_cpp_local_declaration(TSNode node) {
    // Uninitialized locals ("int x;", "Foo a, b;"). Initialized declarators
    // are init_declarator nodes and get their own visit; anything reaching
    // a function_declarator is a prototype and is skipped by
    // declarator_identifier.
    if (!inside_function_definition(node)) return;
    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        TSNode child = ts_node_named_child(node, i);
        std::string_view t = get_node_type(child);
        if (t == "init_declarator") continue;  // separate visit
        if (t != "identifier" && t != "pointer_declarator" &&
            t != "reference_declarator" && t != "array_declarator" &&
            t != "parenthesized_declarator") {
            continue;  // type nodes, qualifiers
        }
        TSNode id = declarator_identifier(child);
        if (ts_node_is_null(id)) continue;
        std::string_view name = node_text(id);
        if (name.empty()) continue;
        TSPoint start = ts_node_start_point(id);
        TSPoint end = ts_node_end_point(id);
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
}

}  // namespace lci::parser
