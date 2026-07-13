// C++ port of the relationships section of internal/core/context_lookup.go
// (context_lookup_relationships.go: fillDirectRelationships + its live
// index-driven getters). Only the index-driven live path is ported; the Go
// file's tree-sitter AST helpers (findStructNode/extractEmbeddedTypes/
// findInterfaceMethods/findAllStructs/findMethodsForType/
// extractReceiverTypeFromMethod/implementsInterface/extractImports/
// extractImportFromSpec/findExportedSymbols/isExported/findObjectBounds/
// findContainingFunction/findContainingScopes/extractSignature/
// extractDocumentation/getReferenceContext/determineSymbolType and
// isWithinObjectScope) are dead under the index-driven path and are
// intentionally NOT ported (see the CLX port map, comment
// 01KXCYNRCFJECKP5EVTZ9A75B7).
//
// Traps preserved bug-for-bug:
//   trap 3  — incoming/outgoing ref Context is the DECIMAL STRING of the Go
//             RefType ordinal. The C++ ReferenceType enum (include/lci/
//             reference.h) happens to declare its members in the SAME order
//             as Go's RefType iota (Import=0, Call=1, Inheritance=2,
//             Assignment=3, Declaration=4, Parameter=5, Return=6,
//             TypeAnnotation=7, Implements=8, Extends=9, Usage=10), so
//             static_cast<int>(ref.type) IS the Go ordinal already — no
//             remapping table needed, just stringify the underlying value.
//   trap 9  — imported_modules (this section) hardcodes import_style=
//             "direct", vs the structure section's imports which hardcode
//             "default" (context_lookup_structure.cpp) — an intentional Go
//             inconsistency, preserved rather than unified.
//   dispatch-gate bug (same shape as trap 6c in S4/S5) — getChildObjects
//             (context_lookup_relationships.go:465) gates methods/fields on
//             SymbolType::Class ONLY, excluding Struct: Go structs never
//             reach get_class_methods/get_class_fields via this dispatch.
//   dispatch-gate bug (unnamed in the port map, verified by direct reading of
//             context_lookup_relationships.go:88-101) — fillDirectRelationships
//             gates BOTH getParentClasses and getImplementingTypes behind
//             `objectID.Type == SymbolTypeClass || objectID.Type ==
//             SymbolTypeMethod`. getParentClasses' own body additionally
//             requires Struct||Class — so a Struct objectID (the only type
//             Go structs actually carry) never reaches it despite the getter
//             supporting Struct internally. getImplementingTypes' own body
//             requires Interface — which the outer gate (Class||Method) can
//             never satisfy. Both buckets are therefore UNREACHABLE-EMPTY by
//             construction, independent of any indexed data; ported as-is.
//
// Divergence NOT a named trap: getParentObjects only ever skips
// scope_chain[0] when its name equals the object's own name
// (context_lookup_relationships.go:412, comment claims "innermost to
// outermost" ordering). This C++ port's ReferenceTracker::
// build_symbol_scope_chain instead preserves the scopes_ discovery order
// (folder, file, then nested scopes in document-visit order) — outermost to
// innermost. The literal index-0 self-check is ported as-is; in this port's
// ordering it is a no-op in the common case (index 0 is folder/file, not the
// object's own scope).
//
// Divergence NOT a named trap: get_caller_functions/get_called_functions
// reuse ReferenceTracker::get_caller_symbols/get_callee_symbols (already
// RefTypeCall-filtered and deduplicated by target/source symbol id — see
// context_lookup_semantic.cpp's is_performance_critical/get_affected_
// components for the established precedent of calling straight through the
// live tracker rather than re-deriving from the pinned snapshot's raw
// outgoing_refs). Go's getCalledFunctions does not deduplicate multiple call
// sites to the same callee; this port's dedup-by-symbol-id is an accepted
// divergence for reusing the existing helper rather than hand-rolling a
// second call-graph scan.

#include <lci/core/context_lookup.h>

#include <string>

#include <absl/container/flat_hash_set.h>

#include <lci/core/reference_tracker.h>
#include <lci/reference.h>
#include <lci/scope.h>
#include <lci/symbol.h>

