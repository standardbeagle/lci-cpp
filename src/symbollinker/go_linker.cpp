#include <lci/symbollinker/go_linker.h>

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

namespace lci::symbollinker {

namespace {

// Returns the text content of a TSNode.
std::string node_text(TSNode node, std::string_view content) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= content.size() || end > content.size() || end <= start) {
        return {};
    }
    return std::string(content.substr(start, end - start));
}

// Finds the first child of a specific type. Returns a null node if not found.
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

// Returns true if a node is null (invalid).
bool is_null(TSNode node) { return ts_node_is_null(node); }

// Returns the start line (0-based) of a node.
int node_line(TSNode node) {
    return static_cast<int>(ts_node_start_point(node).row);
}

// Returns the start column (0-based) of a node.
int node_column(TSNode node) {
    return static_cast<int>(ts_node_start_point(node).column);
}

// Checks if a path ends with a given suffix.
bool ends_with(std::string_view str, std::string_view suffix) {
    if (suffix.size() > str.size()) return false;
    return str.substr(str.size() - suffix.size()) == suffix;
}

// Returns the last path component after the final '/'.
std::string_view last_path_component(std::string_view path) {
    auto pos = path.rfind('/');
    if (pos == std::string_view::npos) return path;
    return path.substr(pos + 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// GoExtractor
// ---------------------------------------------------------------------------

bool GoExtractor::can_handle(std::string_view path) const {
    return ends_with(path, ".go");
}

SymbolTable GoExtractor::extract_symbols(FileID file_id,
                                          std::string_view content,
                                          TSTree* tree) {
    SymbolTable table;
    table.file_id = file_id;
    table.language = parser::Language::Go;

    if (tree == nullptr) return table;

    TSNode root = ts_tree_root_node(tree);
    if (is_null(root)) return table;

    extract_imports(root, content, table);
    extract_from_node(root, content, table);

    return table;
}

std::string GoExtractor::extract_package_name(TSNode root,
                                               std::string_view content) {
    TSNode pkg_clause = find_child_by_type(root, "package_clause");
    if (is_null(pkg_clause)) return {};

    TSNode pkg_ident = find_child_by_type(pkg_clause, "package_identifier");
    if (is_null(pkg_ident)) return {};

    return node_text(pkg_ident, content);
}

void GoExtractor::extract_imports(TSNode root, std::string_view content,
                                   SymbolTable& table) {
    uint32_t count = ts_node_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(root, i);
        if (std::string_view(ts_node_type(child)) != "import_declaration") {
            continue;
        }

        TSNode spec_list = find_child_by_type(child, "import_spec_list");
        if (!is_null(spec_list)) {
            uint32_t spec_count = ts_node_child_count(spec_list);
            for (uint32_t j = 0; j < spec_count; ++j) {
                TSNode spec = ts_node_child(spec_list, j);
                if (std::string_view(ts_node_type(spec)) == "import_spec") {
                    ImportInfo imp = extract_import_spec(spec, content);
                    if (!imp.import_path.empty()) {
                        table.imports.push_back(std::move(imp));
                    }
                }
            }
        } else {
            TSNode spec = find_child_by_type(child, "import_spec");
            if (!is_null(spec)) {
                ImportInfo imp = extract_import_spec(spec, content);
                if (!imp.import_path.empty()) {
                    table.imports.push_back(std::move(imp));
                }
            }
        }
    }
}

ImportInfo GoExtractor::extract_import_spec(TSNode spec,
                                             std::string_view content) {
    ImportInfo imp;
    imp.line = node_line(spec);
    imp.column = node_column(spec);

    uint32_t count = ts_node_child_count(spec);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(spec, i);
        std::string_view kind = ts_node_type(child);

        if (kind == "package_identifier" || kind == "blank_identifier") {
            imp.alias = node_text(child, content);
        } else if (kind == "interpreted_string_literal") {
            std::string path = node_text(child, content);
            if (path.size() >= 2) {
                imp.import_path = path.substr(1, path.size() - 2);
            }
        } else if (kind == "dot") {
            imp.alias = ".";
        }
    }

    // Default alias is the last component of the import path.
    if (imp.alias.empty() && !imp.import_path.empty()) {
        imp.alias = std::string(last_path_component(imp.import_path));
    }

    return imp;
}

