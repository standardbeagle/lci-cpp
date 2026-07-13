// C++ port of the usage section of internal/core/context_lookup.go
// (context_lookup_usage.go: fillUsageAnalysis + its live getters). Only the
// getters that read EnhancedSymbol/live-index data are ported; the Go file's
// tree-sitter AST helpers (findFunctionNode/countDecisionPoints/
// countCognitiveComplexity/findFunctionBody/countNonEmptyLines/
// findAndCountParameters/countParametersInNode/countIdentifiersInParameter/
// countParametersFromText/isGoTypeKeyword/findMaxNestingDepth/
// calculateMatchNestingDepth) are dead under the index-driven live path and
// are intentionally NOT ported (see the CLX port map, comment
// 01KXCYNRCFJECKP5EVTZ9A75B7).
//
// Trap 10 (MANDATORY empirical verification, done BEFORE writing this file):
// the port map claims fan_in's underlying Go helper (findContainingFunction)
// "returns nil" in some case. Rather than trust that secondhand summary, the
// actual Go binary (/home/beagle/work/core/lci/lci) was driven live over
// stdio MCP JSON-RPC (initialize -> tools/call "search" -> tools/call
// "get_context" mode=full) against a hand-built Go fixture:
//
//   func target(x int) int { y := helperA(x); z := helperB(y); return y+z }
//   func helperA(x int) int { return x + 1 }
//   func helperB(x int) int { return x * 2 }
//   func callerOne() int { return target(1) }
//   func callerTwo() int { return target(2) }
//   func callerThree() int { return target(3) }
//
// Go's own get_context response for `target` showed direct_relationships.
// caller_functions correctly populated with all 3 real callers (confidence
// 0.95 each) — yet usage_analysis.fan_in was 0 and usage_analysis.
// call_frequency was 0, while usage_analysis.fan_out was correctly 2. Reading
// context_lookup_usage.go confirms why: calculateFanIn and
// calculateCallFrequency both route through the LEGACY
// `cle.symbolIndex.FindReferences` + `findContainingFunction` /
// `isFunctionCall` path — the same tree-sitter-AST-based machinery the S3
// port map already identified as dead under the index-driven live path (see
// context_lookup_relationships.cpp's file-level comment) — which never
// populates under this build and yields zero every time, regardless of real
// caller count. calculateFanOut, by contrast, routes through
// `getCalledFunctions` (the index-driven live path, already ported in S3),
// and is correctly non-zero. This port therefore pins fan_in and
// call_frequency to 0 as constant stubs (same treatment as trap 5's
// test_coverage/requires_tests) rather than deriving them from
// direct_relationships.caller_functions.size(), matching OBSERVED Go
// behavior rather than a plausible-looking (but wrong) derivation.
//
// Trap 5 (stubs, not implemented): requires_tests is always true;
// test_coverage is always {has_tests:false, test_file_paths:[]} — Go's
// findTestFiles builds testPatterns but never appends to testFiles
// (`_ = testPatterns; return testFiles`), so has_tests is always false.

#include <lci/core/context_lookup.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include <absl/container/flat_hash_set.h>

#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>
#include <lci/symbol.h>

