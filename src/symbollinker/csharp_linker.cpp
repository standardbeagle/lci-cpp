#include <lci/symbollinker/csharp_linker.h>

#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

namespace lci::symbollinker {

namespace {

std::string node_text(TSNode node, std::string_view content) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= content.size() || end > content.size() || end <= start) {
        return {};
    }
    return std::string(content.substr(start, end - start));
}

TSNode find_child_by_type(TSNode node, const char* type) {
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (std::string_view(ts_node_type(child)) == type) {
            return child;
        }
    }
    return ts_node_child(node, count);  // null node
}

bool is_null(TSNode node) { return ts_node_is_null(node); }

int node_line(TSNode node) {
    return static_cast<int>(ts_node_start_point(node).row);
}

int node_column(TSNode node) {
    return static_cast<int>(ts_node_start_point(node).column);
}

bool ends_with(std::string_view str, std::string_view suffix) {
    if (suffix.size() > str.size()) return false;
    return str.substr(str.size() - suffix.size()) == suffix;
}

bool starts_with(std::string_view str, std::string_view prefix) {
    if (prefix.size() > str.size()) return false;
    return str.substr(0, prefix.size()) == prefix;
}

void traverse(TSNode node,
              const std::function<bool(TSNode)>& visitor) {
    if (is_null(node)) return;
    if (!visitor(node)) return;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        traverse(ts_node_child(node, i), visitor);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// CSharpExtractor
// ---------------------------------------------------------------------------

bool CSharpExtractor::can_handle(std::string_view path) const {
    return ends_with(path, ".cs") || ends_with(path, ".csx");
}

SymbolTable CSharpExtractor::extract_symbols(FileID file_id,
                                              std::string_view content,
                                              TSTree* tree) {
    SymbolTable table;
    table.file_id = file_id;
    table.language = parser::Language::CSharp;

    if (tree == nullptr) return table;

    TSNode root = ts_tree_root_node(tree);
    if (is_null(root)) return table;

    extract_usings(root, content, table);
    extract_from_node(root, content, table, {});

    return table;
}

void CSharpExtractor::extract_usings(TSNode root, std::string_view content,
                                      SymbolTable& table) {
    traverse(root, [&](TSNode node) -> bool {
        if (std::string_view(ts_node_type(node)) == "using_directive") {
            ImportInfo imp = extract_using_directive(node, content);
            if (!imp.import_path.empty()) {
                table.imports.push_back(std::move(imp));
            }
        }
        return true;
    });
}

ImportInfo CSharpExtractor::extract_using_directive(
    TSNode node, std::string_view content) {
    ImportInfo imp;
    imp.line = node_line(node);
    imp.column = node_column(node);

    bool is_static = false;
    bool has_alias = false;

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view kind = ts_node_type(child);

        if (kind == "static") {
            is_static = true;
        } else if (kind == "qualified_name" || kind == "identifier") {
            if (!has_alias) {
                imp.import_path = node_text(child, content);
            }
        } else if (kind == "name_equals") {
            has_alias = true;
            uint32_t nc = ts_node_child_count(child);
            for (uint32_t j = 0; j < nc; ++j) {
                TSNode ac = ts_node_child(child, j);
                if (std::string_view(ts_node_type(ac)) == "identifier") {
                    imp.alias = node_text(ac, content);
                    break;
                }
            }
        }
    }

    if (is_static) {
        imp.is_type_only = true;
    }

    return imp;
}

std::string CSharpExtractor::extract_namespace(TSNode root,
                                                std::string_view content) {
    TSNode ns_node = find_child_by_type(root, "namespace_declaration");
    if (is_null(ns_node)) {
        ns_node = find_child_by_type(
            root, "file_scoped_namespace_declaration");
    }
    if (is_null(ns_node)) return {};

    TSNode name_node = find_child_by_type(ns_node, "qualified_name");
    if (is_null(name_node)) {
        name_node = find_child_by_type(ns_node, "identifier");
    }
    if (is_null(name_node)) return {};

    return node_text(name_node, content);
}

