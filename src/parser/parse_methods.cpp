#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <tree_sitter/api.h>

#include <cstring>

namespace lci::parser {

// ---------------------------------------------------------------------------
// Language-specific parse dispatch methods.
//
// The UnifiedExtractor uses direct AST traversal (visit_node) rather than
// tree-sitter query compilation. This is functionally equivalent to the Go
// implementation's query-based approach but avoids the overhead of query
// pattern compilation. Each language's node types are matched in
// process_symbol_node and dispatched to the appropriate extract_* method.
//
// This file adds extraction methods for languages not fully covered by
// the core unified_extractor.cpp: Java, C#, PHP, Kotlin, Zig, Ruby.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Java extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_java_constructor(TSNode node) {
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
    block.type = BlockType::Constructor;
    block.name = std::string(name);
    blocks_.push_back(std::move(block));

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Constructor;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_annotation_type(TSNode node) {
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
    sym.type = SymbolType::Annotation;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_java_import(TSNode node) {
    Import imp;
    imp.path = std::string(node_text(node));
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

void UnifiedExtractor::extract_package_declaration(TSNode node) {
    Import imp;
    imp.path = std::string(node_text(node));
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

// ---------------------------------------------------------------------------
// C# extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_record(TSNode node) {
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
    sym.type = SymbolType::Record;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_csharp_property(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

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

void UnifiedExtractor::extract_using_directive(TSNode node) {
    // C# using directive: extract the qualified name
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view ct = get_node_type(child);
        if (ct == "qualified_name" || ct == "identifier") {
            Import imp;
            imp.path = std::string(node_text(child));
            imp.file_id = file_id_;
            imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
            imports_.push_back(std::move(imp));
            return;
        }
    }
}

void UnifiedExtractor::extract_delegate(TSNode node) {
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));
    if (ts_node_is_null(name_node)) return;
    std::string_view name = node_text(name_node);
    if (name.empty()) return;

    Symbol sym;
    sym.name = std::string(name);
    sym.type = SymbolType::Delegate;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_event(TSNode node) {
    // event_field_declaration contains variable_declaration > variable_declarator > identifier
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (get_node_type(child) == "variable_declaration") {
            uint32_t vcount = ts_node_child_count(child);
            for (uint32_t j = 0; j < vcount; ++j) {
                TSNode vchild = ts_node_child(child, j);
                if (get_node_type(vchild) == "variable_declarator") {
                    uint32_t dcount = ts_node_child_count(vchild);
                    for (uint32_t k = 0; k < dcount; ++k) {
                        TSNode dchild = ts_node_child(vchild, k);
                        if (get_node_type(dchild) == "identifier") {
                            TSPoint start = ts_node_start_point(node);
                            TSPoint end = ts_node_end_point(node);

                            Symbol sym;
                            sym.name = std::string(node_text(dchild));
                            sym.type = SymbolType::Event;
                            sym.file_id = file_id_;
                            sym.line = static_cast<int>(start.row) + 1;
                            sym.column = static_cast<int>(start.column) + 1;
                            sym.end_line = static_cast<int>(end.row) + 1;
                            sym.end_column = static_cast<int>(end.column) + 1;
                            symbols_.push_back(std::move(sym));
                            return;
                        }
                    }
                }
            }
        }
    }
}

void UnifiedExtractor::extract_csharp_field(TSNode node) {
    // field_declaration > variable_declaration > variable_declarator > identifier
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (get_node_type(child) == "variable_declaration") {
            uint32_t vcount = ts_node_child_count(child);
            for (uint32_t j = 0; j < vcount; ++j) {
                TSNode vchild = ts_node_child(child, j);
                if (get_node_type(vchild) == "variable_declarator") {
                    uint32_t dcount = ts_node_child_count(vchild);
                    for (uint32_t k = 0; k < dcount; ++k) {
                        TSNode dchild = ts_node_child(vchild, k);
                        if (get_node_type(dchild) == "identifier") {
                            TSPoint start = ts_node_start_point(node);
                            TSPoint end = ts_node_end_point(node);

                            Symbol sym;
                            sym.name = std::string(node_text(dchild));
                            sym.type = SymbolType::Field;
                            sym.file_id = file_id_;
                            sym.line = static_cast<int>(start.row) + 1;
                            sym.column = static_cast<int>(start.column) + 1;
                            sym.end_line = static_cast<int>(end.row) + 1;
                            sym.end_column = static_cast<int>(end.column) + 1;
                            symbols_.push_back(std::move(sym));
                            return;
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// PHP extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_php_trait(TSNode node) {
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

void UnifiedExtractor::extract_php_namespace(TSNode node) {
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

void UnifiedExtractor::extract_php_use(TSNode node) {
    Import imp;
    imp.path = std::string(node_text(node));
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

void UnifiedExtractor::extract_php_const(TSNode node) {
    // const_declaration may have name field or contain const_element children
    TSNode name_node = ts_node_child_by_field_name(
        node, "name", static_cast<uint32_t>(std::strlen("name")));

    if (!ts_node_is_null(name_node)) {
        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);

        Symbol sym;
        sym.name = std::string(node_text(name_node));
        sym.type = SymbolType::Constant;
        sym.file_id = file_id_;
        sym.line = static_cast<int>(start.row) + 1;
        sym.column = static_cast<int>(start.column) + 1;
        sym.end_line = static_cast<int>(end.row) + 1;
        sym.end_column = static_cast<int>(end.column) + 1;
        symbols_.push_back(std::move(sym));
        return;
    }

    // Fallback: look for const_element children
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view ct = get_node_type(child);
        if (ct == "const_element") {
            TSNode cname = ts_node_child_by_field_name(
                child, "name", static_cast<uint32_t>(std::strlen("name")));
            if (ts_node_is_null(cname)) continue;

            TSPoint start = ts_node_start_point(node);
            TSPoint end = ts_node_end_point(node);

            Symbol sym;
            sym.name = std::string(node_text(cname));
            sym.type = SymbolType::Constant;
            sym.file_id = file_id_;
            sym.line = static_cast<int>(start.row) + 1;
            sym.column = static_cast<int>(start.column) + 1;
            sym.end_line = static_cast<int>(end.row) + 1;
            sym.end_column = static_cast<int>(end.column) + 1;
            symbols_.push_back(std::move(sym));
        }
    }
}

// ---------------------------------------------------------------------------
// Zig extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_zig_struct(TSNode node) {
    // Zig structs are declared via variable_declaration:
    //   const MyStruct = struct { ... };
    // The identifier child holds the name, and a struct_declaration child
    // (or union_declaration) holds the body.
    std::string_view name;
    bool is_struct = false;

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view ct = get_node_type(child);
        if (ct == "identifier" && name.empty()) {
            name = node_text(child);
        } else if (ct == "struct_declaration" || ct == "union_declaration") {
            is_struct = true;
        }
    }

    if (name.empty() || !is_struct) return;

    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

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

// ---------------------------------------------------------------------------
// C/C++ additional extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_struct_specifier(TSNode node) {
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

void UnifiedExtractor::extract_enum_specifier(TSNode node) {
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

void UnifiedExtractor::extract_cpp_namespace(TSNode node) {
    // namespace_definition may have a name child or be anonymous
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

void UnifiedExtractor::extract_preproc_include(TSNode node) {
    TSNode path_node = ts_node_child_by_field_name(
        node, "path", static_cast<uint32_t>(std::strlen("path")));
    if (ts_node_is_null(path_node)) return;

    std::string path(node_text(path_node));
    // Remove surrounding quotes and angle brackets
    if (path.size() >= 2) {
        char first = path.front();
        if (first == '"' || first == '\'' || first == '<') {
            path = path.substr(1, path.size() - 2);
        }
    }

    Import imp;
    imp.path = std::move(path);
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

void UnifiedExtractor::extract_using_declaration(TSNode node) {
    Import imp;
    imp.path = std::string(node_text(node));
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

// ---------------------------------------------------------------------------
// Rust import extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_rust_use(TSNode node) {
    Import imp;
    imp.path = std::string(node_text(node));
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

// ---------------------------------------------------------------------------
// Kotlin extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_kotlin_object(TSNode node) {
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
    sym.type = SymbolType::Object;
    sym.file_id = file_id_;
    sym.line = static_cast<int>(start.row) + 1;
    sym.column = static_cast<int>(start.column) + 1;
    sym.end_line = static_cast<int>(end.row) + 1;
    sym.end_column = static_cast<int>(end.column) + 1;
    symbols_.push_back(std::move(sym));
}

void UnifiedExtractor::extract_kotlin_import(TSNode node) {
    Import imp;
    imp.path = std::string(node_text(node));
    imp.file_id = file_id_;
    imp.line = static_cast<int>(ts_node_start_point(node).row) + 1;
    imports_.push_back(std::move(imp));
}

// ---------------------------------------------------------------------------
// Ruby extraction
// ---------------------------------------------------------------------------

void UnifiedExtractor::extract_ruby_module(TSNode node) {
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

}  // namespace lci::parser
