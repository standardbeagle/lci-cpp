// C++ port of the variables section of internal/core/context_lookup.go
// (fillVariableContext + its live index-driven getters). Only the getters that
// feed VariableContext are ported; the Go file's tree-sitter AST helpers
// (extractReturnTypes, isWithinStructMethod, the *Query builders, etc.) are
// dead under the index-driven path and are intentionally NOT ported.
//
// Traps preserved bug-for-bug (see internal/core/context_lookup.go and the CLX
// port map):
//   trap 4  — VariableInfo.type is the REAL type string (EnhancedSymbol
//             type_info) for globals and class variables, but the symbol-KIND
//             string for locals and parameters.
//   trap 5  — return_values is ALWAYS [] (getReturnValues is a pinned stub).
//   trap 6d — getFunctionParameters uses Go's self-referential scope match; it
//             is replicated verbatim with a TODO marker.
//   trap 1  — Go map iteration is unordered; every emitted bucket is
//             std::stable_sort-ed deterministically so goldens compare
//             set-wise.

#include <lci/core/context_lookup.h>

#include <algorithm>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/core/reference_tracker.h>
#include <lci/reference.h>
#include <lci/scope.h>
#include <lci/symbol.h>

namespace lci {

namespace {

using Snapshot = ReferenceTracker::Snapshot;
using SymbolHandle = Snapshot::SymbolHandle;

// Deterministic ordering for a bucket (trap 1). Go's underlying iteration is
// unordered; sort by (line, column, name) so output is stable across runs.
void sort_bucket(std::vector<VariableInfo>& vars) {
    std::stable_sort(vars.begin(), vars.end(),
                     [](const VariableInfo& a, const VariableInfo& b) {
                         if (a.location.line != b.location.line)
                             return a.location.line < b.location.line;
                         if (a.location.column != b.location.column)
                             return a.location.column < b.location.column;
                         return a.name < b.name;
                     });
}

SourceLocation location_of(const EnhancedSymbol& sym) {
    return {sym.symbol.file_id, sym.symbol.line, sym.symbol.column};
}

// Resolves the target symbol the Go getters re-look-up by name: first match on
// file id (getLocalVariables/getFunctionParameters) — the type-aware variants
// pass require_type=true (getUsedGlobalVariables/getClassVariables).
SymbolHandle resolve_target(const Snapshot& snap, const CodeObjectID& oid,
                            bool require_type) {
    for (const auto& sym : snap.find_symbols_by_name(oid.name)) {
        if (sym->symbol.file_id != oid.file_id) continue;
        if (require_type && sym->symbol.type != oid.type) continue;
        return sym;
    }
    return nullptr;
}

// getGlobalVariables — file-level Variable/Constant symbols whose scope chain is
// entirely File/Package/Namespace. NOTE: the C++ Go index (like Go) prepends a
// folder scope with EndLine=0 to every symbol's scope chain, so this gate
// rejects real-file package globals — faithful to Go's getGlobalVariables.
std::vector<VariableInfo> get_global_variables(const Snapshot& snap,
                                               const CodeObjectID& oid) {
    std::vector<VariableInfo> globals;
    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        if (sym->symbol.type != SymbolType::Variable &&
            sym->symbol.type != SymbolType::Constant) {
            continue;
        }
        bool is_global = true;
        for (const auto& scope : sym->scope_chain) {
            if (scope.type != ScopeType::File &&
                scope.type != ScopeType::Package &&
                scope.type != ScopeType::Namespace) {
                is_global = false;
                break;
            }
        }
        if (!is_global) continue;

        VariableInfo info;
        info.name = std::string(sym->symbol.name);
        info.type = sym->type_info;  // trap 4: real type string.
        info.location = location_of(*sym);
        info.is_used = !sym->incoming_refs.empty();
        info.use_count = static_cast<int>(sym->incoming_refs.size());
        info.scope = "global";
        info.is_mutable = sym->is_mutable;
        globals.push_back(std::move(info));
    }
    sort_bucket(globals);
    return globals;
}

// getUsedGlobalVariables — the accessible globals actually named by the target's
// outgoing references.
std::vector<VariableInfo> get_used_global_variables(const Snapshot& snap,
                                                    const CodeObjectID& oid) {
    std::vector<VariableInfo> used;
    auto all_globals = get_global_variables(snap, oid);

    auto target = resolve_target(snap, oid, /*require_type=*/true);
    if (target == nullptr) {
        for (const auto& g : all_globals) {
            if (g.is_used) used.push_back(g);
        }
        return used;  // already deterministic: all_globals is sorted.
    }

    absl::flat_hash_map<std::string, VariableInfo> by_name;
    by_name.reserve(all_globals.size());
    for (const auto& g : all_globals) by_name.emplace(g.name, g);

    absl::flat_hash_set<std::string> seen;
    for (const auto& ref : target->outgoing_refs) {
        auto it = by_name.find(ref.referenced_name);
        if (it == by_name.end()) continue;
        if (seen.insert(it->second.name).second) used.push_back(it->second);
    }
    sort_bucket(used);
    return used;
}

// getClassVariables — Field/Variable symbols nested in the class/struct by BOTH
// line range AND scope-chain membership (dual gate). Accepts Class and Struct.
std::vector<VariableInfo> get_class_variables(const Snapshot& snap,
                                              const CodeObjectID& oid) {
    std::vector<VariableInfo> class_vars;
    if (oid.type != SymbolType::Class && oid.type != SymbolType::Struct) {
        return class_vars;
    }
    auto target = resolve_target(snap, oid, /*require_type=*/true);
    if (target == nullptr) return class_vars;

    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        if (sym->symbol.type != SymbolType::Field &&
            sym->symbol.type != SymbolType::Variable) {
            continue;
        }
        // Gate 1: line-range nesting inside the class/struct.
        if (sym->symbol.line < target->symbol.line ||
            sym->symbol.end_line > target->symbol.end_line) {
            continue;
        }
        // Gate 2: scope-chain membership by class/struct name.
        bool belongs = false;
        for (const auto& scope : sym->scope_chain) {
            if (scope.name == oid.name && (scope.type == ScopeType::Class ||
                                           scope.type == ScopeType::Struct)) {
                belongs = true;
                break;
            }
        }
        if (!belongs) continue;

        VariableInfo info;
        info.name = std::string(sym->symbol.name);
        info.type = sym->type_info;  // trap 4: real type string.
        info.location = location_of(*sym);
        info.is_used = !sym->incoming_refs.empty();
        info.use_count = static_cast<int>(sym->incoming_refs.size());
        info.scope = "class";
        info.is_mutable = sym->is_mutable;
        class_vars.push_back(std::move(info));
    }
    sort_bucket(class_vars);
    return class_vars;
}

