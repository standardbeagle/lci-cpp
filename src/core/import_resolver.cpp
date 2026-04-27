#include <lci/core/reference_tracker.h>

#include <cstddef>
#include <string>

namespace lci {

// ---------------------------------------------------------------------------
// ImportResolver
// ---------------------------------------------------------------------------

FileImportData ImportResolver::extract_file_imports(
    FileID file_id, std::string_view file_path, std::string_view content) {

    FileImportData data;
    data.file_id = file_id;

    // Determine language from extension.
    auto dot_pos = file_path.rfind('.');
    if (dot_pos == std::string_view::npos) return data;

    auto ext = file_path.substr(dot_pos);
    std::string ext_lower;
    ext_lower.reserve(ext.size());
    for (char c : ext) {
        ext_lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    // Simple line-based import extraction.
    // Splits content into lines and applies language-specific parsing.
    size_t line_start = 0;
    int line_num = 1;
    while (line_start < content.size()) {
        size_t line_end = content.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = content.size();

        auto line = content.substr(line_start,
                                    line_end - line_start);

        std::vector<ImportBinding> bindings;
        if (ext_lower == ".go") {
            bindings = extract_go_imports(line);
        } else if (ext_lower == ".js" || ext_lower == ".ts" ||
                   ext_lower == ".tsx") {
            bindings = extract_js_imports(line);
        } else if (ext_lower == ".py") {
            bindings = extract_python_imports(line);
        } else if (ext_lower == ".rs") {
            bindings = extract_rust_imports(line);
        }

        for (auto& b : bindings) {
            b.line_number = line_num;
            data.bindings.push_back(std::move(b));
        }

        line_start = line_end + 1;
        line_num++;
    }

    return data;
}

void ImportResolver::build_import_graph(
    std::span<const FileImportData> import_data) {
    import_graph_.clear();
    for (const auto& d : import_data) {
        if (!d.bindings.empty()) {
            import_graph_[d.file_id] = d.bindings;
        }
    }
}

SymbolID ImportResolver::resolve_symbol_reference(
    FileID ref_file_id,
    std::string_view referenced_name,
    std::span<const SymbolID> candidates,
    std::function<const EnhancedSymbol*(SymbolID)> symbol_lookup) const {

    // Strategy 1: Check if symbol is imported in this file.
    if (auto it = import_graph_.find(ref_file_id);
        it != import_graph_.end()) {
        for (const auto& binding : it->second) {
            if (binding.imported_name == referenced_name ||
                binding.original_name == referenced_name) {
                for (SymbolID cid : candidates) {
                    if (symbol_lookup(cid) != nullptr) return cid;
                }
            }
        }
    }

    // Strategy 2: Prefer symbols from same file.
    for (SymbolID cid : candidates) {
        if (const auto* sym = symbol_lookup(cid)) {
            if (sym->symbol.file_id == ref_file_id) return cid;
        }
    }

    // Strategy 3: Prefer exported symbols.
    for (SymbolID cid : candidates) {
        if (const auto* sym = symbol_lookup(cid)) {
            if (sym->is_exported) return cid;
        }
    }

    // Strategy 4: First candidate.
    if (!candidates.empty()) return candidates[0];

    return 0;
}

void ImportResolver::remove_file(FileID file_id) {
    import_graph_.erase(file_id);
}

void ImportResolver::clear() {
    import_graph_.clear();
}

// -- Language-specific extractors --------------------------------------------

std::vector<ImportBinding> ImportResolver::extract_go_imports(
    std::string_view line) const {
    std::vector<ImportBinding> bindings;

    // Simple heuristic: look for `import "pkg"` or `"pkg"` inside import block.
    auto trimmed = line;
    while (!trimmed.empty() && (trimmed.front() == ' ' ||
                                 trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
    }

    // Skip comments.
    if (trimmed.starts_with("//")) return bindings;

    // Find quoted string.
    auto q1 = trimmed.find('"');
    if (q1 == std::string_view::npos) return bindings;
    auto q2 = trimmed.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return bindings;

    auto pkg_path = trimmed.substr(q1 + 1, q2 - q1 - 1);
    if (pkg_path.empty()) return bindings;

    // Extract package name (last path component).
    auto slash = pkg_path.rfind('/');
    std::string_view pkg_name = (slash != std::string_view::npos)
                                     ? pkg_path.substr(slash + 1)
                                     : pkg_path;

    // Check for alias before the quote.
    auto before_quote = trimmed.substr(0, q1);
    while (!before_quote.empty() &&
           (before_quote.back() == ' ' || before_quote.back() == '\t')) {
        before_quote.remove_suffix(1);
    }

    // Strip leading "import " if present.
    if (before_quote.starts_with("import")) {
        before_quote.remove_prefix(6);
        while (!before_quote.empty() &&
               (before_quote.front() == ' ' || before_quote.front() == '\t')) {
            before_quote.remove_prefix(1);
        }
    }

    std::string alias;
    if (!before_quote.empty() && before_quote != "(" && before_quote != ")") {
        alias = std::string(before_quote);
    }

    ImportBinding b;
    b.imported_name = alias.empty() ? std::string(pkg_name) : alias;
    b.original_name = b.imported_name;
    b.source_file = std::string(pkg_path);
    bindings.push_back(std::move(b));

    return bindings;
}

std::vector<ImportBinding> ImportResolver::extract_js_imports(
    std::string_view line) const {
    std::vector<ImportBinding> bindings;

    auto trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
    }
    if (!trimmed.starts_with("import")) return bindings;

    // Find "from" keyword.
    auto from_pos = trimmed.find("from");
    if (from_pos == std::string_view::npos) return bindings;

    auto source_part = trimmed.substr(from_pos + 4);
    while (!source_part.empty() &&
           (source_part.front() == ' ' || source_part.front() == '\'' ||
            source_part.front() == '"')) {
        source_part.remove_prefix(1);
    }
    // Find end of source string.
    auto end_pos = source_part.find_first_of("'\";\n");
    std::string source_file(source_part.substr(
        0, end_pos != std::string_view::npos ? end_pos : source_part.size()));

    auto import_part = trimmed.substr(6, from_pos - 6);
    while (!import_part.empty() &&
           (import_part.front() == ' ' || import_part.front() == '\t')) {
        import_part.remove_prefix(1);
    }
    while (!import_part.empty() &&
           (import_part.back() == ' ' || import_part.back() == '\t')) {
        import_part.remove_suffix(1);
    }

    // Check for `{ ... }` named imports.
    auto brace_start = import_part.find('{');
    auto brace_end = import_part.find('}');
    if (brace_start != std::string_view::npos &&
        brace_end != std::string_view::npos && brace_end > brace_start) {
        auto names = import_part.substr(brace_start + 1,
                                         brace_end - brace_start - 1);
        // Split by comma.
        size_t pos = 0;
        while (pos < names.size()) {
            auto comma = names.find(',', pos);
            if (comma == std::string_view::npos) comma = names.size();
            auto name = names.substr(pos, comma - pos);
            while (!name.empty() &&
                   (name.front() == ' ' || name.front() == '\t')) {
                name.remove_prefix(1);
            }
            while (!name.empty() &&
                   (name.back() == ' ' || name.back() == '\t')) {
                name.remove_suffix(1);
            }
            if (!name.empty()) {
                ImportBinding b;
                b.imported_name = std::string(name);
                b.original_name = std::string(name);
                b.source_file = source_file;
                bindings.push_back(std::move(b));
            }
            pos = comma + 1;
        }
    } else if (!import_part.empty()) {
        // Default import.
        ImportBinding b;
        b.imported_name = std::string(import_part);
        b.original_name = std::string(import_part);
        b.source_file = source_file;
        bindings.push_back(std::move(b));
    }

    return bindings;
}

std::vector<ImportBinding> ImportResolver::extract_python_imports(
    std::string_view line) const {
    std::vector<ImportBinding> bindings;

    auto trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
    }

    if (trimmed.starts_with("from ")) {
        auto import_pos = trimmed.find(" import ");
        if (import_pos == std::string_view::npos) return bindings;

        auto module = trimmed.substr(5, import_pos - 5);
        auto names = trimmed.substr(import_pos + 8);

        // Split by comma.
        size_t pos = 0;
        while (pos < names.size()) {
            auto comma = names.find(',', pos);
            if (comma == std::string_view::npos) comma = names.size();
            auto name = names.substr(pos, comma - pos);
            while (!name.empty() &&
                   (name.front() == ' ' || name.front() == '\t')) {
                name.remove_prefix(1);
            }
            while (!name.empty() &&
                   (name.back() == ' ' || name.back() == '\t' ||
                    name.back() == '\r')) {
                name.remove_suffix(1);
            }
            if (!name.empty()) {
                ImportBinding b;
                b.imported_name = std::string(name);
                b.original_name = std::string(name);
                b.source_file = std::string(module);
                bindings.push_back(std::move(b));
            }
            pos = comma + 1;
        }
    } else if (trimmed.starts_with("import ")) {
        auto module = trimmed.substr(7);
        while (!module.empty() &&
               (module.back() == ' ' || module.back() == '\t' ||
                module.back() == '\r')) {
            module.remove_suffix(1);
        }
        if (!module.empty()) {
            ImportBinding b;
            b.imported_name = std::string(module);
            b.original_name = std::string(module);
            b.source_file = std::string(module);
            bindings.push_back(std::move(b));
        }
    }

    return bindings;
}

std::vector<ImportBinding> ImportResolver::extract_rust_imports(
    std::string_view line) const {
    std::vector<ImportBinding> bindings;

    auto trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
    }
    if (!trimmed.starts_with("use ")) return bindings;

