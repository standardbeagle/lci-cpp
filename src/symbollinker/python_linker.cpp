#include <lci/symbollinker/python_linker.h>

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

// Strips leading/trailing quote characters from a string literal.
std::string unquote(std::string_view text) {
    if (text.size() >= 2 &&
        ((text.front() == '"' && text.back() == '"') ||
         (text.front() == '\'' && text.back() == '\''))) {
        return std::string(text.substr(1, text.size() - 2));
    }
    return std::string(text);
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
        if (component.empty() || component == ".") continue;
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

// Simple path joining.
std::string path_join(std::string_view dir, std::string_view file) {
    if (dir.empty()) return std::string(file);
    if (dir.back() == '/') return std::string(dir) + std::string(file);
    return std::string(dir) + "/" + std::string(file);
}

// Traverses all children recursively, calling visitor on each.
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
// PythonExtractor
// ---------------------------------------------------------------------------

bool PythonExtractor::can_handle(std::string_view path) const {
    return ends_with(path, ".py") || ends_with(path, ".pyw") ||
           ends_with(path, ".pyi");
}

SymbolTable PythonExtractor::extract_symbols(FileID file_id,
                                              std::string_view content,
                                              TSTree* tree) {
    SymbolTable table;
    table.file_id = file_id;
    table.language = parser::Language::Python;

    if (tree == nullptr) return table;

    TSNode root = ts_tree_root_node(tree);
    if (is_null(root)) return table;

    extract_imports(root, content, table);
    extract_from_node(root, content, table, {});

    return table;
}

void PythonExtractor::extract_imports(TSNode root, std::string_view content,
                                       SymbolTable& table) {
    traverse(root, [&](TSNode node) -> bool {
        std::string_view kind = ts_node_type(node);
        if (kind == "import_statement") {
            extract_import_statement(node, content, table);
        } else if (kind == "import_from_statement") {
            extract_import_from_statement(node, content, table);
        }
        return true;
    });
}

void PythonExtractor::extract_import_statement(TSNode node,
                                                std::string_view content,
                                                SymbolTable& table) {
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view kind = ts_node_type(child);

        if (kind == "dotted_name" || kind == "identifier") {
            ImportInfo imp;
            imp.line = node_line(child);
            imp.column = node_column(child);
            imp.import_path = node_text(child, content);

            // Check for alias (import module as alias).
            if (i + 1 < count) {
                TSNode next = ts_node_child(node, i + 1);
                if (!is_null(next) &&
                    node_text(next, content) == "as" &&
                    i + 2 < count) {
                    TSNode alias_node = ts_node_child(node, i + 2);
                    if (!is_null(alias_node)) {
                        imp.alias = node_text(alias_node, content);
                    }
                }
            }

            // Default alias is the last dotted component.
            if (imp.alias.empty() && !imp.import_path.empty()) {
                auto dot = imp.import_path.rfind('.');
                if (dot != std::string::npos) {
                    imp.alias = imp.import_path.substr(dot + 1);
                } else {
                    imp.alias = imp.import_path;
                }
            }

            if (!imp.import_path.empty()) {
                table.imports.push_back(std::move(imp));
            }
        } else if (kind == "aliased_import") {
            // Handle aliased imports within the statement.
            std::string name;
            std::string alias;
            uint32_t ac = ts_node_child_count(child);
            for (uint32_t j = 0; j < ac; ++j) {
                TSNode gc = ts_node_child(child, j);
                std::string_view gk = ts_node_type(gc);
                if (gk == "identifier" || gk == "dotted_name") {
                    std::string text = node_text(gc, content);
                    if (text != "as") {
                        if (name.empty()) {
                            name = text;
                        } else {
                            alias = text;
                        }
                    }
                }
            }
            if (!name.empty()) {
                ImportInfo imp;
                imp.line = node_line(child);
                imp.column = node_column(child);
                imp.import_path = name;
                imp.alias = alias;
                imp.imported_names.push_back(name);
                table.imports.push_back(std::move(imp));
            }
        }
    }
}