namespace lci {

namespace {

using Snapshot = ReferenceTracker::Snapshot;
using SymbolHandle = Snapshot::SymbolHandle;

// Resolves the target symbol by (name, file) only — matches Go's repeated
// `for _, sym := range symbols { if sym.FileID == objectID.FileID {...} }`
// scan used by getIncomingReferences/getOutgoingReferences/
// getCallerFunctions/getCalledFunctions/getParentObjects (no type filter;
// intentionally loose, bug-for-bug with Go).
SymbolHandle find_by_file(const Snapshot& snap, const CodeObjectID& oid) {
    for (const auto& sym : snap.find_symbols_by_name(oid.name)) {
        if (sym->symbol.file_id == oid.file_id) return sym;
    }
    return nullptr;
}

// Resolves the target symbol by (name, file, type) — used by the getters
// that additionally gate on the object's own SymbolType (getParentClasses,
// getUsedTypes).
SymbolHandle find_by_file_type(const Snapshot& snap, const CodeObjectID& oid) {
    for (const auto& sym : snap.find_symbols_by_name(oid.name)) {
        if (sym->symbol.file_id != oid.file_id) continue;
        if (sym->symbol.type != oid.type) continue;
        return sym;
    }
    return nullptr;
}

// trap 3: Go's `fmt.Sprintf("%d", ref.Type)`. The C++ ReferenceType enum
// shares Go's RefType ordinal order, so the underlying value IS the ordinal.
std::string ref_type_ordinal(ReferenceType type) {
    return std::to_string(static_cast<int>(type));
}

// -- getIncomingReferences (context_lookup_relationships.go:121) ------------
std::vector<ObjectReference> get_incoming_references(const Snapshot& snap,
                                                     const CodeObjectID& oid,
                                                     double threshold) {
    std::vector<ObjectReference> refs;
    auto target = find_by_file(snap, oid);
    if (target == nullptr) return refs;

    for (const auto& ref : snap.get_symbol_references(target->id, "incoming")) {
        auto source = snap.get_enhanced_symbol(ref.source_symbol);
        if (source == nullptr) continue;

        ObjectReference obj;
        obj.object_id.file_id = source->symbol.file_id;
        obj.object_id.name = std::string(source->symbol.name);
        obj.object_id.type = source->symbol.type;
        obj.object_id.symbol_id = std::to_string(source->id);
        obj.location.file_id = ref.file_id;
        obj.location.line = ref.line;
        obj.location.column = ref.column;
        obj.context = ref_type_ordinal(ref.type);
        obj.confidence = 0.9;
        refs.push_back(std::move(obj));
    }

    ContextLookupEngine::sort_by_confidence_desc(refs);
    return ContextLookupEngine::filter_high_confidence(refs, threshold);
}

// -- getOutgoingReferences (context_lookup_relationships.go:180) -----------
std::vector<ObjectReference> get_outgoing_references(const Snapshot& snap,
                                                     const CodeObjectID& oid,
                                                     double threshold) {
    std::vector<ObjectReference> refs;
    auto target = find_by_file(snap, oid);
    if (target == nullptr) return refs;

    for (const auto& ref : snap.get_symbol_references(target->id, "outgoing")) {
        auto tgt_sym = snap.get_enhanced_symbol(ref.target_symbol);
        if (tgt_sym == nullptr) continue;

        ObjectReference obj;
        obj.object_id.file_id = tgt_sym->symbol.file_id;
        obj.object_id.name = std::string(tgt_sym->symbol.name);
        obj.object_id.type = tgt_sym->symbol.type;
        obj.object_id.symbol_id = std::to_string(tgt_sym->id);
        obj.location.file_id = ref.file_id;
        obj.location.line = ref.line;
        obj.location.column = ref.column;
        obj.context = ref_type_ordinal(ref.type);
        obj.confidence = 0.9;
        refs.push_back(std::move(obj));
    }

    // trap: outgoing dedup keys on {name/symbol, context} — reuse the
    // engine's existing dedup_references (keys on object_id.symbol_id +
    // context), equivalent here since symbol_id already identifies the same
    // resolved target as Go's {Name, FileID} pair would.
    ContextLookupEngine::dedup_references(refs);
    ContextLookupEngine::sort_by_confidence_desc(refs);
    return ContextLookupEngine::filter_high_confidence(refs, threshold);
}

// -- getCallerFunctions (context_lookup_relationships.go:240) --------------
// Reuses ReferenceTracker::get_caller_symbols (already RefTypeCall-filtered
// and deduplicated by source symbol id).
std::vector<ObjectReference> get_caller_functions(const Snapshot& snap,
                                                  ReferenceTracker& tracker,
                                                  const CodeObjectID& oid,
                                                  double threshold) {
    std::vector<ObjectReference> callers;
    if (oid.type != SymbolType::Function && oid.type != SymbolType::Method) {
        return callers;
    }
    auto target = find_by_file(snap, oid);
    if (target == nullptr) return callers;

    for (SymbolID caller_id : tracker.get_caller_symbols(target->id)) {
        auto source = snap.get_enhanced_symbol(caller_id);
        if (source == nullptr) continue;
        if (source->symbol.type != SymbolType::Function &&
            source->symbol.type != SymbolType::Method) {
            continue;
        }

        ObjectReference obj;
        obj.object_id.file_id = source->symbol.file_id;
        obj.object_id.name = std::string(source->symbol.name);
        obj.object_id.type = source->symbol.type;
        obj.object_id.symbol_id = std::to_string(source->id);
        obj.location.file_id = source->symbol.file_id;
        obj.location.line = source->symbol.line;
        obj.location.column = source->symbol.column;
        obj.context = "function_call";
        obj.confidence = 0.95;
        callers.push_back(std::move(obj));
    }

    ContextLookupEngine::sort_by_confidence_desc(callers);
    return ContextLookupEngine::filter_high_confidence(callers, threshold);
}

// -- getCalledFunctions (context_lookup_relationships.go:311) --------------
// Reuses ReferenceTracker::get_callee_symbols (already RefTypeCall-filtered
// and deduplicated by target symbol id).
std::vector<ObjectReference> get_called_functions(const Snapshot& snap,
                                                   ReferenceTracker& tracker,
                                                   const CodeObjectID& oid,
                                                   double threshold) {
    std::vector<ObjectReference> called;
    if (oid.type != SymbolType::Function && oid.type != SymbolType::Method) {
        return called;
    }
    auto target = find_by_file(snap, oid);
    if (target == nullptr) return called;

    for (SymbolID callee_id : tracker.get_callee_symbols(target->id)) {
        auto callee = snap.get_enhanced_symbol(callee_id);
        if (callee == nullptr) continue;
        if (callee->symbol.type != SymbolType::Function &&
            callee->symbol.type != SymbolType::Method) {
            continue;
        }

        ObjectReference obj;
        obj.object_id.file_id = callee->symbol.file_id;
        obj.object_id.name = std::string(callee->symbol.name);
        obj.object_id.type = callee->symbol.type;
        obj.object_id.symbol_id = std::to_string(callee->id);
        obj.location.file_id = callee->symbol.file_id;
        obj.location.line = callee->symbol.line;
        obj.location.column = callee->symbol.column;
        obj.context = "function_call";
        obj.confidence = 0.95;
        called.push_back(std::move(obj));
    }

    ContextLookupEngine::sort_by_confidence_desc(called);
    return ContextLookupEngine::filter_high_confidence(called, threshold);
}

// convertScopeTypeToSymbolType (context_lookup_relationships.go:443). Ported
// verbatim, including the missing ScopeType::Struct case (Go's own switch
// has no Struct arm — it falls to the Variable default. This C++ extractor
// happens to tag Go struct/class-shaped scopes as ScopeType::Class rather
// than ::Struct, so the gap is dormant on this corpus, but the switch stays
// faithful to Go rather than adding a case Go never had.)
SymbolType convert_scope_type_to_symbol_type(ScopeType scope_type) {
    switch (scope_type) {
        case ScopeType::File:
        case ScopeType::Folder:
            return SymbolType::Module;
        case ScopeType::Namespace:
            return SymbolType::Module;
        case ScopeType::Class:
            return SymbolType::Class;
        case ScopeType::Interface:
            return SymbolType::Interface;
        case ScopeType::Function:
        case ScopeType::Method:
            return SymbolType::Function;
        default:
            return SymbolType::Variable;
    }
}

// -- getParentObjects (context_lookup_relationships.go:382) ----------------
std::vector<ObjectReference> get_parent_objects(const Snapshot& snap,
                                                const CodeObjectID& oid) {
    std::vector<ObjectReference> parents;
    auto target = find_by_file(snap, oid);
    if (target == nullptr) return parents;

    const auto& chain = target->scope_chain;
    for (size_t i = 0; i < chain.size(); ++i) {
        const auto& scope = chain[i];
        if (i == 0 && scope.name == target->symbol.name) continue;
        if (scope.name.empty()) continue;

        ObjectReference parent;
        parent.object_id.file_id = oid.file_id;
        parent.object_id.name = scope.name;
        parent.object_id.type = convert_scope_type_to_symbol_type(scope.type);
        parent.object_id.symbol_id = "scope_" + scope.name;
        parent.location.file_id = oid.file_id;
        parent.location.line = scope.start_line;
        parent.location.column = 0;
        parent.context = std::string(to_string(scope.type));
        parent.confidence = 0.95;
        parents.push_back(std::move(parent));
    }
    return parents;
}

// -- getClassMethods (context_lookup_relationships.go:1200) ----------------
std::vector<ObjectReference> get_class_methods(const Snapshot& snap,
                                                const CodeObjectID& oid) {
    std::vector<ObjectReference> methods;
    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        if (sym->symbol.type != SymbolType::Method) continue;

        bool belongs = false;
        for (const auto& scope : sym->scope_chain) {
            if (scope.name == oid.name && (scope.type == ScopeType::Class ||
                                           scope.type == ScopeType::Interface)) {
                belongs = true;
                break;
            }
        }
        if (!belongs) continue;

        ObjectReference method;
        method.object_id.file_id = sym->symbol.file_id;
        method.object_id.name = std::string(sym->symbol.name);
        method.object_id.type = SymbolType::Method;
        method.object_id.symbol_id = std::to_string(sym->id);
        method.location.file_id = sym->symbol.file_id;
        method.location.line = sym->symbol.line;
        method.location.column = sym->symbol.column;
        method.context = "method";
        method.confidence = 0.95;
        methods.push_back(std::move(method));
    }
    return methods;
}

// -- getClassFields (context_lookup_relationships.go:1246) -----------------
// Dual gate (line-range + scope-chain membership), same shape as S4/S5's
// class-variable getters.
std::vector<ObjectReference> get_class_fields(const Snapshot& snap,
                                               const CodeObjectID& oid) {
    std::vector<ObjectReference> fields;

    SymbolHandle class_symbol;
    for (const auto& sym : snap.find_symbols_by_name(oid.name)) {
        if (sym->symbol.file_id == oid.file_id &&
            (sym->symbol.type == SymbolType::Class ||
             sym->symbol.type == SymbolType::Struct)) {
            class_symbol = sym;
            break;
        }
    }
    if (class_symbol == nullptr) return fields;

    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        if (sym->symbol.type != SymbolType::Variable &&
            sym->symbol.type != SymbolType::Field) {
            continue;
        }
        // Gate 1: line-range nesting inside the class/struct.
        if (sym->symbol.line < class_symbol->symbol.line ||
            sym->symbol.end_line > class_symbol->symbol.end_line) {
            continue;
        }
        // Gate 2: scope-chain membership.
        bool is_direct_member = false;
        for (const auto& scope : sym->scope_chain) {
            if (scope.name == oid.name) {
                is_direct_member = true;
                break;
            }
        }
        if (!is_direct_member) continue;

        ObjectReference field;
        field.object_id.file_id = sym->symbol.file_id;
        field.object_id.name = std::string(sym->symbol.name);
        field.object_id.type = SymbolType::Field;
        field.object_id.symbol_id = std::to_string(sym->id);
        field.location.file_id = sym->symbol.file_id;
        field.location.line = sym->symbol.line;
        field.location.column = sym->symbol.column;
        field.context = "field";
        field.confidence = 0.95;
        fields.push_back(std::move(field));
    }
    return fields;
}

