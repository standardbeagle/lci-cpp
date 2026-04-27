#include <lci/symbollinker/php_linker.h>

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
// PhpExtractor
// ---------------------------------------------------------------------------

bool PhpExtractor::can_handle(std::string_view path) const {
    return ends_with(path, ".php") || ends_with(path, ".phtml") ||
           ends_with(path, ".php3") || ends_with(path, ".php4") ||
           ends_with(path, ".php5") || ends_with(path, ".phar");
}

SymbolTable PhpExtractor::extract_symbols(FileID file_id,
                                           std::string_view content,
                                           TSTree* tree) {
    SymbolTable table;
    table.file_id = file_id;
    table.language = parser::Language::PHP;

    if (tree == nullptr) return table;

    TSNode root = ts_tree_root_node(tree);
    if (is_null(root)) return table;

    extract_use_statements(root, content, table);
    extract_includes(root, content, table);
    extract_from_node(root, content, table, {});

    return table;
}

std::string PhpExtractor::extract_namespace(TSNode root,
                                             std::string_view content) {
    TSNode ns_node = find_child_by_type(root, "namespace_definition");
    if (is_null(ns_node)) return {};

    TSNode name_node = find_child_by_type(ns_node, "namespace_name");
    if (is_null(name_node)) return {};

    return node_text(name_node, content);
}

void PhpExtractor::extract_use_statements(TSNode root,
                                           std::string_view content,
                                           SymbolTable& table) {
    traverse(root, [&](TSNode node) -> bool {
        if (std::string_view(ts_node_type(node)) ==
            "namespace_use_declaration") {
            extract_use_declaration(node, content, table);
        }
        return true;
    });
}

void PhpExtractor::extract_use_declaration(TSNode node,
                                            std::string_view content,
                                            SymbolTable& table) {
    std::string base_ns;

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view kind = ts_node_type(child);

        if (kind == "namespace_use_clause") {
            extract_use_clause(child, content, table, base_ns);
        } else if (kind == "namespace_name") {
            base_ns = node_text(child, content);
        } else if (kind == "namespace_use_group") {
            // Grouped use: use Ns\{A, B};
            uint32_t gc = ts_node_child_count(child);
            for (uint32_t j = 0; j < gc; ++j) {
                TSNode group_child = ts_node_child(child, j);
                if (std::string_view(ts_node_type(group_child)) ==
                    "namespace_use_clause") {
                    extract_use_clause(group_child, content, table, base_ns);
                }
            }
        }
    }
}

void PhpExtractor::extract_use_clause(TSNode node, std::string_view content,
                                       SymbolTable& table,
                                       std::string_view base_ns) {
    ImportInfo imp;
    imp.line = node_line(node);
    imp.column = node_column(node);

    std::string import_path;

    // Look for qualified_name or name child.
    TSNode qname = find_child_by_type(node, "qualified_name");
    if (!is_null(qname)) {
        import_path = extract_qualified_name(qname, content);
    } else {
        TSNode name_node = find_child_by_type(node, "name");
        if (!is_null(name_node)) {
            import_path = node_text(name_node, content);
        }
    }

    if (import_path.empty()) return;

    // Prepend base namespace for grouped imports.
    if (!base_ns.empty()) {
        imp.import_path = std::string(base_ns) + "\\" + import_path;
    } else {
        imp.import_path = import_path;
    }

    // Check for alias (as Alias).
    uint32_t count = ts_node_child_count(node);
    bool alias_found = false;
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (node_text(child, content) == "as" && i + 1 < count) {
            TSNode alias_node = ts_node_child(node, i + 1);
            imp.alias = node_text(alias_node, content);
            alias_found = true;
            break;
        }
    }

    // Default alias is the last segment of the import path.
    if (!alias_found) {
        auto pos = imp.import_path.rfind('\\');
        if (pos != std::string::npos) {
            imp.alias = imp.import_path.substr(pos + 1);
        } else {
            imp.alias = imp.import_path;
        }
    }

    table.imports.push_back(std::move(imp));
}