void PythonExtractor::extract_import_from_statement(
    TSNode node, std::string_view content, SymbolTable& table) {
    std::string module_path;
    std::vector<std::string> imported_names;
    bool is_wildcard = false;
    bool past_import_keyword = false;

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view kind = ts_node_type(child);

        if (kind == "import") {
            past_import_keyword = true;
        } else if (kind == "from") {
            // Skip the "from" keyword.
        } else if (kind == "dotted_name" || kind == "identifier") {
            std::string text = node_text(child, content);
            if (!past_import_keyword) {
                if (module_path.empty()) {
                    module_path = text;
                }
            } else {
                imported_names.push_back(std::move(text));
            }
        } else if (kind == "relative_import") {
            module_path = node_text(child, content);
        } else if (kind == "wildcard_import") {
            is_wildcard = true;
        } else if (kind == "aliased_import") {
            TSNode name_node = find_child_by_type(child, "dotted_name");
            if (is_null(name_node)) {
                name_node = find_child_by_type(child, "identifier");
            }
            if (!is_null(name_node)) {
                imported_names.push_back(node_text(name_node, content));
            }
        } else if (kind == "import_list") {
            uint32_t lc = ts_node_child_count(child);
            for (uint32_t j = 0; j < lc; ++j) {
                TSNode lchild = ts_node_child(child, j);
                std::string_view lk = ts_node_type(lchild);
                if (lk == "dotted_name" || lk == "identifier") {
                    imported_names.push_back(
                        node_text(lchild, content));
                } else if (lk == "aliased_import") {
                    TSNode ln =
                        find_child_by_type(lchild, "dotted_name");
                    if (is_null(ln)) {
                        ln = find_child_by_type(lchild, "identifier");
                    }
                    if (!is_null(ln)) {
                        imported_names.push_back(
                            node_text(ln, content));
                    }
                }
            }
        }
    }

    if (!module_path.empty()) {
        ImportInfo imp;
        imp.line = node_line(node);
        imp.column = node_column(node);
        imp.import_path = module_path;
        imp.imported_names = std::move(imported_names);
        imp.is_namespace = is_wildcard;
        table.imports.push_back(std::move(imp));
    }
}

void PythonExtractor::extract_from_node(TSNode node,
                                          std::string_view content,
                                          SymbolTable& table,
                                          std::string_view class_name) {
    if (is_null(node)) return;

    std::string_view kind = ts_node_type(node);

    if (kind == "function_definition" ||
        kind == "async_function_definition") {
        extract_function(node, content, table, class_name);
        return;
    }
    if (kind == "class_definition") {
        extract_class(node, content, table);
        return;
    }
    if (kind == "assignment") {
        extract_assignment(node, content, table, class_name);
    }

    // Recurse into children (but not into function/class bodies twice).
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(node, i), content, table,
                          class_name);
    }
}

void PythonExtractor::extract_function(TSNode node,
                                        std::string_view content,
                                        SymbolTable& table,
                                        std::string_view class_name) {
    TSNode name_node = find_child_by_type(node, "identifier");
    if (is_null(name_node)) return;

    std::string func_name = node_text(name_node, content);
    std::string full_name = func_name;
    if (!class_name.empty()) {
        full_name = std::string(class_name) + "." + func_name;
    }

    add_symbol(table, std::move(full_name), is_exported(func_name));
}

void PythonExtractor::extract_class(TSNode node, std::string_view content,
                                     SymbolTable& table) {
    TSNode name_node = find_child_by_type(node, "identifier");
    if (is_null(name_node)) return;

    std::string class_name = node_text(name_node, content);
    add_symbol(table, class_name, is_exported(class_name));

    // Extract class body members.
    TSNode body = find_child_by_type(node, "block");
    if (is_null(body)) return;

    uint32_t count = ts_node_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        extract_from_node(ts_node_child(body, i), content, table,
                          class_name);
    }
}

void PythonExtractor::extract_assignment(TSNode node,
                                          std::string_view content,
                                          SymbolTable& table,
                                          std::string_view class_name) {
    // Only extract simple identifier assignments at module/class level.
    TSNode left = ts_node_child(node, 0);
    if (is_null(left)) return;

    if (std::string_view(ts_node_type(left)) != "identifier") return;

    std::string var_name = node_text(left, content);
    std::string full_name = var_name;
    if (!class_name.empty()) {
        full_name = std::string(class_name) + "." + var_name;
    }

    add_symbol(table, std::move(full_name), is_exported(var_name));
}

bool PythonExtractor::is_exported(std::string_view name) {
    if (name.empty()) return false;
    return name[0] != '_';
}