// -- getModuleContents (context_lookup_relationships.go:1348) --------------
std::vector<ObjectReference> get_module_contents(const Snapshot& snap,
                                                  const CodeObjectID& oid) {
    std::vector<ObjectReference> contents;
    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        if (!sym->is_exported) continue;
        if (sym->scope_chain.size() > 1) continue;  // top-level only

        ObjectReference content;
        content.object_id.file_id = sym->symbol.file_id;
        content.object_id.name = std::string(sym->symbol.name);
        content.object_id.type = sym->symbol.type;
        content.object_id.symbol_id = std::to_string(sym->id);
        content.location.file_id = sym->symbol.file_id;
        content.location.line = sym->symbol.line;
        content.location.column = sym->symbol.column;
        content.context = "export";
        content.confidence = 0.95;
        contents.push_back(std::move(content));
    }
    return contents;
}

// -- getChildObjects (context_lookup_relationships.go:461) ------------------
// dispatch-gate bug: Class ONLY (excludes Struct) for methods/fields.
std::vector<ObjectReference> get_child_objects(const Snapshot& snap,
                                                const CodeObjectID& oid) {
    std::vector<ObjectReference> children;
    if (oid.type == SymbolType::Class) {
        auto methods = get_class_methods(snap, oid);
        children.insert(children.end(),
                       std::make_move_iterator(methods.begin()),
                       std::make_move_iterator(methods.end()));
        auto fields = get_class_fields(snap, oid);
        children.insert(children.end(),
                       std::make_move_iterator(fields.begin()),
                       std::make_move_iterator(fields.end()));
    }
    if (oid.type == SymbolType::Module) {
        auto contained = get_module_contents(snap, oid);
        children.insert(children.end(),
                       std::make_move_iterator(contained.begin()),
                       std::make_move_iterator(contained.end()));
    }
    ContextLookupEngine::sort_by_confidence_desc(children);
    return children;
}