void GoExtractor::extract_from_node(TSNode node, std::string_view content,
                                     SymbolTable& table) {
    if (is_null(node)) return;

    std::string_view kind = ts_node_type(node);

    if (kind == "function_declaration") {
        extract_function(node, content, table);
        return;
    }
    if (kind == "method_declaration") {
        extract_method(node, content, table);
        return;
    }
    if (kind == "type_declaration") {
        extract_type_declaration(node, content, table);
        return;
    }
    if (kind == "var_declaration") {
        extract_var_declaration(node, content, table, false);
        return;
    }
    if (kind == "const_declaration") {
        extract_var_declaration(node, content, table, true);
        return;
    }

    // Recurse into children for other node types.
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(node, i), content, table);
    }
}

void GoExtractor::extract_function(TSNode node, std::string_view content,
                                    SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "identifier");
    if (is_null(name_node)) return;

    std::string name = node_text(name_node, content);
    add_symbol(table, std::move(name), is_exported(name));
}

void GoExtractor::extract_method(TSNode node, std::string_view content,
                                  SymbolTable& table) {
    // Extract receiver type from the first parameter_list.
    std::string receiver_type;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (std::string_view(ts_node_type(child)) == "parameter_list") {
            uint32_t pcount = ts_node_child_count(child);
            for (uint32_t j = 0; j < pcount; ++j) {
                TSNode param = ts_node_child(child, j);
                if (std::string_view(ts_node_type(param)) !=
                    "parameter_declaration") {
                    continue;
                }
                TSNode type_node =
                    find_child_by_type(param, "type_identifier");
                if (!is_null(type_node)) {
                    receiver_type = node_text(type_node, content);
                } else {
                    TSNode ptr = find_child_by_type(param, "pointer_type");
                    if (!is_null(ptr)) {
                        TSNode inner =
                            find_child_by_type(ptr, "type_identifier");
                        if (!is_null(inner)) {
                            receiver_type = node_text(inner, content);
                        }
                    }
                }
                break;
            }
            break;
        }
    }

    TSNode name_node = find_child_by_type(node, "field_identifier");
    if (is_null(name_node)) return;

    std::string method_name = node_text(name_node, content);
    std::string full_name = method_name;
    if (!receiver_type.empty()) {
        full_name = receiver_type + "." + method_name;
    }

    add_symbol(table, std::move(full_name), is_exported(method_name));
}

void GoExtractor::extract_type_declaration(TSNode node,
                                            std::string_view content,
                                            SymbolTable& table) {
    TSNode spec = find_child_by_type(node, "type_spec");
    if (is_null(spec)) {
        spec = find_child_by_type(node, "type_alias");
    }
    if (is_null(spec)) return;

    TSNode name_node = find_child_by_type(spec, "type_identifier");
    if (is_null(name_node)) return;

    std::string type_name = node_text(name_node, content);
    add_symbol(table, type_name, is_exported(type_name));

    // Extract struct fields or interface methods.
    uint32_t spec_count = ts_node_child_count(spec);
    for (uint32_t i = 0; i < spec_count; ++i) {
        TSNode child = ts_node_child(spec, i);
        std::string_view kind = ts_node_type(child);

        if (kind == "struct_type") {
            extract_struct_fields(child, content, table, type_name);
        } else if (kind == "interface_type") {
            extract_interface_methods(child, content, table, type_name);
        }
    }
}

void GoExtractor::extract_struct_fields(TSNode struct_node,
                                         std::string_view content,
                                         SymbolTable& table,
                                         std::string_view struct_name) {
    TSNode field_list =
        find_child_by_type(struct_node, "field_declaration_list");
    if (is_null(field_list)) return;

    uint32_t count = ts_node_child_count(field_list);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode field = ts_node_child(field_list, i);
        if (std::string_view(ts_node_type(field)) != "field_declaration") {
            continue;
        }

        TSNode name_node = find_child_by_type(field, "field_identifier");
        if (is_null(name_node)) continue;

        std::string field_name = node_text(name_node, content);
        std::string full_name =
            std::string(struct_name) + "." + field_name;
        add_symbol(table, std::move(full_name), is_exported(field_name));
    }
}

void GoExtractor::extract_interface_methods(TSNode iface_node,
                                             std::string_view content,
                                             SymbolTable& table,
                                             std::string_view iface_name) {
    uint32_t count = ts_node_child_count(iface_node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode method = ts_node_child(iface_node, i);
        if (std::string_view(ts_node_type(method)) != "method_elem") {
            continue;
        }

        TSNode name_node = find_child_by_type(method, "field_identifier");
        if (is_null(name_node)) continue;

        std::string method_name = node_text(name_node, content);
        std::string full_name =
            std::string(iface_name) + "." + method_name;
        add_symbol(table, std::move(full_name), is_exported(method_name));
    }
}