void CSharpExtractor::extract_from_node(TSNode node,
                                          std::string_view content,
                                          SymbolTable& table,
                                          std::string_view class_name) {
    if (is_null(node)) return;

    std::string_view kind = ts_node_type(node);

    if (kind == "class_declaration" || kind == "struct_declaration" ||
        kind == "interface_declaration" || kind == "enum_declaration" ||
        kind == "record_declaration") {
        extract_type_declaration(node, content, table);
        return;
    }
    if (kind == "method_declaration") {
        extract_method(node, content, table, class_name);
        return;
    }
    if (kind == "constructor_declaration") {
        extract_constructor(node, content, table, class_name);
        return;
    }
    if (kind == "property_declaration") {
        extract_property(node, content, table, class_name);
        return;
    }
    if (kind == "field_declaration") {
        extract_field(node, content, table, class_name);
        return;
    }
    if (kind == "delegate_declaration") {
        TSNode name_node = find_child_by_type(node, "identifier");
        if (!is_null(name_node)) {
            std::string name = node_text(name_node, content);
            add_symbol(table, std::move(name),
                       has_public_modifier(node, content));
        }
        return;
    }
    if (kind == "namespace_declaration" ||
        kind == "file_scoped_namespace_declaration") {
        // Extract namespace name as a symbol.
        TSNode name_node = find_child_by_type(node, "qualified_name");
        if (is_null(name_node)) {
            name_node = find_child_by_type(node, "identifier");
        }
        if (!is_null(name_node)) {
            std::string ns_name = node_text(name_node, content);
            add_symbol(table, std::move(ns_name), true);
        }
    }

    // Recurse into children.
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(node, i), content, table,
                          class_name);
    }
}

void CSharpExtractor::extract_type_declaration(TSNode node,
                                                std::string_view content,
                                                SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "identifier");
    if (is_null(name_node)) return;

    std::string type_name = node_text(name_node, content);
    bool exported = has_public_modifier(node, content);
    add_symbol(table, type_name, exported);

    std::string_view kind = ts_node_type(node);

    if (kind == "enum_declaration") {
        // Extract enum members from enum_member_declaration_list.
        TSNode body = find_child_by_type(
            node, "enum_member_declaration_list");
        if (!is_null(body)) {
            extract_enum_members(body, content, table, type_name);
        }
        return;
    }

    // For class/struct/interface/record, extract body members.
    TSNode body = find_child_by_type(node, "declaration_list");
    if (is_null(body)) return;

    uint32_t count = ts_node_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(body, i), content, table,
                          type_name);
    }
}

void CSharpExtractor::extract_method(TSNode node, std::string_view content,
                                      SymbolTable& table,
                                      std::string_view class_name) {
    TSNode name_node = find_child_by_type(node, "identifier");
    if (is_null(name_node)) return;

    std::string method_name = node_text(name_node, content);
    std::string full_name = method_name;
    if (!class_name.empty()) {
        full_name = std::string(class_name) + "." + method_name;
    }

    add_symbol(table, std::move(full_name),
               has_public_modifier(node, content));
}

void CSharpExtractor::extract_constructor(TSNode node,
                                           std::string_view content,
                                           SymbolTable& table,
                                           std::string_view class_name) {
    TSNode name_node = find_child_by_type(node, "identifier");
    if (is_null(name_node)) return;

    std::string ctor_name = node_text(name_node, content);
    std::string full_name = ctor_name;
    if (!class_name.empty()) {
        full_name = std::string(class_name) + "." + ctor_name;
    }

    add_symbol(table, std::move(full_name),
               has_public_modifier(node, content));
}

void CSharpExtractor::extract_property(TSNode node,
                                        std::string_view content,
                                        SymbolTable& table,
                                        std::string_view class_name) {
    TSNode name_node = find_child_by_type(node, "identifier");
    if (is_null(name_node)) return;

    std::string prop_name = node_text(name_node, content);
    std::string full_name = prop_name;
    if (!class_name.empty()) {
        full_name = std::string(class_name) + "." + prop_name;
    }

    add_symbol(table, std::move(full_name),
               has_public_modifier(node, content));
}

void CSharpExtractor::extract_field(TSNode node, std::string_view content,
                                     SymbolTable& table,
                                     std::string_view class_name) {
    // Field declarations contain a variable_declaration with declarators.
    TSNode var_decl = find_child_by_type(node, "variable_declaration");
    if (is_null(var_decl)) return;

    bool exported = has_public_modifier(node, content);
    uint32_t count = ts_node_child_count(var_decl);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(var_decl, i);
        if (std::string_view(ts_node_type(child)) != "variable_declarator") {
            continue;
        }
        TSNode name_node = find_child_by_type(child, "identifier");
        if (is_null(name_node)) continue;

        std::string field_name = node_text(name_node, content);
        std::string full_name = field_name;
        if (!class_name.empty()) {
            full_name = std::string(class_name) + "." + field_name;
        }
        add_symbol(table, std::move(full_name), exported);
    }
}

void CSharpExtractor::extract_enum_members(TSNode body,
                                            std::string_view content,
                                            SymbolTable& table,
                                            std::string_view enum_name) {
    uint32_t count = ts_node_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(body, i);
        if (std::string_view(ts_node_type(child)) !=
            "enum_member_declaration") {
            continue;
        }

        TSNode name_node = find_child_by_type(child, "identifier");
        if (is_null(name_node)) continue;

        std::string member_name = node_text(name_node, content);
        std::string full_name =
            std::string(enum_name) + "." + member_name;
        add_symbol(table, std::move(full_name), true);
    }
}