// -- getParentClasses (context_lookup_relationships.go:607) ----------------
// dispatch-gate bug: fill_direct_relationships only calls this for
// Class||Method objects; this body's own Struct||Class gate then rejects
// Method. A Struct objectID (the only shape Go structs carry) never reaches
// this getter at all — see the file-level comment. Ported as-is.
std::vector<ObjectReference> get_parent_classes(const Snapshot& snap,
                                                 const CodeObjectID& oid) {
    std::vector<ObjectReference> parents;
    if (oid.type != SymbolType::Struct && oid.type != SymbolType::Class) {
        return parents;
    }
    auto target = find_by_file_type(snap, oid);
    if (target == nullptr) return parents;

    for (const auto& ref : target->outgoing_refs) {
        if (ref.type != ReferenceType::Inheritance) continue;

        ObjectReference parent;
        parent.object_id.file_id = ref.file_id;
        parent.object_id.name = ref.referenced_name;
        parent.object_id.type = SymbolType::Struct;
        parent.object_id.symbol_id = std::to_string(ref.target_symbol);
        parent.location.file_id = ref.file_id;
        parent.location.line = ref.line;
        parent.location.column = ref.column;
        parent.confidence = 0.95;
        parent.context = "inheritance";
        parents.push_back(std::move(parent));
    }
    return parents;
}