uint32_t PythonExtractor::add_symbol(SymbolTable& table, std::string name,
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
// PythonResolver
// ---------------------------------------------------------------------------

PythonResolver::PythonResolver(std::string root_path)
    : root_path_(std::move(root_path)) {}

void PythonResolver::set_file_registry(
    const absl::flat_hash_map<std::string, FileID>& registry) {
    file_registry_ = registry;
    reverse_registry_.clear();
    for (const auto& [path, fid] : file_registry_) {
        reverse_registry_[fid] = path;
    }
}

ModuleResolution PythonResolver::resolve_import(
    std::string_view import_path, FileID from_file) {
    ModuleResolution res;
    res.request_path = std::string(import_path);

    if (is_builtin_module(import_path) || is_stdlib_module(import_path)) {
        res.resolved_path = std::string(import_path);
        res.is_builtin = true;
        return res;
    }

    // Relative imports start with a dot.
    if (starts_with(import_path, ".")) {
        std::string_view from_dir;
        auto rev_it = reverse_registry_.find(from_file);
        if (rev_it != reverse_registry_.end()) {
            from_dir = dir_of(rev_it->second);
        }
        if (from_dir.empty()) return res;
        return resolve_relative(import_path, from_dir);
    }

    return resolve_absolute(import_path);
}

ModuleResolution PythonResolver::resolve_relative(
    std::string_view import_path, std::string_view from_dir) const {
    ModuleResolution res;
    res.request_path = std::string(import_path);

    // Count leading dots.
    size_t dots = 0;
    while (dots < import_path.size() && import_path[dots] == '.') {
        ++dots;
    }
    std::string_view module_part = import_path.substr(dots);

    // Navigate up (dots - 1) levels.
    std::string current_dir(from_dir);
    for (size_t i = 1; i < dots; ++i) {
        auto pos = current_dir.rfind('/');
        if (pos == std::string::npos) break;
        current_dir = current_dir.substr(0, pos);
    }

    // Convert dots to slashes in module part.
    std::string relative_path;
    for (char c : module_part) {
        relative_path += (c == '.') ? '/' : c;
    }

    if (!relative_path.empty()) {
        // Try module_name.py
        std::string candidate = current_dir + "/" + relative_path + ".py";
        std::string norm = normalize_path(candidate);
        auto it = file_registry_.find(norm);
        if (it != file_registry_.end()) {
            res.resolved_path = it->first;
            res.file_id = it->second;
            return res;
        }

        // Try module_name/__init__.py
        candidate = current_dir + "/" + relative_path + "/__init__.py";
        norm = normalize_path(candidate);
        it = file_registry_.find(norm);
        if (it != file_registry_.end()) {
            res.resolved_path = it->first;
            res.file_id = it->second;
            return res;
        }
    } else {
        // Just dots: resolve to __init__.py in current_dir.
        std::string candidate = current_dir + "/__init__.py";
        std::string norm = normalize_path(candidate);
        auto it = file_registry_.find(norm);
        if (it != file_registry_.end()) {
            res.resolved_path = it->first;
            res.file_id = it->second;
            return res;
        }
    }

    return res;
}

ModuleResolution PythonResolver::resolve_absolute(
    std::string_view import_path) const {
    ModuleResolution res;
    res.request_path = std::string(import_path);

    // Convert dots to slashes.
    std::string relative_path;
    for (char c : import_path) {
        relative_path += (c == '.') ? '/' : c;
    }

    std::string prefix = root_path_;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';

    // Try as module.py
    {
        std::string candidate = prefix + relative_path + ".py";
        auto it = file_registry_.find(candidate);
        if (it != file_registry_.end()) {
            res.resolved_path = it->first;
            res.file_id = it->second;
            return res;
        }
    }

    // Try as package/__init__.py
    {
        std::string candidate = prefix + relative_path + "/__init__.py";
        auto it = file_registry_.find(candidate);
        if (it != file_registry_.end()) {
            res.resolved_path = it->first;
            res.file_id = it->second;
            return res;
        }
    }

    // External package.
    res.is_external = true;
    return res;
}

bool PythonResolver::is_builtin_module(std::string_view import_path) {
    static const absl::flat_hash_set<std::string_view> kBuiltins = {
        "sys",       "builtins",   "gc",        "marshal",
        "imp",       "abc",        "io",        "errno",
        "signal",    "winreg",     "_thread",   "posix",
        "nt",        "pwd",        "grp",
    };
    return kBuiltins.contains(import_path);
}

bool PythonResolver::is_stdlib_module(std::string_view import_path) {
    // Extract the top-level module name.
    std::string_view base = import_path;
    auto dot = import_path.find('.');
    if (dot != std::string_view::npos) {
        base = import_path.substr(0, dot);
    }

    static const absl::flat_hash_set<std::string_view> kStdlib = {
        "os",          "sys",        "json",        "re",
        "math",        "collections","functools",   "itertools",
        "pathlib",     "subprocess", "shutil",      "tempfile",
        "logging",     "unittest",   "typing",      "dataclasses",
        "datetime",    "time",       "argparse",    "copy",
        "hashlib",     "hmac",       "secrets",     "random",
        "string",      "textwrap",   "difflib",     "struct",
        "codecs",      "unicodedata","csv",         "configparser",
        "pprint",      "enum",       "numbers",     "decimal",
        "fractions",   "statistics", "array",       "heapq",
        "bisect",      "queue",      "types",       "weakref",
        "contextlib",  "threading",  "multiprocessing",
        "concurrent",  "asyncio",    "socket",      "ssl",
        "http",        "urllib",     "email",       "html",
        "xml",         "sqlite3",    "dbm",         "gzip",
        "bz2",         "lzma",      "zipfile",     "tarfile",
        "io",          "pickle",    "shelve",      "glob",
        "fnmatch",     "stat",      "fileinput",   "traceback",
        "warnings",    "inspect",   "dis",         "ast",
        "token",       "tokenize",  "platform",    "ctypes",
        "importlib",   "pkgutil",   "pdb",         "cProfile",
        "profile",     "timeit",    "trace",       "distutils",
        "venv",        "site",      "abc",         "atexit",
        "signal",
    };

    return kStdlib.contains(base);
}

}  // namespace lci::symbollinker
