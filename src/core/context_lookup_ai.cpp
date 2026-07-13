// C++ port of the ai section of internal/core/context_lookup.go
// (context_lookup_ai.go: fillAIContext + its helpers). This is the LAST
// section fill (CLX S8) — it deliberately reads sections already populated
// by S3-S7 (direct_relationships, semantic_context, usage_analysis,
// structure_context) instead of recomputing anything.
//
// Traps preserved bug-for-bug (see the CLX port map, comment
// 01KXCYNRCFJECKP5EVTZ9A75B7):
//   trap 5  — similar-object finders (findObjectsWithSimilarStructure/Usage,
//             findObjectsWithPrefix/Suffix) and 3 smell predicates
//             (hasFeatureEnvy/hasDataClumps/hasInappropriateIntimacy) are Go
//             stubs that return empty/false unconditionally. The real
//             findSimilarObjects pipeline (name-similarity -> structure ->
//             usage -> dedup -> cap 5) therefore ALWAYS yields an empty
//             result under Go's current implementation, so similar_objects
//             is pinned as a constant empty list rather than porting the
//             always-empty finder chain. The 3 smell predicates are ported as
//             constant `false` (never fire) rather than real analysis.
//   trap 6a — generateRefactoringSuggestions reads context.AIContext.CodeSmells
//             BEFORE fillAIContext's own detectCodeSmells call populates it
//             (suggestions run 3rd, smells run 4th in Go's fill order) — the
//             smell-severity-driven suggestion line therefore NEVER fires.
//             TODO(clx-port): Go ordering bug, ported bug-for-bug rather than
//             reordered to "fix" it — see fill_ai_context below.

#include <lci/core/context_lookup.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

#include <lci/types.h>