// -- getImplementingTypes (context_lookup_relationships.go:746) ------------
// dispatch-gate bug: fill_direct_relationships only calls this for
// Class||Method objects, but this body requires Interface — a gate the outer
// dispatch can never satisfy. Unreachable-empty by construction; ported
// as-is (see file-level comment).
std::vector<ObjectReference> get_implementing_types(const Snapshot& snap,
                                                     const CodeObjectID& oid) {
    std::vector<ObjectReference> implementations;
    if (oid.type != SymbolType::Interface) return implementations;

    SymbolHandle target;
    for (const auto& sym : snap.find_symbols_by_name(oid.name)) {
        if (sym->symbol.file_id == oid.file_id &&
            sym->symbol.type == SymbolType::Interface) {
            target = sym;
            break;
        }
    }
    if (target == nullptr) return implementations;

    for (const auto& ref : snap.get_symbol_references(target->id, "incoming")) {
        if (ref.type != ReferenceType::Inheritance) continue;
        auto impl_sym = snap.get_enhanced_symbol(ref.source_symbol);
        if (impl_sym == nullptr) continue;

        ObjectReference impl;
        impl.object_id.file_id = impl_sym->symbol.file_id;
        impl.object_id.name = std::string(impl_sym->symbol.name);
        impl.object_id.type = impl_sym->symbol.type;
        impl.object_id.symbol_id = std::to_string(impl_sym->id);
        impl.location.file_id = impl_sym->symbol.file_id;
        impl.location.line = impl_sym->symbol.line;
        impl.location.column = impl_sym->symbol.column;
        impl.confidence = 0.95;
        impl.context = "implements";
        implementations.push_back(std::move(impl));
    }
    return implementations;
}