bool CSharpExtractor::has_public_modifier(TSNode node,
                                           std::string_view content) {
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view kind = ts_node_type(child);
        if (kind == "modifier") {
            if (node_text(child, content) == "public") return true;
        }
        // Some tree-sitter grammars expose modifiers directly.
        if (node_text(child, content) == "public") return true;
    }
    return false;
}

uint32_t CSharpExtractor::add_symbol(SymbolTable& table, std::string name,
                                      bool exported) {
    uint32_t local_id = table.next_local_id++;
    SymbolID sym_id = static_cast<SymbolID>(table.file_id) * 10000 +
                      static_cast<SymbolID>(local_id);

    table.symbol_ids.push_back(sym_id);
    table.symbol_names.push_back(std::move(name));

    if (exported) {
        ExportInfo exp;
        exp.local_id = local_id;
        exp.exported_name = table.symbol_names.back();
        exp.local_name = table.symbol_names.back();
        table.exports.push_back(std::move(exp));
    }

    return local_id;
}

// ---------------------------------------------------------------------------
// CSharpResolver
// ---------------------------------------------------------------------------

CSharpResolver::CSharpResolver(std::string root_path)
    : root_path_(std::move(root_path)) {}

void CSharpResolver::set_file_registry(
    const absl::flat_hash_map<std::string, FileID>& registry) {
    file_registry_ = registry;
    reverse_registry_.clear();
    for (const auto& [path, fid] : file_registry_) {
        reverse_registry_[fid] = path;
    }
}

ModuleResolution CSharpResolver::resolve_import(
    std::string_view import_path, FileID /*from_file*/) {
    ModuleResolution res;
    res.request_path = std::string(import_path);

    if (is_builtin_namespace(import_path)) {
        res.resolved_path = std::string(import_path);
        res.is_builtin = true;
        return res;
    }

    // Try project namespace resolution.
    ModuleResolution project_res = resolve_project_namespace(import_path);
    if (project_res.file_id != 0) return project_res;

    if (is_known_external(import_path)) {
        res.is_external = true;
        res.resolved_path = std::string(import_path);
        return res;
    }

    // Unknown namespace -- mark as external.
    res.is_external = true;
    return res;
}

ModuleResolution CSharpResolver::resolve_project_namespace(
    std::string_view ns) const {
    ModuleResolution res;
    res.request_path = std::string(ns);

    // Convert namespace dots to path separators and look for .cs files.
    std::string relative_path;
    for (char c : ns) {
        relative_path += (c == '.') ? '/' : c;
    }

    std::string prefix = root_path_;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';

    // Search the registry for files under the namespace directory.
    std::string dir_prefix = prefix + relative_path + "/";
    for (const auto& [path, fid] : file_registry_) {
        if (path.size() > dir_prefix.size() &&
            path.substr(0, dir_prefix.size()) == dir_prefix &&
            ends_with(path, ".cs")) {
            res.resolved_path = path;
            res.file_id = fid;
            return res;
        }
    }

    // Try common patterns: src/Namespace/...
    static const std::string_view kCommonDirs[] = {"src/", "lib/"};
    for (auto cd : kCommonDirs) {
        std::string alt_prefix =
            prefix + std::string(cd) + relative_path + "/";
        for (const auto& [path, fid] : file_registry_) {
            if (path.size() > alt_prefix.size() &&
                path.substr(0, alt_prefix.size()) == alt_prefix &&
                ends_with(path, ".cs")) {
                res.resolved_path = path;
                res.file_id = fid;
                return res;
            }
        }
    }

    return res;
}

bool CSharpResolver::is_builtin_namespace(std::string_view ns) {
    static const absl::flat_hash_set<std::string_view> kPrefixes = {
        "System", "Microsoft", "Windows",
    };

    // Extract the first component.
    std::string_view first = ns;
    auto dot = ns.find('.');
    if (dot != std::string_view::npos) {
        first = ns.substr(0, dot);
    }

    return kPrefixes.contains(first);
}

bool CSharpResolver::is_known_external(std::string_view ns) {
    static const absl::flat_hash_set<std::string_view> kPatterns = {
        "Newtonsoft",       "NUnit",           "Moq",
        "AutoMapper",       "FluentValidation","Serilog",
        "NLog",             "EntityFramework", "Dapper",
        "MediatR",          "Swashbuckle",     "StackExchange",
        "Polly",            "Hangfire",        "Quartz",
    };

    std::string_view first = ns;
    auto dot = ns.find('.');
    if (dot != std::string_view::npos) {
        first = ns.substr(0, dot);
    }

    return kPatterns.contains(first);
}

}  // namespace lci::symbollinker
