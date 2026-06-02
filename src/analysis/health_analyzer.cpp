#include <lci/analysis/health_analyzer.h>

#include <lci/idcodec.h>
#include <lci/reference.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>

namespace lci {

namespace {

std::string to_lower(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

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

bool HealthAnalyzer::is_excluded_file(std::string_view path) const {
    for (const auto& pattern : exclude_patterns_) {
        if (contains(path, pattern)) {
            return true;
        }
    }
    return false;
}

bool HealthAnalyzer::is_test_helper_function(std::string_view name) {
    if (name.empty()) return false;

    std::string lower = to_lower(name);

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

bool HealthAnalyzer::is_test_helper_path(std::string_view path) {
    std::string lower = to_lower(path);

    const std::string_view dirs[] = {
        "testhelpers/", "testhelper/", "testing/", "testutil/", "testutils/",
        "mocks/", "fakes/", "stubs/", "fixtures/", "testdata/"};
    for (auto dir : dirs) {
        if (contains(lower, dir)) return true;
    }

    std::string base = to_lower(std::filesystem::path(std::string(path)).filename().string());
    const std::string_view patterns[] = {
        "_test_helper", "test_helper", "_testutil", "testutil_",
        "_mock", "mock_", "_fake", "fake_", "_stub", "stub_"};
    for (auto pat : patterns) {
        if (contains(base, pat)) return true;
    }

    return false;
}

void HealthAnalyzer::set_exclude_patterns(std::vector<std::string> patterns) {
    exclude_patterns_ = std::move(patterns);
}

// ---------------------------------------------------------------------------
// Complexity metrics
// ---------------------------------------------------------------------------

ComplexityMetrics HealthAnalyzer::calculate_complexity_from_files(
    const std::vector<FileSymbolData>& files) const {

    std::vector<double> complexities;
    absl::flat_hash_map<std::string, int> distribution;
    std::vector<FunctionInfo> high_funcs;

    for (const auto& file : files) {
        for (const auto* sym : file.symbols) {
            if (!is_function_or_method(sym->symbol.type)) continue;

            int cc = sym->complexity;
            if (cc <= 0) cc = 1;
            double c = static_cast<double>(cc);
            complexities.push_back(c);

            if (cc <= ci_thresholds::kComplexityLow) {
                distribution["low"]++;
            } else if (cc <= ci_thresholds::kComplexityHigh) {
                distribution["medium"]++;
            } else {
                distribution["high"]++;
                if (static_cast<int>(high_funcs.size()) < 10 &&
                    !is_excluded_file(file.path) &&
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
    }

    ComplexityMetrics result;
    result.average_cc = avg;
    result.median_cc = median;
    result.percentiles["p50"] = median;
    result.percentiles["p75"] = avg * 1.2;
    result.percentiles["p90"] = avg * 1.5;
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

    std::sort(hotspots.begin(), hotspots.end(),
              [](const Hotspot& a, const Hotspot& b) {
                  return a.risk_score > b.risk_score;
              });

    return hotspots;
}

// ---------------------------------------------------------------------------
// Overall health score
// ---------------------------------------------------------------------------

double HealthAnalyzer::calculate_overall_health_score(
    const ComplexityMetrics& complexity, int /*total_files*/) {

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

    double low_ratio =
        static_cast<double>(low) / static_cast<double>(total_functions);
    if (low_ratio > 0.8) {
        score += 1.0;
    } else if (low_ratio > 0.6) {
        score += 0.5;
    }

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

    for (const auto& file : files) {
        for (const auto* sym : file.symbols) {
            total++;
            if (sym->complexity > ci_thresholds::kComplexityModerate ||
                static_cast<int>(sym->incoming_refs.size()) >
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
            if (sym->complexity > ci_thresholds::kComplexityModerate ||
                static_cast<int>(sym->incoming_refs.size()) >
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
            if (is_test_helper_function(sym->symbol.name)) continue;

            // Long function
            int line_count = sym->symbol.end_line - sym->symbol.line;
            if (line_count > ci_thresholds::kLongFunction) {
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

            // High complexity
            if (sym->complexity > ci_thresholds::kHighComplexity) {
                std::string sev =
                    (sym->complexity > ci_thresholds::kHighComplexityHighSev)
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
            // normalize the two labels.)
            int impact = static_cast<int>(sym->incoming_refs.size());
            if (impact > ci_thresholds::kShotgunSurgery) {
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

    return sort_and_limit_smells(std::move(smells),
                                 ci_thresholds::kMaxDetailedSmells);
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
    std::sort(smells.begin(), smells.end(),
              [](const CodeSmellEntry& a, const CodeSmellEntry& b) {
                  return severity_rank(a.severity) >
                         severity_rank(b.severity);
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
    if (static_cast<int>(sym.incoming_refs.size()) > 15) {
        tags.push_back("HIGH_COUPLING");
        risk += 2;
    }
    if (static_cast<int>(sym.outgoing_refs.size()) > 15) {
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

    std::sort(result.begin(), result.end(),
              [](const ProblematicSymbol& a, const ProblematicSymbol& b) {
                  return a.risk_score > b.risk_score;
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
