#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <lci/types.h>

// -----------------------------------------------------------------------------
// CodeObjectContext value types — C++ port of internal/core/context_lookup.go.
//
// DIVERGENCE FROM GO (trap 2, documented once here for the whole type surface):
// Go emits nil slices as JSON `null`. This port stores empty std::vector and
// serializes them as `[]`. `[]` is the more useful shape for MCP consumers (a
// list they can iterate without a null check) and is applied UNIFORMLY across
// every list field below. Goldens that compare these sections must expect `[]`,
// not `null`, for empty collections.
// -----------------------------------------------------------------------------

namespace lci {

// Serializes a vector of to_json()-bearing structs into a JSON array. Empty
// vectors become `[]` (see divergence note above).
template <class T>
inline nlohmann::json ctx_json_array(const std::vector<T>& items) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : items) arr.push_back(item.to_json());
    return arr;
}

// Source location as Go marshals types.SymbolLocation: no json tags, so the
// field names surface capitalized (FileID/Line/Column).
struct SourceLocation {
    FileID file_id{};
    int line{};
    int column{};

    nlohmann::json to_json() const {
        return {{"FileID", file_id}, {"Line", line}, {"Column", column}};
    }
};

// Uniquely identifies a code object. symbol_id is the MCP-facing encoded id
// string (Go's SymbolID is a string field), so it round-trips through
// decode_symbol_id.
struct CodeObjectID {
    FileID file_id{};
    std::string symbol_id;
    std::string name;
    SymbolType type{};

    bool is_valid() const {
        return !name.empty() && file_id != 0 && !symbol_id.empty();
    }

    nlohmann::json to_json() const {
        return {{"file_id", file_id},
                {"symbol_id", symbol_id},
                {"name", name},
                {"type", std::string(to_string(type))}};
    }
};

struct ObjectReference {
    CodeObjectID object_id;
    SourceLocation location;
    std::string context;  // How it's referenced. Trap 3: incoming/outgoing refs
                          // carry a NUMERIC STRING of the Go RefType ordinal
                          // here — C++ ReferenceType ordinals differ, so the
                          // relationships fill (S3) must map to Go's integers.
    double confidence{};

    bool is_valid() const {
        return !object_id.name.empty() && object_id.file_id != 0;
    }

    nlohmann::json to_json() const {
        return {{"object_id", object_id.to_json()},
                {"location", location.to_json()},
                {"context", context},
                {"confidence", confidence}};
    }
};

struct ModuleReference {
    std::string module_path;
    std::string import_style;  // "direct", "aliased", "wildcard"
    std::vector<std::string> used_items;

    nlohmann::json to_json() const {
        return {{"module_path", module_path},
                {"import_style", import_style},
                {"used_items", used_items}};
    }
};

struct VariableInfo {
    std::string name;
    std::string type;  // Trap 4: real type string for globals/class vars,
                       // symbol-KIND string for locals/params.
    SourceLocation location;
    bool is_used{};
    int use_count{};
    std::string scope;  // "global", "class", "local"
    bool is_mutable{};

    nlohmann::json to_json() const {
        return {{"name", name},
                {"type", type},
                {"location", location.to_json()},
                {"is_used", is_used},
                {"use_count", use_count},
                {"scope", scope},
                {"is_mutable", is_mutable}};
    }
};

struct EntryPointRef {
    CodeObjectID entry_point_id;
    std::string type;
    std::string path;
    double confidence{};

    nlohmann::json to_json() const {
        return {{"entry_point_id", entry_point_id.to_json()},
                {"type", type},
                {"path", path},
                {"confidence", confidence}};
    }
};

struct ServiceRef {
    std::string service_name;
    std::string operation_type;
    std::string dependency_type;
    double confidence{};

    nlohmann::json to_json() const {
        return {{"service_name", service_name},
                {"operation_type", operation_type},
                {"dependency_type", dependency_type},
                {"confidence", confidence}};
    }
};

struct PropagationInfo {
    std::string label;
    CodeObjectID source;
    double strength{};
    std::string direction;

    nlohmann::json to_json() const {
        return {{"label", label},
                {"source", source.to_json()},
                {"strength", strength},
                {"direction", direction}};
    }
};

struct CriticalityInfo {
    bool is_critical{};
    std::string criticality_type;
    double impact_score{};
    std::vector<std::string> affected_components;

    nlohmann::json to_json() const {
        return {{"is_critical", is_critical},
                {"criticality_type", criticality_type},
                {"impact_score", impact_score},
                {"affected_components", affected_components}};
    }
};

struct ComplexityMetrics {
    int cyclomatic_complexity{};
    int cognitive_complexity{};
    int line_count{};
    int parameter_count{};
    int nesting_depth{};