// getLocalVariables — Variable symbols nested within a function/method's line
// range. Target lookup matches file id only (bug-for-bug with Go, which does
// not filter on type here).
std::vector<VariableInfo> get_local_variables(const Snapshot& snap,
                                              const CodeObjectID& oid) {
    std::vector<VariableInfo> locals;
    if (oid.type != SymbolType::Function && oid.type != SymbolType::Method) {
        return locals;
    }
    auto target = resolve_target(snap, oid, /*require_type=*/false);
    if (target == nullptr) return locals;

    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        if (sym->symbol.type != SymbolType::Variable) continue;
        if (sym->symbol.line < target->symbol.line ||
            sym->symbol.end_line > target->symbol.end_line) {
            continue;
        }
        int use_count = static_cast<int>(
            snap.get_symbol_references(sym->id, "both").size());

        VariableInfo info;
        info.name = std::string(sym->symbol.name);
        // trap 4: local type is the symbol-KIND string, not a real type.
        info.type = std::string(to_string(sym->symbol.type));
        info.location = location_of(*sym);
        info.is_used = use_count > 0;
        info.use_count = use_count;
        info.scope = "local";
        info.is_mutable = true;  // getLocalVariables hardcodes mutable locals.
        locals.push_back(std::move(info));
    }
    sort_bucket(locals);
    return locals;
}

// getFunctionParameters — Go's parameter heuristic. Target lookup matches file
// id only.
std::vector<VariableInfo> get_function_parameters(const Snapshot& snap,
                                                  const CodeObjectID& oid) {
    std::vector<VariableInfo> params;
    auto target = resolve_target(snap, oid, /*require_type=*/false);
    if (target == nullptr) return params;

    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        if (sym->symbol.type != SymbolType::Variable) continue;

        bool is_parameter = false;
        // Method 1: variable declared on the function's declaration line.
        if (sym->symbol.line == target->symbol.line) is_parameter = true;
        // Method 2: scope-chain parameter match.
        // TODO(clx-port): bug-for-bug parity with Go — self-referential param
        // scope match (a scope whose type is Variable and whose name equals the
        // symbol's own name is treated as a parameter marker).
        for (const auto& scope : sym->scope_chain) {
            if (scope.type == ScopeType::Variable &&
                scope.name == std::string(sym->symbol.name)) {
                is_parameter = true;
                break;
            }
        }
        if (!is_parameter) continue;

        int use_count = static_cast<int>(
            snap.get_symbol_references(sym->id, "both").size());

        VariableInfo info;
        info.name = std::string(sym->symbol.name);
        // trap 4: parameter type is the symbol-KIND string, not a real type.
        info.type = std::string(to_string(sym->symbol.type));
        info.location = location_of(*sym);
        info.is_used = use_count > 0;
        info.use_count = use_count;
        info.scope = "parameter";
        info.is_mutable = false;  // getFunctionParameters hardcodes immutable.
        params.push_back(std::move(info));
    }
    sort_bucket(params);
    return params;
}

}  // namespace

// Populates ctx.variable_context from the pinned snapshot. Mirrors Go's
// fillVariableContext dispatch, reading ctx.object_id (refreshed by
// fill_basic_info) as the objectID. The class-variable gate is widened to
// Struct: Go gates getClassVariables on SymbolTypeClass only, but its body
// already supports Struct and the CLX port requires struct fields to surface.
void fill_variable_context(CodeObjectContext& ctx, const Snapshot& snap) {
    const CodeObjectID& oid = ctx.object_id;

    ctx.variable_context.global_variables = get_global_variables(snap, oid);
    ctx.variable_context.used_globals = get_used_global_variables(snap, oid);

    if (oid.type == SymbolType::Class || oid.type == SymbolType::Struct) {
        ctx.variable_context.class_variables = get_class_variables(snap, oid);
    }

    ctx.variable_context.local_variables = get_local_variables(snap, oid);

    if (oid.type == SymbolType::Function || oid.type == SymbolType::Method) {
        ctx.variable_context.parameters = get_function_parameters(snap, oid);
        // trap 5: return_values is ALWAYS [] — getReturnValues is a pinned
        // stub in Go and is not ported. Leave the bucket default-empty.
    }
}

}  // namespace lci