    auto use_stmt = trimmed.substr(4);
    // Remove trailing semicolon.
    while (!use_stmt.empty() &&
           (use_stmt.back() == ';' || use_stmt.back() == ' ' ||
            use_stmt.back() == '\r')) {
        use_stmt.remove_suffix(1);
    }
    if (use_stmt.empty()) return bindings;

    // Check for braced imports: `use std::{A, B};`
    auto brace_start = use_stmt.find('{');
    auto brace_end = use_stmt.find('}');
    if (brace_start != std::string_view::npos &&
        brace_end != std::string_view::npos && brace_end > brace_start) {
        auto base = use_stmt.substr(0, brace_start);
        while (!base.empty() && (base.back() == ':' || base.back() == ' ')) {
            base.remove_suffix(1);
        }
        auto symbols = use_stmt.substr(brace_start + 1,
                                        brace_end - brace_start - 1);
        size_t pos = 0;
        while (pos < symbols.size()) {
            auto comma = symbols.find(',', pos);
            if (comma == std::string_view::npos) comma = symbols.size();
            auto name = symbols.substr(pos, comma - pos);
            while (!name.empty() &&
                   (name.front() == ' ' || name.front() == '\t')) {
                name.remove_prefix(1);
            }
            while (!name.empty() &&
                   (name.back() == ' ' || name.back() == '\t')) {
                name.remove_suffix(1);
            }
            if (!name.empty()) {
                ImportBinding b;
                b.imported_name = std::string(name);
                b.original_name = std::string(name);
                b.source_file = std::string(base);
                bindings.push_back(std::move(b));
            }
            pos = comma + 1;
        }
    } else {
        // Single import: `use std::collections::HashMap;`
        auto last_sep = use_stmt.rfind("::");
        if (last_sep != std::string_view::npos) {
            auto symbol = use_stmt.substr(last_sep + 2);
            auto source = use_stmt.substr(0, last_sep);
            ImportBinding b;
            b.imported_name = std::string(symbol);
            b.original_name = std::string(symbol);
            b.source_file = std::string(source);
            bindings.push_back(std::move(b));
        } else if (!use_stmt.empty()) {
            ImportBinding b;
            b.imported_name = std::string(use_stmt);
            b.original_name = std::string(use_stmt);
            bindings.push_back(std::move(b));
        }
    }

    return bindings;
}

}  // namespace lci
