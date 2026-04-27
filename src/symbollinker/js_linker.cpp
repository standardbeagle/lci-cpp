#include <lci/symbollinker/js_linker.h>

#include <functional>
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

bool ends_with(std::string_view str, std::string_view suffix) {
    if (suffix.size() > str.size()) return false;
    return str.substr(str.size() - suffix.size()) == suffix;
}

bool starts_with(std::string_view str, std::string_view prefix) {
    if (prefix.size() > str.size()) return false;
    return str.substr(0, prefix.size()) == prefix;
}

// Strips leading/trailing quote characters from a string literal.
std::string unquote(std::string_view text) {
    if (text.size() >= 2 &&
        ((text.front() == '"' && text.back() == '"') ||
         (text.front() == '\'' && text.back() == '\''))) {
        return std::string(text.substr(1, text.size() - 2));
    }
    return std::string(text);
}

// Checks if a child with specific text exists in a node.
bool has_child_with_text(TSNode node, std::string_view content,
                         std::string_view text) {
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (node_text(child, content) == text) {
            return true;
        }
    }
    return false;
}

// Traverses children recursively, calling visitor on each node.
// Visitor returns false to stop traversal of subtree.
void traverse(TSNode node,
              const std::function<bool(TSNode)>& visitor) {
    if (is_null(node)) return;
    if (!visitor(node)) return;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        traverse(ts_node_child(node, i), visitor);
    }
}

// Simple path joining for directory + filename.
std::string path_join(std::string_view dir, std::string_view file) {
    if (dir.empty()) return std::string(file);
    if (dir.back() == '/') {
        return std::string(dir) + std::string(file);
    }
    return std::string(dir) + "/" + std::string(file);
}

// Returns the directory part of a path.
std::string_view dir_of(std::string_view path) {
    auto pos = path.rfind('/');
    if (pos == std::string_view::npos) return ".";
    return path.substr(0, pos);
}

// Normalizes a path by resolving .. and . components.
std::string normalize_path(std::string_view path) {
    std::vector<std::string_view> parts;
    std::string_view remaining = path;

    bool absolute = !remaining.empty() && remaining[0] == '/';

    while (!remaining.empty()) {
        auto slash = remaining.find('/');
        std::string_view component;
        if (slash == std::string_view::npos) {
            component = remaining;
            remaining = {};
        } else {
            component = remaining.substr(0, slash);
            remaining = remaining.substr(slash + 1);
        }

        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(component);
    }

    std::string result;
    if (absolute) result = "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += '/';
        result += parts[i];
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// JSExtractor
// ---------------------------------------------------------------------------

JSExtractor::JSExtractor(bool typescript) : is_typescript_(typescript) {}

bool JSExtractor::can_handle(std::string_view path) const {
    if (is_typescript_) {
        return ends_with(path, ".ts") || ends_with(path, ".tsx") ||
               ends_with(path, ".mts") || ends_with(path, ".cts");
    }
    return ends_with(path, ".js") || ends_with(path, ".jsx") ||
           ends_with(path, ".mjs") || ends_with(path, ".cjs");
}

SymbolTable JSExtractor::extract_symbols(FileID file_id,
                                          std::string_view content,
                                          TSTree* tree) {
    SymbolTable table;
    table.file_id = file_id;
    table.language = is_typescript_ ? parser::Language::TypeScript
                                    : parser::Language::JavaScript;

    if (tree == nullptr) return table;

    TSNode root = ts_tree_root_node(tree);
    if (is_null(root)) return table;

    extract_imports(root, content, table, is_typescript_);
    extract_exports(root, content, table, is_typescript_);
    extract_from_node(root, content, table, is_typescript_);

    return table;
}

void JSExtractor::extract_imports(TSNode root, std::string_view content,
                                   SymbolTable& table, bool is_ts) {
    traverse(root, [&](TSNode node) -> bool {
        std::string_view kind = ts_node_type(node);

        if (kind == "import_statement") {
            extract_import_statement(node, content, table, is_ts);
        }
        return true;
    });
}

void JSExtractor::extract_import_statement(TSNode node,
                                            std::string_view content,
                                            SymbolTable& table,
                                            bool is_ts) {
    ImportInfo imp;
    imp.line = static_cast<int>(ts_node_start_point(node).row);
    imp.column = static_cast<int>(ts_node_start_point(node).column);

    // Extract source path from string literal.
    TSNode source = find_child_by_type(node, "string");
    if (!is_null(source)) {
        imp.import_path = unquote(node_text(source, content));
    }

    // Check for type-only import (TypeScript).
    if (is_ts) {
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(node, i);
            if (node_text(child, content) == "type") {
                imp.is_type_only = true;
                break;
            }
        }
    }

    TSNode import_clause = find_child_by_type(node, "import_clause");
    if (is_null(import_clause)) {
        // Side-effect import: import "module"
        if (!imp.import_path.empty()) {
            table.imports.push_back(std::move(imp));
        }
        return;
    }

    // Default import.
    TSNode default_ident = find_child_by_type(import_clause, "identifier");
    if (!is_null(default_ident)) {
        ImportInfo default_imp = imp;
        default_imp.alias = node_text(default_ident, content);
        default_imp.is_default = true;
        table.imports.push_back(std::move(default_imp));
    }

    // Named imports.
    TSNode named = find_child_by_type(import_clause, "named_imports");
    if (!is_null(named)) {
        std::vector<std::string> names;
        uint32_t count = ts_node_child_count(named);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(named, i);
            if (std::string_view(ts_node_type(child)) == "import_specifier") {
                TSNode name_node = find_child_by_type(child, "identifier");
                if (!is_null(name_node)) {
                    std::string n = node_text(name_node, content);
                    if (n != "as") {
                        names.push_back(std::move(n));
                    }
                }
            }
        }
        if (!names.empty()) {
            ImportInfo named_imp = imp;
            named_imp.imported_names = std::move(names);
            table.imports.push_back(std::move(named_imp));
        }
    }

    // Namespace import (import * as name).
    TSNode ns_import =
        find_child_by_type(import_clause, "namespace_import");
    if (!is_null(ns_import)) {
        TSNode ident = find_child_by_type(ns_import, "identifier");
        if (!is_null(ident)) {
            ImportInfo ns_imp = imp;
            ns_imp.alias = node_text(ident, content);
            ns_imp.is_namespace = true;
            table.imports.push_back(std::move(ns_imp));
        }
    }
}

