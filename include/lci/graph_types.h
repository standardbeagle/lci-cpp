#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lci/types.h>

namespace lci {

/// Type of code node in function call trees.
/// Matches Go's NodeType enum (11 types).
enum class NodeType : uint8_t {
    Function = 0,
    Loop,
    Condition,
    Switch,
    Async,
    Network,
    Database,
    CPUIntensive,
    FileIO,
    External,
    Recursive,
};

/// Returns the string name for a NodeType value.
constexpr std::string_view to_string(NodeType nt) {
    switch (nt) {
        case NodeType::Function: return "function";
        case NodeType::Loop: return "loop";
        case NodeType::Condition: return "condition";
        case NodeType::Switch: return "switch";
        case NodeType::Async: return "async";
        case NodeType::Network: return "network";
        case NodeType::Database: return "database";
        case NodeType::CPUIntensive: return "cpu_intensive";
        case NodeType::FileIO: return "file_io";
        case NodeType::External: return "external";
        case NodeType::Recursive: return "recursive";
    }
    return "unknown";
}

/// Architectural pattern annotation on a tree node.
struct NodeAnnotation {
    NodeType type{};
    std::string emoji;
    std::string description;
};

/// A node in the function call hierarchy.
struct TreeNode {
    std::string name;
    std::string file_path;
    int line{};
    int depth{};
    NodeType node_type{};
    std::vector<NodeAnnotation> annotations;
    std::vector<TreeNode> children;

