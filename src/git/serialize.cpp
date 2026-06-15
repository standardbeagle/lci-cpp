#include <lci/git/analyzer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <string>

#include <nlohmann/json.hpp>

namespace lci {
namespace git {

namespace {

/// Returns `p` rewritten relative to `project_root` when `p` is absolute and
/// lives under that root, or `p` (forward-slash normalized) unchanged when it
/// is already relative or sits outside the root.
///
/// Implemented as a purely lexical, separator-independent string operation —
/// NOT std::filesystem::relative — so the emitted JSON path is deterministic
/// and identical on every platform. std::filesystem treats a '/'-rooted path
/// as non-absolute on Windows (it carries no drive letter), which left such
/// paths un-stripped and produced absolute JSON paths on the Windows leg.
std::string normalize_rel(const std::string& p,
                          const std::string& project_root) {
    if (p.empty()) return p;

    std::string path = p;
    std::replace(path.begin(), path.end(), '\\', '/');

    // "Rooted" = POSIX '/'-rooted or a Windows drive specifier ("C:..."). A
    // relative path passes through (with separators normalized to '/').
    const bool rooted =
        path.front() == '/' ||
        (path.size() >= 2 &&
         std::isalpha(static_cast<unsigned char>(path.front())) &&
         path[1] == ':');
    if (!rooted) return path;

    std::string root = project_root;
    std::replace(root.begin(), root.end(), '\\', '/');
    while (root.size() > 1 && root.back() == '/') root.pop_back();

    // Strip the root only on a path-segment boundary (so "/tmp/projX" is not
    // treated as living under "/tmp/proj"). Absolute paths outside the root
    // are preserved rather than rewritten into a misleading "../.." chain.
    if (!root.empty() && path.size() > root.size() &&
        path.compare(0, root.size(), root) == 0 &&
        path[root.size()] == '/') {
        return path.substr(root.size() + 1);
    }
    return path;
}

nlohmann::json symbol_to_json(const SymbolInfo& symbol,
                              const std::string& project_root) {
    nlohmann::json out;
    out["name"] = symbol.name;
    out["type"] = symbol.type;
    out["file_path"] = normalize_rel(symbol.file_path, project_root);
    out["line"] = symbol.line;
    if (symbol.end_line > 0) out["end_line"] = symbol.end_line;
    if (symbol.complexity > 0) out["complexity"] = symbol.complexity;
    if (symbol.lines_of_code > 0) out["lines_of_code"] = symbol.lines_of_code;
    if (symbol.nesting_depth > 0) out["nesting_depth"] = symbol.nesting_depth;
    return out;
}

nlohmann::json location_to_json(const CodeLocation& loc,
                                const std::string& project_root) {
    nlohmann::json out;
    out["file_path"] = normalize_rel(loc.file_path, project_root);
    out["start_line"] = loc.start_line;
    out["end_line"] = loc.end_line;
    out["symbol_name"] = loc.symbol_name;
    return out;
}

nlohmann::json metrics_to_json(const MetricsFinding& finding,
                               const std::string& project_root) {
    nlohmann::json out;
    out["severity"] = std::string(to_string(finding.severity));
    out["description"] = finding.description;
    out["symbol"] = symbol_to_json(finding.symbol, project_root);
    out["issue_type"] = std::string(to_string(finding.issue_type));
    out["issue"] = finding.issue;
    out["suggestion"] = finding.suggestion;
    if (finding.new_metrics) {
        out["new_metrics"] = {
            {"complexity", finding.new_metrics->complexity},
            {"lines_of_code", finding.new_metrics->lines_of_code},
            {"nesting_depth", finding.new_metrics->nesting_depth},
        };
    } else {
        out["new_metrics"] = {
            {"complexity", finding.symbol.complexity},
            {"lines_of_code", finding.symbol.lines_of_code},
            {"nesting_depth", finding.symbol.nesting_depth},
        };
    }
    return out;
}

nlohmann::json naming_to_json(const NamingFinding& finding,
                              const std::string& project_root) {
    nlohmann::json out;
    out["severity"] = std::string(to_string(finding.severity));
    out["description"] = finding.description;
    out["new_symbol"] = symbol_to_json(finding.new_symbol, project_root);
    out["issue_type"] = std::string(to_string(finding.issue_type));
    out["issue"] = finding.issue;
    out["suggestion"] = finding.suggestion;
    if (finding.similar_names.empty()) {
        out["similar_names"] = nullptr;
    } else {
        out["similar_names"] = nlohmann::json::array();
        for (const auto& sym : finding.similar_names) {
            out["similar_names"].push_back(symbol_to_json(sym, project_root));
        }
    }
    return out;
}

nlohmann::json duplicate_to_json(const DuplicateFinding& finding,
                                 const std::string& project_root) {
    nlohmann::json out;
    out["severity"] = std::string(to_string(finding.severity));
    out["description"] = finding.description;
    out["new_code"] = location_to_json(finding.new_code, project_root);
    out["existing_code"] = location_to_json(finding.existing_code, project_root);
    out["similarity"] = finding.similarity;
    out["type"] = finding.type;
    out["suggestion"] = finding.suggestion;
    return out;
}

std::string format_analyzed_at(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

}  // namespace

nlohmann::json report_to_json(const AnalysisReport& report,
                              const std::string& project_root) {
    nlohmann::json report_j;
    report_j["summary"] = {
        {"files_changed", report.summary.files_changed},
        {"symbols_added", report.summary.symbols_added},
        {"symbols_modified", report.summary.symbols_modified},
        {"symbols_deleted", report.summary.symbols_deleted},
        {"duplicates_found", report.summary.duplicates_found},
        {"naming_issues_found", report.summary.naming_issues_found},
        {"metrics_issues_found", report.summary.metrics_issues_found},
        {"risk_score", report.summary.risk_score},
        {"top_recommendation", report.summary.top_recommendation},
    };

    if (!report.metrics_issues.empty()) {
        report_j["metrics_issues"] = nlohmann::json::array();
        for (const auto& finding : report.metrics_issues) {
            report_j["metrics_issues"].push_back(
                metrics_to_json(finding, project_root));
        }
    }

    if (!report.naming_issues.empty()) {
        report_j["naming_issues"] = nlohmann::json::array();
        for (const auto& finding : report.naming_issues) {
            report_j["naming_issues"].push_back(
                naming_to_json(finding, project_root));
        }
    }

    if (!report.duplicates.empty()) {
        report_j["duplicates"] = nlohmann::json::array();
        for (const auto& finding : report.duplicates) {
            report_j["duplicates"].push_back(
                duplicate_to_json(finding, project_root));
        }
    }

    report_j["metadata"] = {
        {"base_ref", report.metadata.base_ref},
        {"target_ref", report.metadata.target_ref},
        {"scope", std::string(to_string(report.metadata.scope))},
        {"analysis_time_ms", report.metadata.analysis_time_ms},
        {"analyzed_at", format_analyzed_at(report.metadata.analyzed_at)},
    };

    return report_j;
}

}  // namespace git
}  // namespace lci