void JSExtractor::extract_exports(TSNode root, std::string_view content,
                                   SymbolTable& table, bool is_ts) {
    traverse(root, [&](TSNode node) -> bool {
        std::string_view kind = ts_node_type(node);

        if (kind == "export_statement") {
            extract_export_statement(node, content, table, is_ts);
        }
        return true;
    });
}

void JSExtractor::extract_export_statement(TSNode node,
                                            std::string_view content,
                                            SymbolTable& table,
                                            bool is_ts) {
    ExportInfo exp;
    exp.line = static_cast<int>(ts_node_start_point(node).row);
    exp.column = static_cast<int>(ts_node_start_point(node).column);

    // Check for type-only export (TypeScript).
    if (is_ts) {
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            if (node_text(ts_node_child(node, i), content) == "type") {
                exp.is_type_only = true;
                break;
            }
        }
    }

    // Check for default export.
    if (has_child_with_text(node, content, "default")) {
        exp.is_default = true;
        exp.exported_name = "default";

        // Try to find what is being exported.
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(node, i);
            std::string_view ckind = ts_node_type(child);
            if (ckind == "function_declaration" ||
                ckind == "class_declaration") {
                TSNode name = find_child_by_type(child, "identifier");
                if (!is_null(name)) {
                    exp.local_name = node_text(name, content);
                }
            } else if (ckind == "identifier") {
                std::string text = node_text(child, content);
                if (text != "export" && text != "default") {
                    exp.local_name = text;
                }
            }
        }

        table.exports.push_back(std::move(exp));
        return;
    }

    // Export clause: export { a, b } or export { a } from "..."
    TSNode export_clause = find_child_by_type(node, "export_clause");
    if (!is_null(export_clause)) {
        // Check for re-export source.
        TSNode source = find_child_by_type(node, "string");
        std::string source_path;
        if (!is_null(source)) {
            source_path = unquote(node_text(source, content));
        }

        uint32_t count = ts_node_child_count(export_clause);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(export_clause, i);
            if (std::string_view(ts_node_type(child)) != "export_specifier") {
                continue;
            }

            std::string local_name;
            std::string exported_name;
            int ident_count = 0;

            uint32_t spec_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < spec_count; ++j) {
                TSNode gc = ts_node_child(child, j);
                if (std::string_view(ts_node_type(gc)) == "identifier") {
                    std::string name = node_text(gc, content);
                    if (name == "as") continue;
                    if (ident_count == 0) {
                        local_name = name;
                        exported_name = name;
                    } else {
                        exported_name = name;
                    }
                    ++ident_count;
                }
            }

            if (!local_name.empty()) {
                ExportInfo ei = exp;
                ei.local_name = local_name;
                ei.exported_name = exported_name;
                if (!source_path.empty()) {
                    ei.is_re_export = true;
                    ei.source_path = source_path;
                }
                table.exports.push_back(std::move(ei));
            }
        }
        return;
    }

    // Declaration exports: export const/function/class ...
    for (const char* decl_type :
         {"lexical_declaration", "function_declaration",
          "class_declaration"}) {
        TSNode decl = find_child_by_type(node, decl_type);
        if (!is_null(decl)) {
            TSNode name = find_child_by_type(decl, "identifier");
            if (is_null(name)) {
                name = find_child_by_type(decl, "type_identifier");
            }
            if (!is_null(name)) {
                exp.local_name = node_text(name, content);
                exp.exported_name = exp.local_name;
                table.exports.push_back(std::move(exp));
            }
            return;
        }
    }
}

