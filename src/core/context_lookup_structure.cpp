// C++ port of the structure section of internal/core/context_lookup.go
// (context_lookup_structure.go: fillStructureContext + its live index-driven
// getters). Only the index-driven live path is ported; the Go file's
// tree-sitter AST helpers (getImportQuery/getExportQuery/findStructNode/
// hasComposedFields/hasEmbeddedFields/etc, and the *Generic fallbacks) are
// dead under the index-driven path and are intentionally NOT ported (see the
// CLX port map, comment 01KXCYNRCFJECKP5EVTZ9A75B7).
//
// Traps preserved bug-for-bug:
//   trap 1  — Go's getImports keys a map by module_path (unordered
//             iteration); the C++ port sorts the imports bucket
//             deterministically by module_path so goldens compare set-wise.
//   trap 5  — InterfaceInfo.methods stays [] and is_fully_implemented stays
//             true (Go hardcodes both — "would need deeper analysis").
//             ImportInfo.import_name is never set (Go's getImports never
//             populates ImportName either).
//   trap 6c — getInterfaceImplementations/getInheritanceChain gate on
//             SymbolType::Class ONLY (context_lookup_structure.go:151,195),
//             excluding Struct — same shape of divergence as the S4
//             class-variables dispatch gate. TODO below marks the bug.
//   trap 9  — imports (this section) hardcode import_style="default"
//             (context_lookup_structure.go:97); the relationships section's
//             imported_modules uses "direct" — an intentional Go
//             inconsistency, preserved here rather than unified.

#include <lci/core/context_lookup.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>

#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/reference.h>
#include <lci/search/search_engine.h>
#include <lci/symbol.h>

namespace lci {

namespace {

using Snapshot = ReferenceTracker::Snapshot;
using SymbolHandle = Snapshot::SymbolHandle;

// Resolves the target symbol by (name, file, type) — same lookup shape as
// context_lookup_variables.cpp's resolve_target(require_type=true).
SymbolHandle find_target(const Snapshot& snap, const CodeObjectID& oid) {
    for (const auto& sym : snap.find_symbols_by_name(oid.name)) {
        if (sym->symbol.file_id != oid.file_id) continue;
        if (sym->symbol.type != oid.type) continue;
        return sym;
    }
    return nullptr;
}

// -- getFileInfo (context_lookup_structure.go:62) ----------------------------
// extractModuleFromPath / extractPackageFromPath ported verbatim: a "src"
// substring in the containing directory switches module to the path
// remainder after "src"; otherwise module falls back to the same value as
// package (the containing directory's base name).

std::string extract_package_from_path(const std::string& file_path) {
    std::filesystem::path dir = std::filesystem::path(file_path).parent_path();
    if (dir.empty()) return ".";
    std::string base = dir.filename().string();
    return base.empty() ? "." : base;
}

std::string extract_module_from_path(const std::string& file_path) {
    std::filesystem::path dir_path =
        std::filesystem::path(file_path).parent_path();
    std::string dir = dir_path.string();
    auto pos = dir.find("src");
    if (pos != std::string::npos) {
        std::string after = dir.substr(pos + 3);
        if (!after.empty() && after.front() == '/') after.erase(0, 1);
        return after;
    }
    return extract_package_from_path(file_path);
}

// -- getImports (context_lookup_structure.go:75) -----------------------------
// Scans every enhanced symbol in the file for Import-type outgoing refs,
// dedup'd by module_path (Go's map key). is_used is always true (an Import
// ref existing IS the usage signal); import_style is hardcoded "default"
// (trap 9); import_name is never set (trap 5).
std::vector<ImportInfo> get_imports(const Snapshot& snap,
                                    const CodeObjectID& oid) {
    absl::flat_hash_map<std::string, ImportInfo> by_module;
    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        for (const auto& ref : sym->outgoing_refs) {
            if (ref.type != ReferenceType::Import) continue;
            if (ref.referenced_name.empty()) continue;
            if (by_module.contains(ref.referenced_name)) continue;
            ImportInfo info;
            info.module_path = ref.referenced_name;
            info.import_style = "default";
            info.is_used = true;
            by_module.emplace(ref.referenced_name, std::move(info));
        }
    }
    std::vector<ImportInfo> imports;
    imports.reserve(by_module.size());
    for (auto& [path, info] : by_module) imports.push_back(std::move(info));
    // trap 1: Go's map iteration is unordered; sort deterministically.
    std::stable_sort(imports.begin(), imports.end(),
                     [](const ImportInfo& a, const ImportInfo& b) {
                         return a.module_path < b.module_path;
                     });
    return imports;
}

// -- getExports (context_lookup_structure.go:114) ----------------------------
// Every exported symbol in the file, with used_by naming the other files that
// reference it ("file:N").
std::vector<ExportInfo> get_exports(const Snapshot& snap,
                                    const CodeObjectID& oid) {
    std::vector<ExportInfo> exports;
    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        if (!sym->is_exported) continue;
        ExportInfo info;
        info.name = std::string(sym->symbol.name);
        info.type = std::string(to_string(sym->symbol.type));
        info.export_style = "named";
        for (const auto& ref : sym->incoming_refs) {
            if (ref.file_id == oid.file_id) continue;
            info.used_by.push_back("file:" + std::to_string(ref.file_id));
        }
        exports.push_back(std::move(info));
    }
    return exports;
}