void PhpExtractor::extract_includes(TSNode root, std::string_view content,
                                     SymbolTable& table) {
    traverse(root, [&](TSNode node) -> bool {
        std::string_view kind = ts_node_type(node);
        if (kind != "include_expression" &&
            kind != "require_expression" &&
            kind != "include_once_expression" &&
            kind != "require_once_expression") {
            return true;
        }

        ImportInfo imp;
        imp.line = node_line(node);
        imp.column = node_column(node);

        // Find string literal argument.
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(node, i);
            if (std::string_view(ts_node_type(child)) == "string") {
                std::string path = node_text(child, content);
                if (path.size() >= 2) {
                    imp.import_path = path.substr(1, path.size() - 2);
                }
                break;
            }
        }

        if (kind == "include_once_expression" ||
            kind == "require_once_expression") {
            imp.is_type_only = true;
        }

        if (!imp.import_path.empty()) {
            table.imports.push_back(std::move(imp));
        }

        return true;
    });
}

void PhpExtractor::extract_from_node(TSNode node, std::string_view content,
                                      SymbolTable& table,
                                      std::string_view class_name) {
    if (is_null(node)) return;

    std::string_view kind = ts_node_type(node);

    if (kind == "namespace_definition") {
        TSNode name_node = find_child_by_type(node, "namespace_name");
        if (!is_null(name_node)) {
            std::string ns_name = node_text(name_node, content);
            add_symbol(table, std::move(ns_name), true);
        }
    } else if (kind == "function_definition") {
        extract_function(node, content, table);
        return;
    } else if (kind == "class_declaration") {
        extract_class(node, content, table);
        return;
    } else if (kind == "interface_declaration") {
        extract_interface(node, content, table);
        return;
    } else if (kind == "trait_declaration") {
        extract_trait(node, content, table);
        return;
    } else if (kind == "enum_declaration") {
        extract_enum(node, content, table);
        return;
    } else if (kind == "method_declaration") {
        extract_method(node, content, table, class_name);
        return;
    } else if (kind == "property_declaration") {
        extract_property(node, content, table, class_name);
        return;
    } else if (kind == "const_declaration") {
        extract_const(node, content, table, class_name);
        return;
    }

    // Recurse into children.
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(node, i), content, table, class_name);
    }
}

void PhpExtractor::extract_function(TSNode node, std::string_view content,
                                     SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "name");
    if (is_null(name_node)) return;

    std::string func_name = node_text(name_node, content);
    add_symbol(table, std::move(func_name), true);
}

void PhpExtractor::extract_class(TSNode node, std::string_view content,
                                  SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "name");
    if (is_null(name_node)) return;

    std::string class_name = node_text(name_node, content);
    add_symbol(table, class_name, true);

    TSNode body = find_child_by_type(node, "declaration_list");
    if (is_null(body)) return;

    uint32_t count = ts_node_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(body, i), content, table, class_name);
    }
}

void PhpExtractor::extract_interface(TSNode node, std::string_view content,
                                      SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "name");
    if (is_null(name_node)) return;

    std::string iface_name = node_text(name_node, content);
    add_symbol(table, iface_name, true);

    TSNode body = find_child_by_type(node, "declaration_list");
    if (is_null(body)) return;

    uint32_t count = ts_node_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(body, i), content, table, iface_name);
    }
}

void PhpExtractor::extract_trait(TSNode node, std::string_view content,
                                  SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "name");
    if (is_null(name_node)) return;

    std::string trait_name = node_text(name_node, content);
    add_symbol(table, trait_name, true);

    TSNode body = find_child_by_type(node, "declaration_list");
    if (is_null(body)) return;

    uint32_t count = ts_node_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(body, i), content, table, trait_name);
    }
}

void PhpExtractor::extract_enum(TSNode node, std::string_view content,
                                 SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "name");
    if (is_null(name_node)) return;

    std::string enum_name = node_text(name_node, content);
    add_symbol(table, enum_name, true);

    // Extract enum cases from the body.
    TSNode body = find_child_by_type(node, "enum_declaration_list");
    if (is_null(body)) return;

    uint32_t count = ts_node_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(body, i);
        if (std::string_view(ts_node_type(child)) == "enum_case") {
            TSNode case_name = find_child_by_type(child, "name");
            if (!is_null(case_name)) {
                std::string member = node_text(case_name, content);
                std::string full = enum_name + "." + member;
                add_symbol(table, std::move(full), true);
            }
        }
    }
}