void JSExtractor::extract_from_node(TSNode node, std::string_view content,
                                     SymbolTable& table, bool is_ts) {
    if (is_null(node)) return;

    std::string_view kind = ts_node_type(node);

    if (kind == "function_declaration") {
        extract_function(node, content, table);
    } else if (kind == "class_declaration") {
        extract_class(node, content, table);
    } else if (kind == "variable_declaration" ||
               kind == "lexical_declaration") {
        extract_variable_declaration(node, content, table);
    } else if (is_ts && kind == "interface_declaration") {
        extract_interface(node, content, table);
    } else if (is_ts && kind == "type_alias_declaration") {
        extract_type_alias(node, content, table);
    } else if (is_ts && kind == "enum_declaration") {
        extract_enum(node, content, table);
    }

    // Recurse into children.
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(node, i), content, table, is_ts);
    }
}

void JSExtractor::extract_function(TSNode node, std::string_view content,
                                    SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "identifier");
    if (is_null(name_node)) return;

    std::string name = node_text(name_node, content);
    bool exported = is_node_exported(node, content);
    add_symbol(table, std::move(name), exported);
}

void JSExtractor::extract_class(TSNode node, std::string_view content,
                                 SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "type_identifier");
    if (is_null(name_node)) {
        name_node = find_child_by_type(node, "identifier");
    }
    if (is_null(name_node)) return;

    std::string class_name = node_text(name_node, content);
    bool exported = is_node_exported(node, content);
    add_symbol(table, class_name, exported);

    // Extract class methods.
    TSNode body = find_child_by_type(node, "class_body");
    if (is_null(body)) return;

    uint32_t count = ts_node_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(body, i);
        if (std::string_view(ts_node_type(child)) != "method_definition") {
            continue;
        }

        TSNode mname = find_child_by_type(child, "property_identifier");
        if (is_null(mname)) {
            mname = find_child_by_type(child, "identifier");
        }
        if (is_null(mname)) continue;

        std::string method_name = node_text(mname, content);
        std::string full_name = class_name + "." + method_name;
        bool is_private = !method_name.empty() &&
                          (method_name[0] == '#' || method_name[0] == '_');
        add_symbol(table, std::move(full_name), !is_private);
    }
}

void JSExtractor::extract_variable_declaration(TSNode node,
                                                std::string_view content,
                                                SymbolTable& table) {
    bool is_const = has_child_with_text(node, content, "const");
    bool exported = is_node_exported(node, content);

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (std::string_view(ts_node_type(child)) != "variable_declarator") {
            continue;
        }

        TSNode name_node = find_child_by_type(child, "identifier");
        if (is_null(name_node)) continue;

        std::string name = node_text(name_node, content);
        add_symbol(table, std::move(name), exported);

        // Suppress the is_const warning by using it.
        (void)is_const;
    }
}

void JSExtractor::extract_interface(TSNode node, std::string_view content,
                                     SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "type_identifier");
    if (is_null(name_node)) return;

    std::string name = node_text(name_node, content);
    bool exported = is_node_exported(node, content);
    add_symbol(table, std::move(name), exported);
}

void JSExtractor::extract_type_alias(TSNode node, std::string_view content,
                                      SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "type_identifier");
    if (is_null(name_node)) return;

    std::string name = node_text(name_node, content);
    bool exported = is_node_exported(node, content);
    add_symbol(table, std::move(name), exported);
}

void JSExtractor::extract_enum(TSNode node, std::string_view content,
                                SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "identifier");
    if (is_null(name_node)) return;

    std::string enum_name = node_text(name_node, content);
    bool exported = is_node_exported(node, content);
    add_symbol(table, enum_name, exported);

    // Extract enum members.
    TSNode body = find_child_by_type(node, "enum_body");
    if (is_null(body)) return;

    uint32_t count = ts_node_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(body, i);
        std::string_view ckind = ts_node_type(child);
        if (ckind != "enum_assignment" && ckind != "property_identifier") {
            continue;
        }

        TSNode member = find_child_by_type(child, "property_identifier");
        if (is_null(member)) {
            member = find_child_by_type(child, "identifier");
        }
        if (is_null(member)) continue;

        std::string member_name = node_text(member, content);
        std::string full_name = enum_name + "." + member_name;
        add_symbol(table, std::move(full_name), false);
    }
}