    // Edit safety and stability indicators for coding agents
    int edit_risk_score{};
    std::vector<std::string> stability_tags;
    int dependency_count{};
    int dependent_count{};
    int impact_radius{};
    std::vector<std::string> safety_notes;
};

/// Options for tree construction.
struct TreeOptions {
    int max_depth{};
    std::string exclude_pattern;
    bool agent_mode{};
};

/// Complete call hierarchy of a function.
struct FunctionTree {
    std::string root_function;
    TreeNode root;
    int max_depth{};
    int total_nodes{};
    TreeOptions options;
};

/// Access control level for symbols.
enum class AccessLevel : uint8_t {
    Unknown = 0,
    Public,
    Private,
    Protected,
    Internal,
    Package,
};

/// Returns the string name for an AccessLevel value.
constexpr std::string_view to_string(AccessLevel al) {
    switch (al) {
        case AccessLevel::Public: return "public";
        case AccessLevel::Private: return "private";
        case AccessLevel::Protected: return "protected";
        case AccessLevel::Internal: return "internal";
        case AccessLevel::Package: return "package";
        case AccessLevel::Unknown: return "unknown";
    }
    return "unknown";
}

/// Type of relationship between symbols.
enum class RelationshipType : uint8_t {
    Extends = 0,
    Implements,
    Contains,
    ContainedBy,
    DependsOn,
    DependedOnBy,
    Calls,
    CalledBy,
    References,
    ReferencedBy,
    Imports,
    ImportedBy,
    FileCoLocated,
    TypeOf,
    HasType,
    ParentType,
    ChildType,
    CrossLanguage,
};

/// Returns the string name for a RelationshipType value.
constexpr std::string_view to_string(RelationshipType rt) {
    switch (rt) {
        case RelationshipType::Extends: return "extends";
        case RelationshipType::Implements: return "implements";
        case RelationshipType::Contains: return "contains";
        case RelationshipType::ContainedBy: return "contained_by";
        case RelationshipType::DependsOn: return "depends_on";
        case RelationshipType::DependedOnBy: return "depended_on_by";
        case RelationshipType::Calls: return "calls";
        case RelationshipType::CalledBy: return "called_by";
        case RelationshipType::References: return "references";
        case RelationshipType::ReferencedBy: return "referenced_by";
        case RelationshipType::Imports: return "imports";
        case RelationshipType::ImportedBy: return "imported_by";
        case RelationshipType::FileCoLocated: return "file_co_located";
        case RelationshipType::TypeOf: return "type_of";
        case RelationshipType::HasType: return "has_type";
        case RelationshipType::ParentType: return "parent_type";
        case RelationshipType::ChildType: return "child_type";
        case RelationshipType::CrossLanguage: return "cross_language";
    }
    return "unknown";
}

/// Type of dependency between symbols.
enum class DependencyType : uint8_t {
    Unknown = 0,
    Import,
    Inheritance,
    Composition,
    Aggregation,
    Association,
    Usage,
    Configuration,
    Runtime,
};

/// Returns the string name for a DependencyType value.
constexpr std::string_view to_string(DependencyType dt) {
    switch (dt) {
        case DependencyType::Import: return "import";
        case DependencyType::Inheritance: return "inheritance";
        case DependencyType::Composition: return "composition";
        case DependencyType::Aggregation: return "aggregation";
        case DependencyType::Association: return "association";
        case DependencyType::Usage: return "usage";
        case DependencyType::Configuration: return "configuration";
        case DependencyType::Runtime: return "runtime";
        case DependencyType::Unknown: return "unknown";
    }
    return "unknown";
}

/// Strength of a dependency.
enum class DependencyStrength : uint8_t {
    Weak = 0,
    Moderate,
    Strong,
    Critical,
};

/// Returns the string name for a DependencyStrength value.
constexpr std::string_view to_string(DependencyStrength ds) {
    switch (ds) {
        case DependencyStrength::Weak: return "weak";
        case DependencyStrength::Moderate: return "moderate";
        case DependencyStrength::Strong: return "strong";
        case DependencyStrength::Critical: return "critical";
    }
    return "unknown";
}

/// Type of function call.
enum class CallType : uint8_t {
    Direct = 0,
    Method,
    Callback,
    Dynamic,
    Recursive,
    Virtual,
    Interface,
    Async,
    Deferred,
};

/// Returns the string name for a CallType value.
constexpr std::string_view to_string(CallType ct) {
    switch (ct) {
        case CallType::Direct: return "direct";
        case CallType::Method: return "method";
        case CallType::Callback: return "callback";
        case CallType::Dynamic: return "dynamic";
        case CallType::Recursive: return "recursive";
        case CallType::Virtual: return "virtual";
        case CallType::Interface: return "interface";
        case CallType::Async: return "async";
        case CallType::Deferred: return "deferred";
    }
    return "unknown";
}

/// Type of cross-language link.
enum class CrossLinkType : uint8_t {
    FFI = 0,
    API,
    RPC,
    Message,
    SharedData,
    Config,
    Build,
};

/// Returns the string name for a CrossLinkType value.
constexpr std::string_view to_string(CrossLinkType clt) {
    switch (clt) {
        case CrossLinkType::FFI: return "ffi";
        case CrossLinkType::API: return "api";
        case CrossLinkType::RPC: return "rpc";
        case CrossLinkType::Message: return "message";
        case CrossLinkType::SharedData: return "shared_data";
        case CrossLinkType::Config: return "config";
        case CrossLinkType::Build: return "build";
    }
    return "unknown";
}

/// Type of annotation on a symbol.
enum class AnnotationType : uint8_t {
    Decorator = 0,
    Attribute,
    Pragma,
    Directive,
    Comment,
    Generic,
};

/// Returns the string name for an AnnotationType value.
constexpr std::string_view to_string(AnnotationType at) {
    switch (at) {
        case AnnotationType::Decorator: return "decorator";
        case AnnotationType::Attribute: return "attribute";
        case AnnotationType::Pragma: return "pragma";
        case AnnotationType::Directive: return "directive";
        case AnnotationType::Comment: return "comment";
        case AnnotationType::Generic: return "generic";
    }
    return "unknown";
}

/// An argument in a function call.
struct CallArgument {
    std::string name;
    std::string type;
    std::string value;
    bool is_literal{};
};

/// A dependency relationship with additional context.
struct SymbolDependency {
    SymbolID target{};
    DependencyType type{};
    DependencyStrength strength{};
    std::string context;
    std::string import_path;
    bool is_optional{};
    bool is_conditional{};
};

/// Location of a symbol in source code.
struct SymbolLocation {
    FileID file_id{};
    int start_line{};
    int end_line{};
    int start_column{};
    int end_column{};
};

/// A function call with context.
struct FunctionCall {
    SymbolID target{};
    CallType call_type{};
    SymbolLocation location;
    std::string context;
    bool is_async{};
    bool is_recursive{};
    std::vector<CallArgument> arguments;
};

/// A cross-language link between symbols.
struct CrossLanguageLink {
    SymbolID target{};
    CrossLinkType link_type{};
    std::string language;
    std::string bridge;
    double confidence{};
    std::string description;
};

/// A location with high symbol usage.
struct UsageHotSpot {
    FileID file_id{};
    int start_line{};
    int end_line{};
    int usage_count{};
    std::string usage_type;
    double intensity{};
};

/// A language-specific annotation on a symbol.
struct SymbolAnnotation {
    AnnotationType type{};
    std::string value;
    std::vector<std::string> parameters;
    SymbolLocation location;
};

/// All relationship types for a symbol.
struct SymbolRelationships {
    std::vector<SymbolID> extends;
    std::vector<SymbolID> implements;
    std::vector<SymbolID> contains;
    SymbolID contained_by{};
    bool has_contained_by{};
    std::vector<SymbolDependency> dependencies;
    std::vector<SymbolID> dependents;
    std::vector<FunctionCall> calls_to;
    std::vector<FunctionCall> called_by;
    std::vector<SymbolID> file_co_located;
    std::vector<CrossLanguageLink> cross_language;
};

/// Access control and scope information for a symbol.
struct SymbolVisibilityInfo {
    AccessLevel access{};
    bool is_exported{};
    bool is_external{};
    bool is_builtin{};
    bool is_generated{};
};

/// Usage statistics and metrics for a symbol.
struct SymbolUsage {
    int reference_count{};
    int import_count{};
    int inheritance_count{};
    int call_count{};
    int modification_count{};
    std::vector<FileID> referencing_files;
    std::vector<UsageHotSpot> hot_spots;
};

/// Additional contextual information for a symbol.
struct SymbolMetadata {
    std::vector<std::string> documentation;
    std::vector<std::string> comments;
    std::vector<ContextAttribute> attributes;
    std::vector<SymbolAnnotation> annotations;
    int complexity_score{};
    int coupling_score{};
    int cohesion_score{};
    int edit_risk_score{};
    std::vector<std::string> stability_tags;
    std::vector<std::string> safety_notes;
};

/// Core identification information for a symbol in the universal graph.
struct SymbolIdentity {
    SymbolID id{};
    std::string name;
    std::string full_name;
    SymbolType kind{};
    std::string language;
    SymbolLocation location;
    std::string signature;
    std::string type;
    std::string value;
};

/// A node in the Universal Symbol Graph.
struct UniversalSymbolNode {
    SymbolIdentity identity;
    SymbolRelationships relationships;
    SymbolVisibilityInfo visibility;
    SymbolUsage usage;
    SymbolMetadata metadata;
};

}  // namespace lci