    nlohmann::json to_json() const {
        return {{"cyclomatic_complexity", cyclomatic_complexity},
                {"cognitive_complexity", cognitive_complexity},
                {"line_count", line_count},
                {"parameter_count", parameter_count},
                {"nesting_depth", nesting_depth}};
    }
};

struct ChangeImpactInfo {
    std::string breaking_change_risk;
    std::vector<std::string> dependent_components;
    int estimated_impact{};
    bool requires_tests{};

    nlohmann::json to_json() const {
        return {{"breaking_change_risk", breaking_change_risk},
                {"dependent_components", dependent_components},
                {"estimated_impact", estimated_impact},
                {"requires_tests", requires_tests}};
    }
};

struct TestCoverageInfo {
    bool has_tests{};
    std::vector<std::string> test_file_paths;

    nlohmann::json to_json() const {
        return {{"has_tests", has_tests},
                {"test_file_paths", test_file_paths}};
    }
};

struct ImportInfo {
    std::string module_path;
    std::string import_name;
    std::string import_style;
    bool is_used{};

    nlohmann::json to_json() const {
        return {{"module_path", module_path},
                {"import_name", import_name},
                {"import_style", import_style},
                {"is_used", is_used}};
    }
};

struct ExportInfo {
    std::string name;
    std::string type;
    std::string export_style;
    std::vector<std::string> used_by;

    nlohmann::json to_json() const {
        return {{"name", name},
                {"type", type},
                {"export_style", export_style},
                {"used_by", used_by}};
    }
};

struct InterfaceInfo {
    CodeObjectID interface_id;
    std::vector<std::string> methods;
    bool is_fully_implemented{};

    nlohmann::json to_json() const {
        return {{"interface_id", interface_id.to_json()},
                {"methods", methods},
                {"is_fully_implemented", is_fully_implemented}};
    }
};

struct CodeSmell {
    std::string type;
    std::string description;
    std::string severity;
    SourceLocation location;

    nlohmann::json to_json() const {
        return {{"type", type},
                {"description", description},
                {"severity", severity},
                {"location", location.to_json()}};
    }
};

// -- Section structs ---------------------------------------------------------

struct DirectRelationships {
    std::vector<ObjectReference> incoming_references;
    std::vector<ObjectReference> caller_functions;
    std::vector<ObjectReference> parent_classes;
    std::vector<ObjectReference> implementing_types;
    std::vector<ObjectReference> outgoing_references;
    std::vector<ObjectReference> called_functions;
    std::vector<ObjectReference> used_types;
    std::vector<ModuleReference> imported_modules;
    std::vector<ObjectReference> parent_objects;
    std::vector<ObjectReference> child_objects;

    nlohmann::json to_json() const {
        return {{"incoming_references", ctx_json_array(incoming_references)},
                {"caller_functions", ctx_json_array(caller_functions)},
                {"parent_classes", ctx_json_array(parent_classes)},
                {"implementing_types", ctx_json_array(implementing_types)},
                {"outgoing_references", ctx_json_array(outgoing_references)},
                {"called_functions", ctx_json_array(called_functions)},
                {"used_types", ctx_json_array(used_types)},
                {"imported_modules", ctx_json_array(imported_modules)},
                {"parent_objects", ctx_json_array(parent_objects)},
                {"child_objects", ctx_json_array(child_objects)}};
    }
};

struct VariableContext {
    std::vector<VariableInfo> global_variables;
    std::vector<VariableInfo> used_globals;
    std::vector<VariableInfo> class_variables;
    std::vector<VariableInfo> local_variables;
    std::vector<VariableInfo> parameters;
    std::vector<VariableInfo> return_values;

    nlohmann::json to_json() const {
        return {{"global_variables", ctx_json_array(global_variables)},
                {"used_globals", ctx_json_array(used_globals)},
                {"class_variables", ctx_json_array(class_variables)},
                {"local_variables", ctx_json_array(local_variables)},
                {"parameters", ctx_json_array(parameters)},
                {"return_values", ctx_json_array(return_values)}};
    }
};

struct SemanticContext {
    std::vector<EntryPointRef> entry_point_dependencies;
    std::vector<ServiceRef> service_dependencies;
    std::vector<PropagationInfo> propagation_labels;
    CriticalityInfo criticality_analysis;
    std::string purpose;
    double confidence{};

    nlohmann::json to_json() const {
        return {{"entry_point_dependencies",
                 ctx_json_array(entry_point_dependencies)},
                {"service_dependencies", ctx_json_array(service_dependencies)},
                {"propagation_labels", ctx_json_array(propagation_labels)},
                {"criticality_analysis", criticality_analysis.to_json()},
                {"purpose", purpose},
                {"confidence", confidence}};
    }
};