bool JSExtractor::is_node_exported(TSNode node, std::string_view content) {
    TSNode parent = ts_node_parent(node);
    if (is_null(parent)) return false;

    std::string_view pkind = ts_node_type(parent);
    if (pkind == "export_statement" ||
        pkind == "export_default_declaration") {
        return true;
    }

    // Check if parent has "export" keyword before this node.
    uint32_t count = ts_node_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(parent, i);
        if (ts_node_start_byte(child) == ts_node_start_byte(node) &&
            ts_node_end_byte(child) == ts_node_end_byte(node)) {
            break;
        }
        if (node_text(child, content) == "export") {
            return true;
        }
    }

    return false;
}

uint32_t JSExtractor::add_symbol(SymbolTable& table, std::string name,
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
// JSResolver
// ---------------------------------------------------------------------------

JSResolver::JSResolver(std::string root_path)
    : root_path_(std::move(root_path)) {}

void JSResolver::set_file_registry(
    const absl::flat_hash_map<std::string, FileID>& registry) {
    file_registry_ = registry;
    reverse_registry_.clear();
    for (const auto& [path, fid] : file_registry_) {
        reverse_registry_[fid] = path;
    }
}

ModuleResolution JSResolver::resolve_import(std::string_view import_path,
                                             FileID from_file) {
    ModuleResolution res;
    res.request_path = std::string(import_path);

    if (is_builtin_module(import_path)) {
        res.is_builtin = true;
        res.is_external = true;
        return res;
    }

    // Determine source directory.
    std::string_view from_dir;
    auto rev_it = reverse_registry_.find(from_file);
    if (rev_it != reverse_registry_.end()) {
        from_dir = dir_of(rev_it->second);
    }

    if (starts_with(import_path, "./") || starts_with(import_path, "../")) {
        if (from_dir.empty()) {
            return res;
        }
        return resolve_relative(import_path, from_dir);
    }

    // Bare specifier: try direct registry lookup first.
    auto it = file_registry_.find(std::string(import_path));
    if (it != file_registry_.end()) {
        res.resolved_path = it->first;
        res.file_id = it->second;
        return res;
    }

    // External package.
    res.is_external = true;
    return res;
}

ModuleResolution JSResolver::resolve_relative(
    std::string_view import_path, std::string_view from_dir) const {
    std::string target = normalize_path(
        path_join(from_dir, import_path));

    // Try exact match and with extensions.
    ModuleResolution res = try_resolve_file(target);
    if (res.file_id != 0) return res;

    // Try as directory with index files.
    static const std::string_view kIndexFiles[] = {
        "/index.js",  "/index.ts",  "/index.jsx",
        "/index.tsx",  "/index.mjs", "/index.cjs",
    };
    for (auto suffix : kIndexFiles) {
        std::string index_path = target + std::string(suffix);
        auto it = file_registry_.find(index_path);
        if (it != file_registry_.end()) {
            res.request_path = std::string(import_path);
            res.resolved_path = it->first;
            res.file_id = it->second;
            return res;
        }
    }

    res.request_path = std::string(import_path);
    return res;
}

ModuleResolution JSResolver::try_resolve_file(
    std::string_view base_path) const {
    ModuleResolution res;
    res.request_path = std::string(base_path);

    static const std::string_view kExtensions[] = {
        "", ".js", ".ts", ".jsx", ".tsx", ".mjs", ".cjs", ".d.ts",
    };

    for (auto ext : kExtensions) {
        std::string full = std::string(base_path) + std::string(ext);
        auto it = file_registry_.find(full);
        if (it != file_registry_.end()) {
            res.resolved_path = it->first;
            res.file_id = it->second;
            return res;
        }
    }

    return res;
}

bool JSResolver::is_builtin_module(std::string_view import_path) {
    if (starts_with(import_path, "node:")) return true;

    static const absl::flat_hash_set<std::string_view> kBuiltins = {
        "assert",         "buffer",     "child_process",
        "cluster",        "crypto",     "dgram",
        "dns",            "domain",     "events",
        "fs",             "http",       "https",
        "net",            "os",         "path",
        "punycode",       "querystring","readline",
        "repl",           "stream",     "string_decoder",
        "tls",            "tty",        "url",
        "util",           "vm",         "zlib",
        "constants",      "module",     "process",
        "timers",         "console",
    };

    return kBuiltins.contains(import_path);
}

// ---------------------------------------------------------------------------
// TSResolver
// ---------------------------------------------------------------------------

TSResolver::TSResolver(std::string root_path)
    : inner_(std::move(root_path)) {}

ModuleResolution TSResolver::resolve_import(std::string_view import_path,
                                             FileID from_file) {
    return inner_.resolve_import(import_path, from_file);
}

void TSResolver::set_file_registry(
    const absl::flat_hash_map<std::string, FileID>& registry) {
    inner_.set_file_registry(registry);
}

}  // namespace lci::symbollinker
