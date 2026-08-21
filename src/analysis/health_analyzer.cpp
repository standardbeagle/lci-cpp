#include <lci/analysis/health_analyzer.h>

#include <lci/path_classifier.h>

#include <lci/idcodec.h>
#include <lci/reference.h>
#include <lci/core/text.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>

namespace lci {

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

bool is_function_or_method(SymbolType t) {
    return t == SymbolType::Function || t == SymbolType::Method;
}

bool is_class_or_struct(SymbolType t) {
    return t == SymbolType::Class || t == SymbolType::Struct;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exclusion helpers
// ---------------------------------------------------------------------------

bool HealthAnalyzer::is_test_helper_function(std::string_view name) {
    if (name.empty()) return false;

    std::string lower = text::ascii_lower(name);

    const std::string_view always_helper[] = {
        "setup", "teardown", "helper", "mock", "fake", "stub"};
    for (auto prefix : always_helper) {
        if (starts_with(lower, prefix)) {
            if (prefix == "setup" || prefix == "teardown" ||
                prefix == "helper" || prefix == "mock" ||
                prefix == "fake" || prefix == "stub") {
                return true;
            }
        }
    }

    const std::string_view factory_prefixes[] = {"create", "build", "new", "make"};
    for (auto prefix : factory_prefixes) {
        if (starts_with(lower, prefix)) {
            if (contains(lower, "test") || contains(lower, "mock") ||
                contains(lower, "fake") || contains(lower, "fixture")) {
                return true;
            }
        }
    }

    const std::string_view suffixes[] = {
        "helper", "helpers", "fixture", "fixtures", "mock", "mocks",
        "fake", "fakes", "stub", "stubs", "factory"};
    for (auto suffix : suffixes) {
        if (ends_with(lower, suffix)) return true;
    }

    if (contains(lower, "fortest")) return true;

    return false;
}

// Test helpers do not belong in the "worst complexity" list: a 200-line
// table-driven fixture builder is not technical debt in the product. The
// directory and basename patterns behind this used to live here as a private
// list; they are shipped attribute rules now, so a project's own `.lci.kdl`
// reaches this gate like every other.
bool HealthAnalyzer::is_test_helper_path(std::string_view path) {
    PathClassifier classifier;
    return !classifier.registry().activates(classifier.classify(path),
                                            Capability::Analysis);
}

// ---------------------------------------------------------------------------
// Complexity metrics
// ---------------------------------------------------------------------------

ComplexityMetrics HealthAnalyzer::calculate_complexity_from_files(
    const std::vector<FileSymbolData>& files) const {

    std::vector<double> complexities;
    absl::flat_hash_map<std::string, int> distribution;
    std::vector<FunctionInfo> high_funcs;
    double max_cc = 0.0;

    for (const auto& file : files) {
        for (const auto* sym : file.symbols) {
            if (!is_function_or_method(sym->symbol.type)) continue;

            int cc = sym->complexity;
            if (cc <= 0) cc = 1;
            double c = static_cast<double>(cc);
            complexities.push_back(c);
            if (c > max_cc) max_cc = c;

            if (cc <= ci_thresholds::kComplexityLow) {
                distribution["low"]++;
            } else if (cc <= ci_thresholds::kComplexityHigh) {
                distribution["medium"]++;
            } else {
                distribution["high"]++;
                if (static_cast<int>(high_funcs.size()) < 10 &&
                    !is_test_helper_path(file.path) &&
                    !is_test_helper_function(sym->symbol.name)) {
                    FunctionInfo fi;
                    fi.object_id = encode_symbol_id(sym->id);
                    fi.name = sym->symbol.name;
                    fi.location = file.path + ":" +
                                  std::to_string(sym->symbol.line);
                    fi.complexity = c;
                    high_funcs.push_back(std::move(fi));
                }
            }
        }
    }

    double avg = 0.0;
    double median = 0.0;
    double p75 = 0.0;
    double p90 = 0.0;
    if (!complexities.empty()) {
        for (double v : complexities) avg += v;
        avg /= static_cast<double>(complexities.size());

        std::vector<double> sorted = complexities;
        std::sort(sorted.begin(), sorted.end());
        auto mid = sorted.size() / 2;
        if (sorted.size() % 2 == 0) {
            median = (sorted[mid - 1] + sorted[mid]) / 2.0;
        } else {
            median = sorted[mid];
        }
        // Real order statistics off the vector already sorted for the median.
        // p75/p90 used to be average*1.2 and average*1.5 -- numbers with no
        // relationship to the distribution, which on the usual long tail of
        // complexity understate both badly.
        p75 = sorted[sorted.size() * 3 / 4];
        p90 = sorted[sorted.size() * 9 / 10];
    }

    ComplexityMetrics result;
    result.average_cc = avg;
    result.median_cc = median;
    result.max_cc = max_cc;
    result.percentiles["p50"] = median;
    result.percentiles["p75"] = p75;
    result.percentiles["p90"] = p90;
    result.high_complexity_funcs = std::move(high_funcs);
    result.distribution = std::move(distribution);
    return result;
}

// ---------------------------------------------------------------------------
// Hotspots
// ---------------------------------------------------------------------------

std::vector<Hotspot> HealthAnalyzer::identify_hotspots_from_files(
    const std::vector<FileSymbolData>& files) const {

    std::vector<Hotspot> hotspots;

    for (const auto& file : files) {
        if (is_test_helper_path(file.path)) continue;

        for (const auto* sym : file.symbols) {
            if (is_test_helper_function(sym->symbol.name)) continue;
            if (!is_function_or_method(sym->symbol.type)) continue;

            int cc = sym->complexity;
            if (cc <= 0) cc = 1;

            int line_count = sym->symbol.end_line - sym->symbol.line;
            if (line_count <= 0) line_count = 1;

            if (cc > ci_thresholds::kHotspotComplexity ||
                line_count > ci_thresholds::kHotspotLinecount) {

                double risk = static_cast<double>(cc) * 0.7 +
                              static_cast<double>(line_count) * 0.03;
                if (risk > ci_thresholds::kRiskScoreMax) {
                    risk = ci_thresholds::kRiskScoreMax;
                }

                Hotspot h;
                h.location = file.path + ":" + sym->symbol.name + ":" +
                             std::to_string(sym->symbol.line);
                h.complexity = static_cast<double>(cc);
                h.risk_score = risk;
                hotspots.push_back(std::move(h));
            }
        }
    }

    // std::sort is not stable, so equal risk scores must be separated
    // explicitly or the truncated head varies run to run (Karpathy rule 4).
    std::sort(hotspots.begin(), hotspots.end(),
              [](const Hotspot& a, const Hotspot& b) {
                  if (a.risk_score != b.risk_score) {
                      return a.risk_score > b.risk_score;
                  }
                  return a.location < b.location;
              });

    return hotspots;
}

// ---------------------------------------------------------------------------
// Overall health score
// ---------------------------------------------------------------------------

// D3 de-saturation: the score must discriminate. Every defect signal is a
// deduction; there is no "mostly low complexity" bonus (the old +1.0 bonus
// clamped every real-world repo to 10.00 while the same report showed cc=70
// functions and god objects). 10.0 is reachable only with zero penalties.
double HealthAnalyzer::calculate_overall_health_score(
    const ComplexityMetrics& complexity, double tech_debt_ratio,
    int problematic_symbol_count) {

    double score = 10.0;

    int low = 0, med = 0, high = 0;
    if (auto it = complexity.distribution.find("low");
        it != complexity.distribution.end())
        low = it->second;
    if (auto it = complexity.distribution.find("medium");
        it != complexity.distribution.end())
        med = it->second;
    if (auto it = complexity.distribution.find("high");
        it != complexity.distribution.end())
        high = it->second;

    int total_functions = low + med + high;
    if (total_functions == 0) total_functions = 1;

    double high_ratio =
        static_cast<double>(high) / static_cast<double>(total_functions);
    score -= high_ratio * 4.0;

    double med_ratio =
        static_cast<double>(med) / static_cast<double>(total_functions);
    score -= med_ratio * 1.5;

    if (complexity.average_cc >
        static_cast<double>(ci_thresholds::kComplexityLow)) {
        double deduction =
            (complexity.average_cc -
             static_cast<double>(ci_thresholds::kComplexityLow)) *
            0.15;
        if (deduction > 3.0) deduction = 3.0;
        score -= deduction;
    }

    // Worst-case complexity: a single cc=70 function is a real defect even
    // when thousands of trivial functions dilute the ratios above.
    if (complexity.max_cc > static_cast<double>(ci_thresholds::kComplexityHigh)) {
        double deduction =
            (complexity.max_cc -
             static_cast<double>(ci_thresholds::kComplexityHigh)) *
            0.04;
        if (deduction > 2.5) deduction = 2.5;
        score -= deduction;
    }

    // Technical debt ratio (function population, see
    // calculate_tech_debt_ratio_from_files).
    if (tech_debt_ratio < 0.0) tech_debt_ratio = 0.0;
    double debt_deduction = tech_debt_ratio * 4.0;
    if (debt_deduction > 2.0) debt_deduction = 2.0;
    score -= debt_deduction;

    // Risk symbols above the cutoff.
    if (problematic_symbol_count < 0) problematic_symbol_count = 0;
    double risk_deduction =
        static_cast<double>(problematic_symbol_count) * 0.2;
    if (risk_deduction > 1.5) risk_deduction = 1.5;
    score -= risk_deduction;

    if (score < 0.0) score = 0.0;
    if (score > ci_thresholds::kRiskScoreMax)
        score = ci_thresholds::kRiskScoreMax;

    return score;
}

// ---------------------------------------------------------------------------
// Technical debt
// ---------------------------------------------------------------------------

double HealthAnalyzer::calculate_tech_debt_ratio_from_files(
    const std::vector<FileSymbolData>& files) const {

    int total = 0;
    int debt = 0;

    // Debt is a function-level metric: counting every symbol (variables,
    // fields, imports) in the denominator diluted a cc=70 repo to
    // debt=0.00 (D3).
    for (const auto& file : files) {
        for (const auto* sym : file.symbols) {
            if (!is_function_or_method(sym->symbol.type)) continue;
            total++;
            if (sym->complexity > ci_thresholds::kComplexityModerate ||
                static_cast<int>(sym->incoming_ref_count) >
                    ci_thresholds::kHighReferenceCount) {
                debt++;
            }
        }
    }

    if (total == 0) return 0.0;
    return static_cast<double>(debt) / static_cast<double>(total);
}

std::string HealthAnalyzer::estimate_debt_remediation_time(double ratio) {
    if (ratio < 0.05) return "1 day";
    if (ratio < 0.10) return "1 week";
    if (ratio < 0.20) return "2 weeks";
    if (ratio < 0.30) return "1 month";
    return "3+ months";
}

std::vector<std::string> HealthAnalyzer::identify_debt_components(
    const std::vector<FileSymbolData>& files) const {

    absl::flat_hash_map<std::string, int> debt_by_file;

    for (const auto& file : files) {
        int count = 0;
        for (const auto* sym : file.symbols) {
            if (!is_function_or_method(sym->symbol.type)) continue;
            if (sym->complexity > ci_thresholds::kComplexityModerate ||
                static_cast<int>(sym->incoming_ref_count) >
                    ci_thresholds::kHighReferenceCount) {
                count++;
            }
        }
        if (count > ci_thresholds::kHighUsage) {
            debt_by_file[file.path] = count;
        }
    }

    struct FileDebt {
        std::string path;
        int count;
    };
    std::vector<FileDebt> debts;
    debts.reserve(debt_by_file.size());
    for (const auto& [path, count] : debt_by_file) {
        debts.push_back({path, count});
    }
    std::sort(debts.begin(), debts.end(),
              [](const FileDebt& a, const FileDebt& b) {
                  return a.count > b.count;
              });

    std::vector<std::string> components;
    for (const auto& fd : debts) {
        if (static_cast<int>(components.size()) >= 5) break;
        components.push_back(fd.path + " (" + std::to_string(fd.count) +
                             " issues)");
    }
    return components;
}

// ---------------------------------------------------------------------------
// Code smells
// ---------------------------------------------------------------------------

std::vector<CodeSmellEntry> HealthAnalyzer::calculate_detailed_code_smells(
    const std::vector<FileSymbolData>& files) const {

    std::vector<CodeSmellEntry> smells;

    for (const auto& file : files) {
        if (is_test_helper_path(file.path)) continue;

        std::string base_path =
            std::filesystem::path(file.path).filename().string();

        for (const auto* sym : file.symbols) {
            // Empty names are extraction gaps — not actionable, fail fast
            // by filtering them out of the report.
            if (sym->symbol.name.empty()) continue;
            if (is_test_helper_function(sym->symbol.name)) continue;

            bool callable = is_function_or_method(sym->symbol.type);

            // Long function — functions/methods only; a trait/class/type
            // declaration spanning many lines is not a long function.
            int line_count = sym->symbol.end_line - sym->symbol.line;
            if (callable && line_count > ci_thresholds::kLongFunction) {
                std::string sev = (line_count > ci_thresholds::kLongFunctionHighSev)
                                      ? "high"
                                      : "medium";
                CodeSmellEntry e;
                e.type = "long-function";
                e.object_id = encode_symbol_id(sym->id);
                e.symbol = sym->symbol.name;
                e.location = base_path + ":" +
                             std::to_string(sym->symbol.line);
                e.severity = sev;
                e.description = std::to_string(line_count) +
                                " lines (recommend < 30)";
                smells.push_back(std::move(e));
            }

            // High complexity — same threshold as the "high" bucket of the
            // complexity distribution, so smell counts and distribution
            // never contradict each other (single source of truth).
            if (callable && sym->complexity > ci_thresholds::kComplexityHigh) {
                std::string sev =
                    (sym->complexity > 2 * ci_thresholds::kComplexityHigh)
                        ? "high"
                        : "medium";
                CodeSmellEntry e;
                e.type = "high-complexity";
                e.object_id = encode_symbol_id(sym->id);
                e.symbol = sym->symbol.name;
                e.location = base_path + ":" +
                             std::to_string(sym->symbol.line);
                e.severity = sev;
                e.description = "CC=" + std::to_string(sym->complexity) +
                                " (recommend < 10)";
                smells.push_back(std::move(e));
            }

            // God class
            if (is_class_or_struct(sym->symbol.type)) {
                int method_count = count_child_methods(file.symbols, *sym);
                if (method_count > ci_thresholds::kGodClass) {
                    std::string sev =
                        (method_count > ci_thresholds::kGodClassHighSev)
                            ? "high"
                            : "medium";
                    CodeSmellEntry e;
                    e.type = "god-class";
                    e.object_id = encode_symbol_id(sym->id);
                    e.symbol = sym->symbol.name;
                    e.location = base_path + ":" +
                                 std::to_string(sym->symbol.line);
                    e.severity = sev;
                    e.description = std::to_string(method_count) +
                                    " methods (consider splitting)";
                    smells.push_back(std::move(e));
                }
            }

            // High fan-in: many incoming references. (Go labels this
            // "shotgun-surgery", but the metric is incoming-ref count — the
            // opposite of shotgun surgery, which is one-change-many-edits.
            // The C++ port uses the accurate name; parity descriptors
            // normalize the two labels.) Fields, variables, and type
            // declarations are excluded: heavy reference of data/type
            // declarations is normal, not a smell.
            int impact = static_cast<int>(sym->incoming_ref_count);
            if ((callable || is_class_or_struct(sym->symbol.type)) &&
                impact > ci_thresholds::kShotgunSurgery) {
                std::string sev =
                    (impact > ci_thresholds::kShotgunSurgeryHighSev)
                        ? "high"
                        : "medium";
                CodeSmellEntry e;
                e.type = "high-fan-in";
                e.object_id = encode_symbol_id(sym->id);
                e.symbol = sym->symbol.name;
                e.location = base_path + ":" +
                             std::to_string(sym->symbol.line);
                e.severity = sev;
                e.description = std::to_string(impact) +
                                " incoming references";
                smells.push_back(std::move(e));
            }
        }
    }

    // Full sorted set — callers truncate for display. Counting a truncated
    // list produced the "smells: high-complexity=1 vs distribution: high=3"
    // contradiction (D3).
    const int all = static_cast<int>(smells.size());
    return sort_and_limit_smells(std::move(smells), all);
}

int HealthAnalyzer::count_child_methods(
    const std::vector<const EnhancedSymbol*>& symbols,
    const EnhancedSymbol& parent) {
    int count = 0;
    for (const auto* sym : symbols) {
        if (sym->symbol.type == SymbolType::Method &&
            sym->symbol.line > parent.symbol.line &&
            sym->symbol.end_line <= parent.symbol.end_line) {
            count++;
        }
    }
    return count;
}

int HealthAnalyzer::severity_rank(std::string_view sev) {
    if (sev == "high") return 2;
    if (sev == "medium") return 1;
    return 0;
}

std::vector<CodeSmellEntry> HealthAnalyzer::sort_and_limit_smells(
    std::vector<CodeSmellEntry> smells, int max_count) {
    // Severity alone leaves most entries tied, and this list is truncated to
    // max_count -- without a total order WHICH smells survive varies run to
    // run (Karpathy rule 4).
    std::sort(smells.begin(), smells.end(),
              [](const CodeSmellEntry& a, const CodeSmellEntry& b) {
                  int ra = severity_rank(a.severity);
                  int rb = severity_rank(b.severity);
                  if (ra != rb) return ra > rb;
                  if (a.type != b.type) return a.type < b.type;
                  if (a.location != b.location) return a.location < b.location;
                  return a.symbol < b.symbol;
              });
    if (static_cast<int>(smells.size()) > max_count) {
        smells.resize(static_cast<size_t>(max_count));
    }
    return smells;
}

// ---------------------------------------------------------------------------
// Problematic symbols
// ---------------------------------------------------------------------------

std::pair<std::vector<std::string>, int>
HealthAnalyzer::calculate_symbol_risk_and_tags(const EnhancedSymbol& sym) {
    std::vector<std::string> tags;
    int risk = 0;

    if (sym.complexity > 15) {
        tags.push_back("HIGH_COMPLEXITY");
        risk += 3;
    }
    int line_count = sym.symbol.end_line - sym.symbol.line;
    if (line_count > 100) {
        tags.push_back("LARGE_FUNCTION");
        risk += 2;
    }
    if (static_cast<int>(sym.incoming_ref_count) > 15) {
        tags.push_back("HIGH_COUPLING");
        risk += 2;
    }
    if (static_cast<int>(sym.outgoing_ref_count) > 15) {
        tags.push_back("MANY_DEPENDENCIES");
        risk += 2;
    }
    if (risk > 10) risk = 10;

    return {tags, risk};
}

std::vector<ProblematicSymbol> HealthAnalyzer::identify_problematic_symbols(
    const std::vector<FileSymbolData>& files) const {

    std::vector<ProblematicSymbol> result;

    for (const auto& file : files) {
        if (is_test_helper_path(file.path)) continue;

        std::string base_path =
            std::filesystem::path(file.path).filename().string();

        for (const auto* sym : file.symbols) {
            // Empty names are extraction gaps — filter, don't report.
            if (sym->symbol.name.empty()) continue;
            if (is_test_helper_function(sym->symbol.name)) continue;

            auto [tags, risk] = calculate_symbol_risk_and_tags(*sym);
            if (risk >= ci_thresholds::kRiskScoreCutoff) {
                ProblematicSymbol ps;
                ps.object_id = encode_symbol_id(sym->id);
                ps.name = sym->symbol.name;
                ps.location = base_path + ":" +
                              std::to_string(sym->symbol.line);
                ps.risk_score = risk;
                ps.tags = std::move(tags);
                result.push_back(std::move(ps));
            }
        }
    }

    // Total order before the truncation below (Karpathy rule 4): risk_score
    // is a small integer, so ties are the common case.
    std::sort(result.begin(), result.end(),
              [](const ProblematicSymbol& a, const ProblematicSymbol& b) {
                  if (a.risk_score != b.risk_score) {
                      return a.risk_score > b.risk_score;
                  }
                  if (a.location != b.location) return a.location < b.location;
                  return a.name < b.name;
              });

    if (static_cast<int>(result.size()) > ci_thresholds::kMaxProblematicSymbols) {
        result.resize(static_cast<size_t>(ci_thresholds::kMaxProblematicSymbols));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Quality helpers
// ---------------------------------------------------------------------------

QualityMetrics HealthAnalyzer::calculate_quality_from_complexity(
    const ComplexityMetrics& complexity) {
    double mi = ci_thresholds::kMaintainabilityMax - (complexity.average_cc * 2.0);
    if (mi < ci_thresholds::kMaintainabilityMin)
        mi = ci_thresholds::kMaintainabilityMin;
    if (mi > ci_thresholds::kMaintainabilityMax)
        mi = ci_thresholds::kMaintainabilityMax;

    int low = 0, med = 0, high = 0;
    if (auto it = complexity.distribution.find("low");
        it != complexity.distribution.end())
        low = it->second;
    if (auto it = complexity.distribution.find("medium");
        it != complexity.distribution.end())
        med = it->second;
    if (auto it = complexity.distribution.find("high");
        it != complexity.distribution.end())
        high = it->second;

    int total = low + med + high;
    double debt = 0.0;
    if (total > 0) {
        debt = static_cast<double>(high) / static_cast<double>(total);
    }

    QualityMetrics qm;
    qm.maintainability_index = mi;
    qm.technical_debt_ratio = debt;
    return qm;
}

std::string HealthAnalyzer::get_maintainability_rating(double score) {
    if (score >= 80.0) return "A";
    if (score >= 70.0) return "B";
    if (score >= 60.0) return "C";
    if (score >= 50.0) return "D";
    return "F";
}

absl::flat_hash_map<std::string, int> HealthAnalyzer::count_smells_by_type(
    const std::vector<CodeSmellEntry>& smells) {
    absl::flat_hash_map<std::string, int> counts;
    for (const auto& s : smells) {
        counts[s.type]++;
    }
    return counts;
}

}  // namespace lci