void GoExtractor::extract_var_declaration(TSNode node,
                                           std::string_view content,
                                           SymbolTable& table,
                                           bool is_const) {
    // Collect spec nodes from grouped or single declarations.
    std::vector<TSNode> specs;

    const char* list_type = is_const ? "const_spec_list" : "var_spec_list";
    const char* spec_type = is_const ? "const_spec" : "var_spec";

    TSNode spec_list = find_child_by_type(node, list_type);
    if (!is_null(spec_list)) {
        uint32_t count = ts_node_child_count(spec_list);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(spec_list, i);
            if (std::string_view(ts_node_type(child)) == spec_type) {
                specs.push_back(child);
            }
        }
    } else {
        // Direct children.
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(node, i);
            if (std::string_view(ts_node_type(child)) == spec_type) {
                specs.push_back(child);
            }
        }
    }

    for (TSNode spec : specs) {
        uint32_t count = ts_node_child_count(spec);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(spec, i);
            if (std::string_view(ts_node_type(child)) == "identifier") {
                std::string var_name = node_text(child, content);
                add_symbol(table, std::move(var_name),
                           is_exported(var_name));
            }
        }
    }
}

bool GoExtractor::is_exported(std::string_view name) {
    if (name.empty()) return false;
    auto ch = static_cast<unsigned char>(name[0]);
    return std::isupper(ch) != 0;
}

uint32_t GoExtractor::add_symbol(SymbolTable& table, std::string name,
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
// GoResolver
// ---------------------------------------------------------------------------

GoResolver::GoResolver(std::string root_path)
    : root_path_(std::move(root_path)) {}

void GoResolver::set_file_registry(
    const absl::flat_hash_map<std::string, FileID>& registry) {
    file_registry_ = registry;
}

void GoResolver::set_module_name(std::string_view name) {
    module_name_ = std::string(name);
}

ModuleResolution GoResolver::resolve_import(std::string_view import_path,
                                             FileID /*from_file*/) {
    ModuleResolution res;
    res.request_path = std::string(import_path);

    if (is_standard_package(import_path)) {
        res.resolved_path = std::string(import_path);
        res.is_builtin = true;
        return res;
    }

    if (!module_name_.empty() &&
        import_path.substr(0, module_name_.size()) == module_name_) {
        return resolve_module_import(import_path);
    }

    // External package.
    res.is_external = true;
    res.resolved_path = std::string(import_path);
    return res;
}

ModuleResolution GoResolver::resolve_module_import(
    std::string_view import_path) const {
    ModuleResolution res;
    res.request_path = std::string(import_path);

    // Strip module prefix to get relative path.
    std::string_view relative = import_path.substr(module_name_.size());
    if (!relative.empty() && relative[0] == '/') {
        relative.remove_prefix(1);
    }

    // Look for any registered file that starts with root + relative.
    std::string prefix = root_path_;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    prefix += std::string(relative);

    for (const auto& [path, fid] : file_registry_) {
        if (path.size() >= prefix.size() &&
            path.substr(0, prefix.size()) == prefix) {
            res.resolved_path = path;
            res.file_id = fid;
            return res;
        }
    }

    // Directory match: check if prefix matches a directory.
    std::string dir_prefix = prefix + "/";
    for (const auto& [path, fid] : file_registry_) {
        if (path.size() > dir_prefix.size() &&
            path.substr(0, dir_prefix.size()) == dir_prefix &&
            ends_with(path, ".go")) {
            res.resolved_path = path;
            res.file_id = fid;
            return res;
        }
    }

    res.is_external = true;
    return res;
}

bool GoResolver::is_standard_package(std::string_view import_path) {
    // Extract base package name (before first /).
    std::string_view base = import_path;
    auto slash = import_path.find('/');
    if (slash != std::string_view::npos) {
        base = import_path.substr(0, slash);
    }

    static const absl::flat_hash_set<std::string_view> kStdPkgs = {
        "archive",   "bufio",    "builtin",   "bytes",     "compress",
        "container", "context",  "crypto",    "database",  "debug",
        "embed",     "encoding", "errors",    "expvar",    "flag",
        "fmt",       "go",       "hash",      "html",      "image",
        "index",     "io",       "log",       "maps",      "math",
        "mime",      "net",      "os",        "path",      "plugin",
        "reflect",   "regexp",   "runtime",   "slices",    "sort",
        "strconv",   "strings",  "sync",      "syscall",   "testing",
        "text",      "time",     "unicode",   "unsafe",    "C",
        "internal",
    };

    return kStdPkgs.contains(base);
}

}  // namespace lci::symbollinker