void PhpExtractor::extract_method(TSNode node, std::string_view content,
                                   SymbolTable& table,
                                   std::string_view class_name) {
    TSNode name_node = find_child_by_type(node, "name");
    if (is_null(name_node)) return;

    std::string method_name = node_text(name_node, content);
    bool exported = has_public_modifier(node, content);

    std::string full_name = method_name;
    if (!class_name.empty()) {
        full_name = std::string(class_name) + "." + method_name;
    }

    add_symbol(table, std::move(full_name), exported);
}

void PhpExtractor::extract_property(TSNode node, std::string_view content,
                                     SymbolTable& table,
                                     std::string_view class_name) {
    bool exported = has_public_modifier(node, content);

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (std::string_view(ts_node_type(child)) != "property_element") {
            continue;
        }

        TSNode var_node = find_child_by_type(child, "variable_name");
        if (is_null(var_node)) continue;

        std::string prop_name = node_text(var_node, content);
        // Remove $ prefix from PHP variable names.
        if (!prop_name.empty() && prop_name.front() == '$') {
            prop_name.erase(0, 1);
        }

        std::string full_name = prop_name;
        if (!class_name.empty()) {
            full_name = std::string(class_name) + "." + prop_name;
        }

        add_symbol(table, std::move(full_name), exported);
    }
}

void PhpExtractor::extract_const(TSNode node, std::string_view content,
                                  SymbolTable& table,
                                  std::string_view class_name) {
    bool exported = has_public_modifier(node, content);

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (std::string_view(ts_node_type(child)) != "const_element") {
            continue;
        }

        TSNode name_node = find_child_by_type(child, "name");
        if (is_null(name_node)) continue;

        std::string const_name = node_text(name_node, content);

        std::string full_name = const_name;
        if (!class_name.empty()) {
            full_name = std::string(class_name) + "." + const_name;
        }

        // Constants without explicit visibility are public by default.
        add_symbol(table, std::move(full_name), exported || class_name.empty());
    }
}

std::string PhpExtractor::extract_qualified_name(TSNode node,
                                                  std::string_view content) {
    std::string result;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view kind = ts_node_type(child);
        if (kind == "name") {
            if (!result.empty()) result += '\\';
            result += node_text(child, content);
        } else if (kind == "namespace_name") {
            std::string nested = extract_qualified_name(child, content);
            if (!nested.empty()) {
                if (!result.empty()) result += '\\';
                result += nested;
            }
        }
    }
    return result;
}

bool PhpExtractor::has_public_modifier(TSNode node,
                                        std::string_view content) {
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view kind = ts_node_type(child);
        if (kind == "visibility_modifier" || kind == "modifier") {
            if (node_text(child, content) == "public") return true;
        }
        if (node_text(child, content) == "public") return true;
    }
    return false;
}