struct StructureContext {
    std::string file_path;
    std::string module;
    std::string package;
    std::vector<ImportInfo> imports;
    std::vector<ExportInfo> exports;
    std::vector<InterfaceInfo> interface_implementations;
    std::vector<ObjectReference> inheritance_chain;
    std::string composition_pattern;

    nlohmann::json to_json() const {
        return {{"file_path", file_path},
                {"module", module},
                {"package", package},
                {"imports", ctx_json_array(imports)},
                {"exports", ctx_json_array(exports)},
                {"interface_implementations",
                 ctx_json_array(interface_implementations)},
                {"inheritance_chain", ctx_json_array(inheritance_chain)},
                {"composition_pattern", composition_pattern}};
    }
};

struct UsageAnalysis {
    int64_t call_frequency{};
    int fan_in{};
    int fan_out{};
    ComplexityMetrics complexity_metrics;
    ChangeImpactInfo change_impact;
    TestCoverageInfo test_coverage;

    nlohmann::json to_json() const {
        return {{"call_frequency", call_frequency},
                {"fan_in", fan_in},
                {"fan_out", fan_out},
                {"complexity_metrics", complexity_metrics.to_json()},
                {"change_impact", change_impact.to_json()},
                {"test_coverage", test_coverage.to_json()}};
    }
};

struct AIContext {
    std::string natural_language_summary;
    std::vector<ObjectReference> similar_objects;
    std::vector<std::string> refactoring_suggestions;
    std::vector<CodeSmell> code_smells;
    std::vector<std::string> best_practices;

    nlohmann::json to_json() const {
        return {{"natural_language_summary", natural_language_summary},
                {"similar_objects", ctx_json_array(similar_objects)},
                {"refactoring_suggestions", refactoring_suggestions},
                {"code_smells", ctx_json_array(code_smells)},
                {"best_practices", best_practices}};
    }
};

// -- Diagnostics -------------------------------------------------------------

// Go's LookupError has NO json tags, so its fields marshal capitalized
// (Code/Message/Field/Fatal). Preserve that shape.
struct LookupError {
    std::string code;
    std::string message;
    std::string field;
    bool fatal{};

    nlohmann::json to_json() const {
        return {{"Code", code},
                {"Message", message},
                {"Field", field},
                {"Fatal", fatal}};
    }
};

struct LookupDiagnostics {
    std::vector<LookupError> errors;    // omitempty
    std::vector<std::string> warnings;  // omitempty
    bool symbol_index_ready{};
    bool ref_tracker_ready{};
    bool call_graph_populated{};
    bool side_effects_ready{};
    int relationships_found{};
    int symbols_searched{};

    void add_error(LookupError err) { errors.push_back(std::move(err)); }
    void add_warning(std::string msg) { warnings.push_back(std::move(msg)); }

    bool has_fatal_error() const {
        for (const auto& e : errors) {
            if (e.fatal) return true;
        }
        return false;
    }

    nlohmann::json to_json() const {
        nlohmann::json out;
        if (!errors.empty()) out["errors"] = ctx_json_array(errors);
        if (!warnings.empty()) out["warnings"] = warnings;
        out["symbol_index_ready"] = symbol_index_ready;
        out["ref_tracker_ready"] = ref_tracker_ready;
        out["call_graph_populated"] = call_graph_populated;
        out["side_effects_ready"] = side_effects_ready;
        out["relationships_found"] = relationships_found;
        out["symbols_searched"] = symbols_searched;
        return out;
    }
};

// -- Top-level context -------------------------------------------------------

struct CodeObjectContext {
    CodeObjectID object_id;
    std::string signature;
    std::string documentation;
    SourceLocation location;

    DirectRelationships direct_relationships;
    VariableContext variable_context;
    SemanticContext semantic_context;
    StructureContext structure_context;
    UsageAnalysis usage_analysis;
    AIContext ai_context;

    std::string generated_at;
    std::string context_version;
    LookupDiagnostics diagnostics;

    nlohmann::json to_json() const {
        return {{"object_id", object_id.to_json()},
                {"signature", signature},
                {"documentation", documentation},
                {"location", location.to_json()},
                {"direct_relationships", direct_relationships.to_json()},
                {"variable_context", variable_context.to_json()},
                {"semantic_context", semantic_context.to_json()},
                {"structure_context", structure_context.to_json()},
                {"usage_analysis", usage_analysis.to_json()},
                {"ai_context", ai_context.to_json()},
                {"generated_at", generated_at},
                {"context_version", context_version},
                {"diagnostics", diagnostics.to_json()}};
    }
};

}  // namespace lci
