#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lci {

/// Bitfield constants for side effect categories (uint32_t).
/// Multiple categories can be combined with bitwise OR.
/// Matches Go's SideEffectCategory (17 categories).
namespace side_effect {
inline constexpr uint32_t kNone = 0;

// Write effects - mutations to state
inline constexpr uint32_t kParamWrite = 1 << 0;
inline constexpr uint32_t kReceiverWrite = 1 << 1;
inline constexpr uint32_t kGlobalWrite = 1 << 2;
inline constexpr uint32_t kClosureWrite = 1 << 3;
inline constexpr uint32_t kFieldWrite = 1 << 4;

// I/O effects
inline constexpr uint32_t kIO = 1 << 5;
inline constexpr uint32_t kDatabase = 1 << 6;
inline constexpr uint32_t kNetwork = 1 << 7;

// Control flow effects
inline constexpr uint32_t kThrow = 1 << 8;
inline constexpr uint32_t kChannel = 1 << 9;
inline constexpr uint32_t kAsync = 1 << 10;

// Uncertainty markers (conservative flags)
inline constexpr uint32_t kExternalCall = 1 << 11;
inline constexpr uint32_t kDynamicCall = 1 << 12;
inline constexpr uint32_t kReflection = 1 << 13;
inline constexpr uint32_t kUncertain = 1 << 14;
inline constexpr uint32_t kIndirectWrite = 1 << 15;

// Aggregate masks
inline constexpr uint32_t kWriteMask = kParamWrite | kReceiverWrite | kGlobalWrite |
                                       kClosureWrite | kFieldWrite | kIndirectWrite;
inline constexpr uint32_t kIOMask = kIO | kDatabase | kNetwork;
inline constexpr uint32_t kUncertaintyMask = kExternalCall | kDynamicCall | kReflection |
                                             kUncertain;

/// Total number of individual side effect category flags.
inline constexpr int kCategoryCount = 16;
}  // namespace side_effect

/// Purity classification of a function (1-5 scale).
/// Based on two-phase analysis: internal effects + transitive dependencies.
enum class PurityLevel : uint8_t {
    Pure = 1,
    InternallyPure = 2,
    ObjectState = 3,
    ModuleGlobal = 4,
    ExternalDependency = 5,
};

/// Returns the string name for a PurityLevel value.
constexpr std::string_view to_string(PurityLevel pl) {
    switch (pl) {
        case PurityLevel::Pure: return "Pure";
        case PurityLevel::InternallyPure: return "InternallyPure";
        case PurityLevel::ObjectState: return "ObjectState";
        case PurityLevel::ModuleGlobal: return "ModuleGlobal";
        case PurityLevel::ExternalDependency: return "ExternalDependency";
    }
    return "Unknown";
}

/// Confidence level in purity classification.
enum class PurityConfidence : uint8_t {
    None = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    Proven = 4,
};

/// Returns the string name for a PurityConfidence value.
constexpr std::string_view to_string(PurityConfidence pc) {
    switch (pc) {
        case PurityConfidence::None: return "none";
        case PurityConfidence::Low: return "low";
        case PurityConfidence::Medium: return "medium";
        case PurityConfidence::High: return "high";
        case PurityConfidence::Proven: return "proven";
    }
    return "unknown";
}

/// Distinguishes reads from writes.
enum class AccessType : uint8_t {
    Read = 1,
    Write = 2,
};

/// Returns the string name for an AccessType value.
constexpr std::string_view to_string(AccessType at) {
    switch (at) {
        case AccessType::Read: return "read";
        case AccessType::Write: return "write";
    }
    return "unknown";
}

/// Classification of what is being accessed.
enum class AccessTarget : uint8_t {
    Local = 0,
    Parameter,
    Receiver,
    Global,
    Closure,
    Field,
    Index,
    Unknown,
};

/// Returns the string name for an AccessTarget value.
constexpr std::string_view to_string(AccessTarget at) {
    switch (at) {
        case AccessTarget::Local: return "local";
        case AccessTarget::Parameter: return "parameter";
        case AccessTarget::Receiver: return "receiver";
        case AccessTarget::Global: return "global";
        case AccessTarget::Closure: return "closure";
        case AccessTarget::Field: return "field";
        case AccessTarget::Index: return "index";
        case AccessTarget::Unknown: return "unknown";
    }
    return "unknown";
}

/// A single access to a variable/field.
struct FieldAccess {
    std::string target;
    AccessTarget target_type{};
    AccessType type{};
    int line{};
    int column{};
    int seq_num{};
    std::string base_identifier;
    std::vector<std::string> field_path;
};

/// Overall read/write pattern classification.
enum class AccessPatternType : uint8_t {
    Pure = 0,
    ReadThenWrite,
    WriteOnly,
    WriteThenRead,
    Interleaved,
    Unknown,
};