// -- getUsedTypes (context_lookup_relationships.go:983) --------------------
std::vector<ObjectReference> get_used_types(const Snapshot& snap,
                                             const CodeObjectID& oid) {
    std::vector<ObjectReference> used_types;
    auto target = find_by_file_type(snap, oid);
    if (target == nullptr) return used_types;

    absl::flat_hash_set<std::string> seen;
    for (const auto& ref : target->outgoing_refs) {
        if (ref.type != ReferenceType::Declaration &&
            ref.type != ReferenceType::Assignment) {
            continue;
        }
        if (ref.referenced_name.empty()) continue;
        if (!seen.insert(ref.referenced_name).second) continue;

        auto type_sym = snap.get_enhanced_symbol(ref.target_symbol);
        SymbolType type_type =
            type_sym != nullptr ? type_sym->symbol.type : SymbolType::Type;

        ObjectReference used;
        used.object_id.file_id = oid.file_id;
        used.object_id.name = ref.referenced_name;
        used.object_id.type = type_type;
        used.object_id.symbol_id = std::to_string(ref.target_symbol);
        used.location.file_id = ref.file_id;
        used.location.line = ref.line;
        used.location.column = ref.column;
        used.confidence = 0.8;
        used.context = "type_reference";
        used_types.push_back(std::move(used));
    }
    return used_types;
}

// -- getImportedModules (context_lookup_relationships.go:1115) -------------
// trap 9: import_style hardcoded "direct" (vs the structure section's
// "default"). File-wide scan: every enhanced symbol in the file is checked
// for Import-type outgoing refs, dedup'd by module path.
std::vector<ModuleReference> get_imported_modules(const Snapshot& snap,
                                                   const CodeObjectID& oid) {
    std::vector<ModuleReference> modules;
    absl::flat_hash_set<std::string> seen;
    for (const auto& sym : snap.get_file_enhanced_symbols(oid.file_id)) {
        for (const auto& ref : sym->outgoing_refs) {
            if (ref.type != ReferenceType::Import) continue;
            if (ref.referenced_name.empty()) continue;
            if (!seen.insert(ref.referenced_name).second) continue;

            ModuleReference mod;
            mod.module_path = ref.referenced_name;
            mod.import_style = "direct";  // trap 9
            modules.push_back(std::move(mod));
        }
    }
    return modules;
}

}  // namespace

// Populates ctx.direct_relationships. Mirrors Go's fillDirectRelationships
// dispatch, reading ctx.object_id (refreshed by fill_basic_info).
// `tracker` is the engine's live ReferenceTracker (needed for
// get_caller_symbols/get_callee_symbols); `threshold` is the engine's
// confidence_threshold() at call time.
void fill_direct_relationships(CodeObjectContext& ctx, const Snapshot& snap,
                               ReferenceTracker& tracker, double threshold) {
    const CodeObjectID& oid = ctx.object_id;
    DirectRelationships& dr = ctx.direct_relationships;

    dr.incoming_references = get_incoming_references(snap, oid, threshold);
    dr.outgoing_references = get_outgoing_references(snap, oid, threshold);
    dr.caller_functions =
        get_caller_functions(snap, tracker, oid, threshold);
    dr.called_functions =
        get_called_functions(snap, tracker, oid, threshold);
    dr.parent_objects = get_parent_objects(snap, oid);
    dr.child_objects = get_child_objects(snap, oid);

    // dispatch-gate bug (see file-level comment): both getters are
    // unreachable-empty by construction for the objectID types that could
    // otherwise satisfy their internal gates.
    if (oid.type == SymbolType::Class || oid.type == SymbolType::Method) {
        dr.parent_classes = get_parent_classes(snap, oid);
        dr.implementing_types = get_implementing_types(snap, oid);
    }

    dr.used_types = get_used_types(snap, oid);
    dr.imported_modules = get_imported_modules(snap, oid);
}

}  // namespace lci