// -- getInterfaceImplementations (context_lookup_structure.go:147) -----------
// TODO(clx-port): bug-for-bug parity with Go — this gate excludes Struct,
// same divergence shape as the S4 class-variables dispatch gate
// (context_lookup_variables.cpp fill_variable_context). Go's own gate is
// `objectID.Type != types.SymbolTypeClass` (line 151); Struct objects never
// reach this getter even though structs can implement interfaces in several
// target languages.
std::vector<InterfaceInfo> get_interface_implementations(
    const Snapshot& snap, const CodeObjectID& oid) {
    std::vector<InterfaceInfo> interfaces;
    if (oid.type != SymbolType::Class) return interfaces;
    auto target = find_target(snap, oid);
    if (target == nullptr) return interfaces;

    for (const auto& ref : target->outgoing_refs) {
        if (ref.type != ReferenceType::Inheritance) continue;
        InterfaceInfo info;
        info.interface_id.file_id = ref.file_id;
        info.interface_id.name = ref.referenced_name;
        info.interface_id.type = SymbolType::Interface;
        info.interface_id.symbol_id = std::to_string(ref.target_symbol);
        info.is_fully_implemented = true;  // trap 5: Go hardcodes this stub.
        interfaces.push_back(std::move(info));
    }
    return interfaces;
}

// -- getInheritanceChain (context_lookup_structure.go:191) -------------------
// Same Class-only gate as getInterfaceImplementations (trap 6c).
std::vector<ObjectReference> get_inheritance_chain(const Snapshot& snap,
                                                   const CodeObjectID& oid) {
    std::vector<ObjectReference> chain;
    if (oid.type != SymbolType::Class) return chain;
    auto target = find_target(snap, oid);
    if (target == nullptr) return chain;

    for (const auto& ref : target->outgoing_refs) {
        if (ref.type != ReferenceType::Inheritance) continue;
        ObjectReference parent;
        parent.object_id.file_id = ref.file_id;
        parent.object_id.name = ref.referenced_name;
        parent.object_id.type = SymbolType::Class;
        parent.object_id.symbol_id = std::to_string(ref.target_symbol);
        parent.location.file_id = ref.file_id;
        parent.location.line = ref.line;
        parent.location.column = ref.column;
        parent.context = "inheritance";
        parent.confidence = 0.9;
        chain.push_back(std::move(parent));
    }
    return chain;
}