/// Returns the string name for an AccessPatternType value.
constexpr std::string_view to_string(AccessPatternType apt) {
    switch (apt) {
        case AccessPatternType::Pure: return "pure";
        case AccessPatternType::ReadThenWrite: return "read-then-write";
        case AccessPatternType::WriteOnly: return "write-only";
        case AccessPatternType::WriteThenRead: return "write-then-read";
        case AccessPatternType::Interleaved: return "interleaved";
        case AccessPatternType::Unknown: return "unknown";
    }
    return "unknown";
}

/// Returns true if the access pattern is considered clean/expected.
constexpr bool is_clean(AccessPatternType p) {
    return p == AccessPatternType::Pure || p == AccessPatternType::ReadThenWrite ||
           p == AccessPatternType::WriteOnly;
}

/// Access pattern for a single variable/field target.
struct TargetAccessPattern {
    std::string target;
    AccessTarget target_type{};
    AccessPatternType pattern{};
    int read_count{};
    int write_count{};
    std::string sequence;
    int first_read_line{};
    int first_write_line{};
    int first_read_after_write_line{};
};

/// Categorizes concerning access patterns.
enum class ViolationType : uint8_t {
    WriteBeforeRead = 0,
    ReadAfterWrite,
    InterleavedAccess,
    SelfInterference,
    MutateParameter,
    MutateReceiver,
};

/// Returns the string name for a ViolationType value.
constexpr std::string_view to_string(ViolationType vt) {
    switch (vt) {
        case ViolationType::WriteBeforeRead: return "write-before-read";
        case ViolationType::ReadAfterWrite: return "read-after-write";
        case ViolationType::InterleavedAccess: return "interleaved-access";
        case ViolationType::SelfInterference: return "self-interference";
        case ViolationType::MutateParameter: return "mutate-parameter";
        case ViolationType::MutateReceiver: return "mutate-receiver";
    }
    return "unknown";
}

/// A specific concerning access pattern violation.
struct PatternViolation {
    ViolationType type{};
    std::string target;
    int line{};
    int read_line{};
    int write_line{};
    std::string description;
    double severity{};
};

/// Summary of the read/write pattern for a function.
struct AccessPattern {
    std::vector<FieldAccess> accesses;
    AccessPatternType pattern{};
    std::vector<PatternViolation> violations;
    int total_reads{};
    int total_writes{};
    int unique_targets{};
    int parameter_writes{};
    int receiver_writes{};
    int global_writes{};
    int closure_writes{};
};

/// Details a write to a function parameter.
/// parameter_index value meaning "this write mutates a parameter, but which
/// one is not known" -- the written identifier was never tied back to the
/// signature. Consumers must render it as unknown rather than as a position;
/// it is excluded from PurityClassification::mutated_parameters.
inline constexpr int kUnknownParameterIndex = -1;

struct ParameterWriteInfo {
    std::string parameter_name;
    int parameter_index{kUnknownParameterIndex};
    int line{};
    int column{};
    std::vector<std::string> field_path;
    bool is_pointer{};
};

/// Details a write to global state.
struct GlobalWriteInfo {
    std::string global_name;
    int line{};
    int column{};
    std::vector<std::string> field_path;
    bool is_package{};
};

/// Details a call to an external/unknown function.
struct ExternalCallInfo {
    std::string function_name;
    int line{};
    int column{};
    bool is_method{};
    std::string receiver_type;
    std::string package;
    std::string reason;
};

/// Tracks a function call that needs Phase 2 resolution.
struct UnresolvedCallInfo {
    std::string function_name;
    std::string qualifier;
    bool is_method{};
    int line{};
    int column{};
};

/// Details a throw/panic/raise site.
struct ThrowSiteInfo {
    std::string type;
    int line{};
    int column{};
};

/// Error-handling / resource finding signals (design:
/// docs/plans/2026-08-17-error-handling-score-design.md). Syntactic + name
/// heuristics only (no CFG/dataflow) — every finding carries a confidence,
/// never a verdict.
enum class EhSignal : uint8_t {
    EmptyCatch = 0,
    CatchAndContinue,
    BroadCatch,
    LogAndSwallow,
    DroppedError,
    RethrowNoCause,
    LeakNoRelease,
    LeakOnErrorPath,
    UnguardedRelease,
};

constexpr std::string_view to_string(EhSignal s) {
    switch (s) {
        case EhSignal::EmptyCatch: return "empty-catch";
        case EhSignal::CatchAndContinue: return "catch-and-continue";
        case EhSignal::BroadCatch: return "broad-catch";
        case EhSignal::LogAndSwallow: return "log-and-swallow";
        case EhSignal::DroppedError: return "dropped-error";
        case EhSignal::RethrowNoCause: return "rethrow-no-cause";
        case EhSignal::LeakNoRelease: return "leak-no-release";
        case EhSignal::LeakOnErrorPath: return "leak-on-error-path";
        case EhSignal::UnguardedRelease: return "unguarded-release";
    }
    return "unknown";
}