namespace lci {

namespace {

using Snapshot = ReferenceTracker::Snapshot;
using SymbolHandle = Snapshot::SymbolHandle;

// Resolves the target symbol by (name, file, type) — same lookup shape as
// the sibling S4/S5/S7 files' resolve_target/find_target.
SymbolHandle find_target(const Snapshot& snap, const CodeObjectID& oid) {
    for (const auto& sym : snap.find_symbols_by_name(oid.name)) {
        if (sym->symbol.file_id != oid.file_id) continue;
        if (sym->symbol.type != oid.type) continue;
        return sym;
    }
    return nullptr;
}

bool is_function_or_method(SymbolType type) {
    return type == SymbolType::Function || type == SymbolType::Method;
}

// extractModuleFromPath (context_lookup_structure.go / duplicated locally per
// this codebase's established convention of per-TU static helpers — see
// context_lookup_structure.cpp's identically-named helper). A "src" substring
// in the containing directory switches module to the path remainder after
// "src"; otherwise module falls back to the containing directory's base name.
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
    std::string base = dir_path.filename().string();
    return base.empty() ? "." : base;
}

// countParameters (context_lookup_usage.go:526). ParameterCount from the
// indexed symbol is the primary source; the fallback parses the signature's
// parenthesized parameter list by counting commas (Go's
// countParametersFromText heuristic, simplified to the comma-count shortcut
// Go's own countParameters function actually uses inline).
int count_parameters(const EnhancedSymbol& sym) {
    if (sym.parameter_count > 0) return static_cast<int>(sym.parameter_count);
    if (!sym.signature.empty() &&
        sym.signature.find('(') != std::string::npos) {
        int commas = static_cast<int>(
            std::count(sym.signature.begin(), sym.signature.end(), ','));
        return commas + 1;
    }
    return 0;
}

// calculateComplexityMetrics (context_lookup_usage.go:124). Only computed for
// Function/Method objects; every field zeroes for other types (matching each
// individual Go helper's own type gate).
ComplexityMetrics calculate_complexity_metrics(const Snapshot& snap,
                                               const CodeObjectID& oid) {
    ComplexityMetrics metrics;
    if (!is_function_or_method(oid.type)) return metrics;

    auto target = find_target(snap, oid);
    if (target == nullptr) {
        metrics.cyclomatic_complexity = 1;  // Go default when unresolved.
        return metrics;
    }

    metrics.cyclomatic_complexity = target->complexity;
    // calculateCognitiveComplexity's live-path fallback reads the SAME
    // EnhancedSymbol.Complexity field as cyclomatic (context_lookup_usage.go:
    // 292-317) — the AST-walking cognitive-complexity counter is dead under
    // this index-driven path.
    metrics.cognitive_complexity = target->complexity;
    if (target->symbol.end_line > target->symbol.line) {
        metrics.line_count = target->symbol.end_line - target->symbol.line;
    }
    metrics.parameter_count = count_parameters(*target);
    // calculateNestingDepth: complexity/2 heuristic, gated on complexity>1.
    if (target->complexity > 1) metrics.nesting_depth = target->complexity / 2;
    return metrics;
}

// assessBreakingChangeRisk (context_lookup_usage.go:922).
std::string assess_breaking_change_risk(int fan_in, int fan_out) {
    if (fan_in > 10 || fan_out > 10) return "high";
    if (fan_in > 3 || fan_out > 3) return "medium";
    return "low";
}

// findDependentComponents (context_lookup_usage.go:937). Extracts one module
// name per caller (via its file path), deduped, in first-seen order — reuses
// ctx.direct_relationships.caller_functions (already computed by S3's
// fill_direct_relationships, which runs before this section) rather than
// re-deriving the caller list a second time (trap 8 hoist convention).
std::vector<std::string> find_dependent_components(
    const std::vector<ObjectReference>& caller_functions,
    MasterIndex& indexer) {
    std::vector<std::string> dependents;
    absl::flat_hash_set<std::string> seen;
    for (const auto& caller : caller_functions) {
        std::string path = indexer.get_file_path(caller.object_id.file_id);
        if (path.empty()) continue;
        std::string module = extract_module_from_path(path);
        if (seen.insert(module).second) dependents.push_back(std::move(module));
    }
    return dependents;
}

// isPublicAPI (context_lookup_usage.go:1033): exported names start uppercase
// (and are not underscore-prefixed).
bool is_public_api(const std::string& name) {
    if (name.empty()) return false;
    if (name.front() == '_') return false;
    return std::isupper(static_cast<unsigned char>(name.front())) != 0;
}

// calculateImpactScore (context_lookup_usage.go:957).
int calculate_impact_score(int fan_in, int fan_out, int cyclomatic,
                           const std::string& name) {
    int score = 1;
    score += std::min(fan_in / 2, 5);
    score += std::min(fan_out / 2, 3);
    if (cyclomatic > 10) score += 2;
    if (is_public_api(name)) score += 2;
    return std::min(score, 10);
}

// analyzeChangeImpact (context_lookup_usage.go:150).
ChangeImpactInfo analyze_change_impact(
    const CodeObjectContext& ctx, int fan_in, int fan_out,
    int cyclomatic, MasterIndex& indexer) {
    ChangeImpactInfo impact;
    impact.breaking_change_risk = assess_breaking_change_risk(fan_in, fan_out);
    impact.dependent_components = find_dependent_components(
        ctx.direct_relationships.caller_functions, indexer);
    impact.estimated_impact =
        calculate_impact_score(fan_in, fan_out, cyclomatic, ctx.object_id.name);
    // trap 5: requiresTestsForChange always returns true.
    impact.requires_tests = true;
    return impact;
}

}  // namespace

// Populates ctx.usage_analysis. Mirrors Go's fillUsageAnalysis dispatch,
// reading ctx.object_id (refreshed by fill_basic_info) and
// ctx.direct_relationships (already filled by S3 — see trap 8/hoist note on
// find_dependent_components).
void fill_usage_analysis(CodeObjectContext& ctx, const Snapshot& snap,
                         MasterIndex& indexer) {
    UsageAnalysis& usage = ctx.usage_analysis;

    // trap 10: fan_in and call_frequency are pinned-0 stubs — see file-level
    // comment for the empirical Go-binary trace justifying this.
    usage.call_frequency = 0;
    usage.fan_in = 0;
    usage.fan_out =
        static_cast<int>(ctx.direct_relationships.called_functions.size());

    usage.complexity_metrics = calculate_complexity_metrics(snap, ctx.object_id);
    usage.change_impact =
        analyze_change_impact(ctx, usage.fan_in, usage.fan_out,
                              usage.complexity_metrics.cyclomatic_complexity,
                              indexer);

    // trap 5: test_coverage is always {has_tests:false, test_file_paths:[]}
    // (Go's findTestFiles never appends to its result). Leave default.
}

}  // namespace lci