// -- countParameters (context_lookup_usage.go:526), ported minimally for the
// dependency-injection heuristic below: prefer the indexed ParameterCount,
// fall back to a comma count over the signature text.
int count_parameters(const Snapshot& snap, const CodeObjectID& oid) {
    if (oid.type != SymbolType::Function && oid.type != SymbolType::Method) {
        return 0;
    }
    auto target = find_target(snap, oid);
    if (target == nullptr) return 0;
    if (target->parameter_count > 0) {
        return static_cast<int>(target->parameter_count);
    }
    if (!target->signature.empty() &&
        target->signature.find('(') != std::string::npos) {
        return static_cast<int>(
                   std::count(target->signature.begin(),
                             target->signature.end(), ',')) +
               1;
    }
    return 0;
}

// -- Pattern detection (context_lookup_structure.go:728-1019) ----------------

bool has_dependency_injection(const Snapshot& snap, const CodeObjectID& oid) {
    if (oid.type != SymbolType::Function) return false;
    if (oid.name.rfind("New", 0) != 0) return false;  // strings.HasPrefix
    return count_parameters(snap, oid) >= 1;
}

bool has_composition_pattern(const Snapshot& snap, const CodeObjectID& oid) {
    if (oid.type != SymbolType::Struct) return false;
    auto target = find_target(snap, oid);
    if (target == nullptr) return false;
    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        if (sym->symbol.type != SymbolType::Field &&
            sym->symbol.type != SymbolType::Variable) {
            continue;
        }
        if (sym->symbol.line >= target->symbol.line &&
            sym->symbol.end_line <= target->symbol.end_line) {
            return true;
        }
    }
    return false;
}

bool has_inheritance_pattern(const Snapshot& snap, const CodeObjectID& oid) {
    if (oid.type != SymbolType::Struct) return false;
    auto target = find_target(snap, oid);
    if (target == nullptr) return false;
    for (const auto& ref : target->outgoing_refs) {
        if (ref.type == ReferenceType::Inheritance) return true;
    }
    return false;
}

bool has_factory_pattern(const CodeObjectID& oid) {
    if (oid.type != SymbolType::Function) return false;
    static constexpr std::string_view kPrefixes[] = {"Create", "Make",
                                                     "Build"};
    for (auto prefix : kPrefixes) {
        if (oid.name.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

bool has_singleton_pattern(const CodeObjectID& oid) {
    if (oid.type != SymbolType::Function) return false;
    static constexpr std::string_view kNames[] = {
        "Instance",   "GetInstance",   "Shared",
        "GetShared", "GetSingleton", "Singleton"};
    for (auto name : kNames) {
        if (oid.name == name) return true;
    }
    return false;
}

// -- determineCompositionPattern (context_lookup_structure.go:239) ----------
std::string determine_composition_pattern(const Snapshot& snap,
                                          const CodeObjectID& oid) {
    if (has_dependency_injection(snap, oid)) return "dependency-injection";
    if (has_composition_pattern(snap, oid)) return "composition";
    if (has_inheritance_pattern(snap, oid)) return "inheritance";
    if (has_factory_pattern(oid)) return "factory";
    if (has_singleton_pattern(oid)) return "singleton";
    return "simple";
}

}  // namespace

// Populates ctx.structure_context. Mirrors Go's fillStructureContext
// dispatch, reading ctx.object_id (refreshed by fill_basic_info).
void fill_structure_context(CodeObjectContext& ctx, const Snapshot& snap,
                            MasterIndex& indexer) {
    const CodeObjectID& oid = ctx.object_id;
    StructureContext& sc = ctx.structure_context;

    // get_file_path returns an ABSOLUTE path; every other emitted path in
    // this MCP surface is project-root-relative (relative_to_root is the
    // established helper — see handlers_core.cpp's `rel` lambda).
    sc.file_path = std::string(relative_to_root(
        indexer.get_file_path(oid.file_id), indexer.config().project.root));
    sc.module = extract_module_from_path(sc.file_path);
    sc.package = extract_package_from_path(sc.file_path);

    sc.imports = get_imports(snap, oid);
    sc.exports = get_exports(snap, oid);
    sc.interface_implementations = get_interface_implementations(snap, oid);
    sc.inheritance_chain = get_inheritance_chain(snap, oid);
    sc.composition_pattern = determine_composition_pattern(snap, oid);
}

}  // namespace lci