enum class FindingSeverity : uint8_t { Low = 0, Med = 1, High = 2 };

constexpr std::string_view to_string(FindingSeverity s) {
    switch (s) {
        case FindingSeverity::Low: return "low";
        case FindingSeverity::Med: return "med";
        case FindingSeverity::High: return "high";
    }
    return "unknown";
}

/// One per-function error-handling or resource finding, with evidence line.
struct EhFinding {
    EhSignal signal{};
    FindingSeverity severity{};
    double confidence{};  // 0..1
    int line{};           // evidence line inside the function
    std::string detail;   // e.g. "caught=Exception", "`_ = w.Close()`"
};

/// Syntactic facts about one catch/except/rescue site, gathered by the
/// extractor's subtree walk and classified by SideEffectAnalyzer.
struct CatchSiteInfo {
    int line{};
    std::string caught_type;   // "" when the grammar carries no type
    bool body_empty{};         // no statements (comments don't count)
    bool broad_type{};         // Exception / Throwable / bare except / (...)
    bool has_rethrow{};        // throw/raise inside the body
    bool rethrow_uses_cause{}; // rethrow references the caught variable
    bool has_log_call{};       // a log-category callee in the body
    bool has_other_call{};     // any non-log call in the body
    bool has_return{};         // returns a value (error may be re-surfaced)
};

/// One acquire/release call site (syntactic resource pairing).
struct ResourceOp {
    std::string callee;  // bare callee name (last segment)
    int line{};
    bool guarded{};      // inside defer/errdefer/finally/ensure/using/with
};

/// Exception safety characteristics of a function.
struct ErrorHandlingInfo {
    bool can_throw{};
    bool returns_error{};
    bool exception_neutral{};
    bool exception_safe{};
    int defer_count{};
    int try_finally_count{};
    int throw_count{};
    int catch_count{};
    std::vector<int> error_return_lines;
};

/// Fine-grained purity classification vector.
struct PurityClassification {
    std::vector<int> mutated_parameters;
    bool mutates_receiver{};
    std::vector<std::string> mutated_globals;
    std::vector<std::string> mutated_closures;
    bool performs_io{};
    bool performs_network{};
    bool performs_database{};
    bool can_throw{};
};

/// Complete side-effect analysis for a function.
struct SideEffectInfo {
    // Function identification
    std::string function_name;
    std::string file_path;
    int start_line{};
    int end_line{};

    // Bitfield of detected side effect categories
    uint32_t categories{};

    // Confidence in the analysis
    PurityConfidence confidence{};

    // Purity level (two-phase analysis result)
    PurityLevel purity_level{PurityLevel::InternallyPure};

    // Fine-grained purity classification
    PurityClassification purity_classification;

    // Access pattern analysis (nullptr when not computed)
    AccessPattern access_pattern;
    bool has_access_pattern{};

    // Specific details about detected effects
    std::vector<ParameterWriteInfo> parameter_writes;
    std::vector<GlobalWriteInfo> global_writes;
    std::vector<ExternalCallInfo> external_calls;
    std::vector<ThrowSiteInfo> throw_sites;

    // Unresolved function calls (for Phase 2 propagation)
    std::vector<UnresolvedCallInfo> unresolved_calls;

    // Error handling analysis
    ErrorHandlingInfo error_handling;
    bool has_error_handling{};

    // Error-handling + resource findings (sorted by line; deterministic).
    std::vector<EhFinding> error_findings;
    std::vector<EhFinding> resource_findings;
    std::vector<ResourceOp> resource_acquires;
    std::vector<ResourceOp> resource_releases;

    // Transitive effects (from callees) - populated by propagation
    uint32_t transitive_categories{};
    // Per-hop-decayed confidence in the transitive assessment. A function that
    // directly holds an effect keeps full confidence (1.0); each call-graph hop
    // outward multiplies by ConfidenceDecay (0.95), floored at MinConfidence
    // (0.3). Continuous, matching Go's SideEffectPropagator.decayConfidence
    // (hence double, not the coarse PurityConfidence bucket). 0.0 until
    // propagate_transitive runs.
    double transitive_confidence{};

    // Combined assessment
    bool is_pure{};
    double purity_score{};
    double purity_confidence_score{};

    // Reasons for impurity
    std::vector<std::string> impurity_reasons;
};

/// Converts a side-effect category bitfield into human-readable category
/// names (e.g. "io", "global_write", "throw"). Empty for kNone. Shared by the
/// side_effects MCP handler and get_context purity emission (Go parity:
/// categoriesToStrings).
std::vector<std::string> categories_to_strings(uint32_t categories);

}  // namespace lci