namespace lci {

namespace {

std::string lower(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

// Go's determineSeverity(value, mediumThreshold, highThreshold).
std::string determine_severity(int value, int medium_threshold,
                               int high_threshold) {
    if (value >= high_threshold) return "critical";
    if (value >= medium_threshold) return "high";
    return "medium";
}

// Go's hasGoodNaming: reject short/generic/temp-ish names.
bool has_good_naming(const std::string& name) {
    if (name.size() < 3) return false;
    static const char* const kGeneric[] = {"temp", "data", "info",
                                           "item", "obj",  "val"};
    for (const char* g : kGeneric) {
        if (name == g) return false;
    }
    return name.find("temp") == std::string::npos &&
          name.find("tmp") == std::string::npos;
}

// Go's generateNaturalLanguageSummary. Reads only already-filled sections.
std::string generate_natural_language_summary(const CodeObjectContext& ctx) {
    std::ostringstream out;
    out << "This " << to_string(ctx.object_id.type) << " `"
        << ctx.object_id.name << "`";

    if (!ctx.semantic_context.purpose.empty()) {
        out << " is a " << ctx.semantic_context.purpose;
    }
    if (!ctx.structure_context.file_path.empty()) {
        out << " located in " << ctx.structure_context.file_path;
    }
    if (!ctx.direct_relationships.incoming_references.empty()) {
        out << " that is referenced by "
            << ctx.direct_relationships.incoming_references.size()
            << " other objects";
    }
    if (!ctx.direct_relationships.called_functions.empty()) {
        out << " and calls "
            << ctx.direct_relationships.called_functions.size()
            << " other functions";
    }
    if (ctx.usage_analysis.complexity_metrics.cyclomatic_complexity > 5) {
        out << " with high cyclomatic complexity ("
            << ctx.usage_analysis.complexity_metrics.cyclomatic_complexity
            << ")";
    }
    if (ctx.semantic_context.criticality_analysis.is_critical) {
        out << " and is marked as "
            << ctx.semantic_context.criticality_analysis.criticality_type
            << "-critical";
    }
    if (ctx.usage_analysis.call_frequency > 0) {
        out << ". It is called approximately "
            << ctx.usage_analysis.call_frequency << " times";
    }
    if (ctx.usage_analysis.test_coverage.has_tests) {
        out << " and has test coverage";
    } else {
        out << " but lacks test coverage";
    }
    out << ".";

    if (!ctx.semantic_context.service_dependencies.empty()) {
        const auto& services = ctx.semantic_context.service_dependencies;
        out << " It depends on " << services.size()
            << " external services including ";
        if (services.size() > 3) {
            out << services[0].service_name << " and others";
        } else {
            for (size_t i = 0; i < services.size(); ++i) {
                if (i > 0) out << ", ";
                out << services[i].service_name;
            }
        }
        out << ".";
    }

    if (ctx.usage_analysis.change_impact.estimated_impact > 7) {
        out << " Changes to this object would have high impact on the "
              "system.";
    }
    return out.str();
}

// Go's detectCodeSmells. feature-envy/data-clumps/inappropriate-intimacy are
// trap-5 constant-false predicates and never fire (see file header).
std::vector<CodeSmell> detect_code_smells(const CodeObjectContext& ctx) {
    std::vector<CodeSmell> smells;
    const auto& metrics = ctx.usage_analysis.complexity_metrics;

    if (metrics.line_count > 50) {
        CodeSmell s;
        s.type = "long-function";
        s.description = "Function is " + std::to_string(metrics.line_count) +
                        " lines long (consider < 30 lines)";
        s.severity = determine_severity(metrics.line_count, 30, 50);
        s.location = ctx.location;
        smells.push_back(std::move(s));
    }

    if (metrics.cyclomatic_complexity > 10) {
        CodeSmell s;
        s.type = "high-cyclomatic-complexity";
        s.description = "Cyclomatic complexity is " +
                        std::to_string(metrics.cyclomatic_complexity) +
                        " (consider < 10)";
        s.severity = determine_severity(metrics.cyclomatic_complexity, 10, 15);
        s.location = ctx.location;
        smells.push_back(std::move(s));
    }

    if (ctx.object_id.type == SymbolType::Class &&
        ctx.direct_relationships.child_objects.size() > 15) {
        int child_count =
            static_cast<int>(ctx.direct_relationships.child_objects.size());
        CodeSmell s;
        s.type = "god-class";
        s.description = "Class has " + std::to_string(child_count) +
                        " methods/fields (consider splitting "
                        "responsibilities)";
        s.severity = determine_severity(child_count, 10, 15);
        s.location = ctx.location;
        smells.push_back(std::move(s));
    }

    // trap 5: feature-envy / data-clumps / inappropriate-intimacy predicates
    // are Go stubs pinned to constant `false` — never fire, not ported as
    // real analysis.

    if (ctx.usage_analysis.change_impact.estimated_impact > 8) {
        CodeSmell s;
        s.type = "shotgun-surgery";
        s.description =
            "Changes to this object require modifications in many "
            "different places";
        s.severity = "high";
        s.location = ctx.location;
        smells.push_back(std::move(s));
    }

    return smells;
}

// Go's generateRefactoringSuggestions. `smells_so_far` is whatever
// ctx.ai_context.code_smells holds AT CALL TIME — trap 6a: fill_ai_context
// calls this BEFORE detect_code_smells runs, so smells_so_far is always
// empty here and the smell-driven suggestion line never fires.
std::vector<std::string> generate_refactoring_suggestions(
    const CodeObjectContext& ctx, const std::vector<CodeSmell>& smells_so_far) {
    std::vector<std::string> suggestions;
    const auto& metrics = ctx.usage_analysis.complexity_metrics;

    if (metrics.cyclomatic_complexity > 10) {
        suggestions.push_back(
            "Consider breaking down this function into smaller functions to "
            "reduce cyclomatic complexity");
    }
    if (metrics.cognitive_complexity > 15) {
        suggestions.push_back(
            "High cognitive complexity detected - consider simplifying "
            "logic and reducing nesting");
    }
    if (metrics.parameter_count > 5) {
        suggestions.push_back(
            "Too many parameters - consider using a parameter object or "
            "configuration struct");
    }
    if (ctx.usage_analysis.fan_in > 20) {
        suggestions.push_back(
            "This function is heavily used - consider adding comprehensive "
            "tests and documentation");
    }
    if (ctx.usage_analysis.fan_out > 10) {
        suggestions.push_back(
            "This function calls many other functions - consider applying "
            "the facade pattern");
    }
    if (!ctx.usage_analysis.test_coverage.has_tests) {
        suggestions.push_back(
            "Add unit tests to ensure reliability and prevent regressions");
    }
    if (ctx.semantic_context.service_dependencies.size() > 3) {
        suggestions.push_back(
            "Multiple service dependencies detected - consider implementing "
            "dependency injection");
    }
    for (const auto& smell : smells_so_far) {
        if (smell.severity == "high" || smell.severity == "critical") {
            suggestions.push_back("Address " + smell.type + ": " +
                                  smell.description);
        }
    }
    if (!has_good_naming(ctx.object_id.name)) {
        suggestions.push_back(
            "Consider using more descriptive names to improve code "
            "readability");
    }
    if (ctx.documentation.empty()) {
        suggestions.push_back(
            "Add documentation to explain the purpose and usage of this "
            "object");
    }
    return suggestions;
}

// Go's suggestBestPractices. The first two lines are unconditional, so the
// result is never empty once ai text is included.
std::vector<std::string> suggest_best_practices(const CodeObjectContext& ctx) {
    std::vector<std::string> practices;
    practices.push_back(
        "Follow consistent naming conventions across the codebase");
    practices.push_back(
        "Write self-documenting code that minimizes the need for comments");

    switch (ctx.object_id.type) {
        case SymbolType::Function:
        case SymbolType::Method:
            practices.push_back(
                "Keep functions small and focused on a single "
                "responsibility");
            practices.push_back(
                "Use pure functions when possible to improve testability");
            practices.push_back(
                "Validate input parameters at the beginning of functions");
            break;
        case SymbolType::Class:
            practices.push_back(
                "Design classes with high cohesion and low coupling");
            practices.push_back(
                "Prefer composition over inheritance when possible");
            practices.push_back(
                "Implement meaningful equals and hashCode methods");
            break;
        default:
            break;
    }

    if (ctx.semantic_context.criticality_analysis.is_critical) {
        practices.push_back(
            "Add comprehensive error handling and logging for critical "
            "code");
        practices.push_back(
            "Consider adding circuit breakers for external service calls");
    }
    if (!ctx.semantic_context.service_dependencies.empty()) {
        practices.push_back(
            "Implement proper error handling for external service calls");
        practices.push_back(
            "Add timeouts and retry logic for network operations");
    }
    if (!ctx.usage_analysis.test_coverage.has_tests) {
        practices.push_back(
            "Write tests before fixing bugs to ensure the fix works");
        practices.push_back(
            "Use test-driven development for new features");
    }
    if (ctx.usage_analysis.call_frequency > 100) {
        practices.push_back(
            "Consider performance optimizations for frequently called "
            "code");
        practices.push_back(
            "Add metrics and monitoring for high-traffic functions");
    }
    if (ctx.usage_analysis.complexity_metrics.cyclomatic_complexity > 5) {
        practices.push_back(
            "Consider extracting complex conditions into well-named "
            "boolean functions");
    }

    std::string lname = lower(ctx.object_id.name);
    if (lname.find("auth") != std::string::npos ||
        lname.find("password") != std::string::npos ||
        lname.find("token") != std::string::npos) {
        practices.push_back(
            "Follow security best practices for authentication/"
            "authorization code");
        practices.push_back(
            "Never log sensitive information like passwords or tokens");
        practices.push_back(
            "Use secure coding practices and validate all inputs");
    }

    return practices;
}

}  // namespace

// fill_ai_context — C++ port of internal/core/context_lookup_ai.go
// fillAIContext. Gated wholesale on `include_ai_text` (task spec: false
// skips/empties the entire section). Order preserved from Go bug-for-bug
// (trap 6a): summary -> similar_objects (constant empty, trap 5) ->
// refactoring_suggestions (reads EMPTY code_smells) -> code_smells (detected
// here, too late for the suggestions step above) -> best_practices.
void fill_ai_context(CodeObjectContext& ctx, bool include_ai_text) {
    if (!include_ai_text) return;

    ctx.ai_context.natural_language_summary =
        generate_natural_language_summary(ctx);

    // trap 5: similar_objects stays default-empty. Go's findSimilarObjects
    // pipeline (name/structure/usage finders -> dedup -> cap 5) always
    // yields empty under Go's current stub finder chain (see file header);
    // not ported as real analysis.

    ctx.ai_context.refactoring_suggestions =
        generate_refactoring_suggestions(ctx, ctx.ai_context.code_smells);

    ctx.ai_context.code_smells = detect_code_smells(ctx);

    ctx.ai_context.best_practices = suggest_best_practices(ctx);
}

}  // namespace lci