uint32_t PhpExtractor::add_symbol(SymbolTable& table, std::string name,
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
// PhpResolver
// ---------------------------------------------------------------------------

PhpResolver::PhpResolver(std::string root_path)
    : root_path_(std::move(root_path)) {}

void PhpResolver::set_file_registry(
    const absl::flat_hash_map<std::string, FileID>& registry) {
    file_registry_ = registry;
    reverse_registry_.clear();
    for (const auto& [path, fid] : file_registry_) {
        reverse_registry_[fid] = path;
    }
}

void PhpResolver::add_psr4_mapping(std::string_view ns_prefix,
                                    std::string_view directory) {
    psr4_mappings_[std::string(ns_prefix)] = std::string(directory);
}

ModuleResolution PhpResolver::resolve_import(std::string_view import_path,
                                              FileID from_file) {
    if (is_file_path(import_path)) {
        return resolve_file_include(import_path, from_file);
    }
    return resolve_namespace_import(import_path);
}

bool PhpResolver::is_file_path(std::string_view import_path) {
    return import_path.find('/') != std::string_view::npos ||
           starts_with(import_path, "./") ||
           starts_with(import_path, "../") ||
           ends_with(import_path, ".php") ||
           ends_with(import_path, ".phar") ||
           ends_with(import_path, ".inc");
}

ModuleResolution PhpResolver::resolve_namespace_import(
    std::string_view class_name) const {
    ModuleResolution res;
    res.request_path = std::string(class_name);

    // Strip leading backslash.
    std::string_view normalized = class_name;
    if (!normalized.empty() && normalized.front() == '\\') {
        normalized.remove_prefix(1);
    }

    if (is_builtin(normalized)) {
        res.resolved_path = std::string(normalized);
        res.is_builtin = true;
        return res;
    }

    // Try PSR-4 resolution.
    for (const auto& [prefix, dir] : psr4_mappings_) {
        if (!starts_with(normalized, prefix)) continue;

        std::string_view relative = normalized.substr(prefix.size());
        if (!relative.empty() && relative.front() == '\\') {
            relative.remove_prefix(1);
        }

        // Convert namespace separators to directory separators.
        std::string file_path;
        for (char c : relative) {
            file_path += (c == '\\') ? '/' : c;
        }
        file_path += ".php";

        std::string base = dir;
        if (!base.empty() && base.back() != '/') base += '/';
        if (base.front() != '/') {
            std::string abs = root_path_;
            if (!abs.empty() && abs.back() != '/') abs += '/';
            base = abs + base;
        }

        std::string full_path = base + file_path;
        auto it = file_registry_.find(full_path);
        if (it != file_registry_.end()) {
            res.resolved_path = full_path;
            res.file_id = it->second;
            return res;
        }
    }

    // Try directory-based namespace resolution.
    std::string relative_path;
    for (char c : normalized) {
        relative_path += (c == '\\') ? '/' : c;
    }

    std::string prefix = root_path_;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';

    // Search registry for matching files.
    std::string dir_prefix = prefix + relative_path + "/";
    for (const auto& [path, fid] : file_registry_) {
        if (path.size() > dir_prefix.size() &&
            path.substr(0, dir_prefix.size()) == dir_prefix &&
            ends_with(path, ".php")) {
            res.resolved_path = path;
            res.file_id = fid;
            return res;
        }
    }

    // Try common source directories.
    static const std::string_view kCommonDirs[] = {"src/", "lib/", "app/"};
    for (auto cd : kCommonDirs) {
        std::string alt_prefix =
            prefix + std::string(cd) + relative_path + "/";
        for (const auto& [path, fid] : file_registry_) {
            if (path.size() > alt_prefix.size() &&
                path.substr(0, alt_prefix.size()) == alt_prefix &&
                ends_with(path, ".php")) {
                res.resolved_path = path;
                res.file_id = fid;
                return res;
            }
        }
    }

    // Unknown namespace -- mark as external.
    res.is_external = true;
    return res;
}

ModuleResolution PhpResolver::resolve_file_include(
    std::string_view include_path, FileID from_file) const {
    ModuleResolution res;
    res.request_path = std::string(include_path);

    auto from_it = reverse_registry_.find(from_file);
    if (from_it == reverse_registry_.end()) {
        res.is_external = true;
        return res;
    }

    const std::string& from_path = from_it->second;
    auto slash_pos = from_path.rfind('/');
    std::string current_dir =
        (slash_pos != std::string::npos) ? from_path.substr(0, slash_pos) : ".";

    // Strip leading ./ from the include path.
    std::string_view rel_path = include_path;
    if (rel_path.size() >= 2 && rel_path[0] == '.' && rel_path[1] == '/') {
        rel_path.remove_prefix(2);
    }

    // Try relative to current file.
    std::string candidate = current_dir + "/" + std::string(rel_path);
    auto it = file_registry_.find(candidate);
    if (it != file_registry_.end()) {
        res.resolved_path = candidate;
        res.file_id = it->second;
        return res;
    }

    // Try relative to project root.
    std::string root_candidate = root_path_;
    if (!root_candidate.empty() && root_candidate.back() != '/') {
        root_candidate += '/';
    }
    root_candidate += std::string(include_path);
    it = file_registry_.find(root_candidate);
    if (it != file_registry_.end()) {
        res.resolved_path = root_candidate;
        res.file_id = it->second;
        return res;
    }

    res.is_external = true;
    return res;
}

bool PhpResolver::is_builtin(std::string_view ns) {
    static const absl::flat_hash_set<std::string_view> kBuiltins = {
        "stdClass",     "Exception",     "Error",
        "Throwable",    "Iterator",      "IteratorAggregate",
        "ArrayAccess",  "Serializable",  "Closure",
        "Generator",    "Countable",     "Traversable",
        "JsonSerializable", "Stringable",
    };

    // Check if top-level name is a builtin class/interface.
    std::string_view first = ns;
    auto sep = ns.find('\\');
    if (sep != std::string_view::npos) {
        first = ns.substr(0, sep);
    }

    return kBuiltins.contains(first);
}

}  // namespace lci::symbollinker
