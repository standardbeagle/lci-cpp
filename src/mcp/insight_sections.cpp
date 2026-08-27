#include <lci/mcp/insight_sections.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include <lci/analysis/coupling_analyzer.h>
#include <lci/core/graph_propagator.h>
#include <lci/core/reference_tracker.h>
#include <lci/git/analyzer.h>
#include <lci/git/provider.h>
#include <lci/indexing/master_index.h>
#include <lci/language_map.h>
#include <lci/search/search_engine.h>
#include <lci/symbol.h>
#include <lci/version.h>

#include <absl/container/flat_hash_set.h>

namespace lci {
namespace mcp {
namespace insight {

std::string fmt2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}
std::string fmt1(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return std::string(buf);
}

// LCF token estimate for the header. Mirrors Go's estimateLCFTokenCount:
//   modules*20 + dep_edges*15 + (health?50) + entry*15 + (stats?50) + 20.
// `n_modules` is the post-truncation count (<=15) the repo map actually
// emits, matching Go (the formatter runs after budget truncation).
int lcf_token_count(int n_modules, int n_dep_edges, bool has_health,
                    int n_entry, bool has_stats) {
    int est = n_modules * 20 + n_dep_edges * 15 + n_entry * 15 + 20;
    if (has_health) est += 50;
    if (has_stats) est += 50;
    return est;
}

void emit_lcf_header(std::ostringstream& out, std::string_view mode, int tier,
                     int tokens, std::string_view scope) {
    out << "LCF/1.0\nmode=" << mode << "\ntier=" << tier << "\ntokens="
        << tokens;
    // Which attributes the numbers below cover. Always emitted for the
    // analysis modes: a score whose corpus is unstated invites the reader to
    // assume it covered everything, which is exactly the mistake that put a
    // benchmark harness into this repo's error-handling score.
    if (!scope.empty()) out << "\nattributes=" << scope;
    out << "\n---\n";
}

// Forwarder: the analysis modes all pass a scope, the git modes never do.

// == REPOSITORY MAP == — one line per module, capped to 15 (Go truncates to
// 15 during budget enforcement). Emitted only when non-empty (Go: nil skip).
void emit_repository_map(std::ostringstream& out,
                         const std::vector<ModuleBoundary>& mods) {
    if (mods.empty()) return;
    out << "== REPOSITORY MAP ==\n";
    size_t lim = std::min(mods.size(), size_t{15});
    for (size_t i = 0; i < lim; ++i) {
        out << "module=" << mods[i].name << " files=" << mods[i].file_count
            << "\n";
    }
    out << "---\n";
}

// == HEALTH == — score, complexity, smell summary + detail, problematic
// symbols, purity. Object IDs ([o=XX]) come from analyzer-populated fields.
void emit_health(std::ostringstream& out, const HealthDashboard& hd,
                 const PuritySummary* purity) {
    // Unit-labeled: HEALTH is 0-10 while STATISTICS maintainability is
    // 0-100; unlabeled they read as one inconsistent scale.
    out << "== HEALTH ==\n"
        << "score=" << fmt2(hd.overall_score) << "/10\n"
        << "complexity=" << fmt2(hd.complexity.average_cc) << "\n";

    // Massive files lead the detail: the score's dominant deduction. Each
    // listed file exceeds what an agent can read whole.
    if (!hd.massive_files.empty()) {
        out << "massive_files (>=" << ci_thresholds::kMassiveFileLines
            << " lines, unreadable-whole for an agent): "
            << hd.massive_files.size() << "\n";
        size_t lim = std::min(hd.massive_files.size(),
                              size_t{ci_thresholds::kMaxMassiveFiles});
        for (size_t i = 0; i < lim; ++i) {
            out << "  " << hd.massive_files[i].path << ": "
                << hd.massive_files[i].lines << " lines\n";
        }
        if (hd.massive_files.size() > lim) {
            out << "  ... and " << (hd.massive_files.size() - lim)
                << " more\n";
        }
    }

    if (!hd.smell_counts.empty()) {
        // Go iterates the smell-count map (non-deterministic order). The C++
        // port sorts by smell type for stable output.
        std::vector<std::pair<std::string, int>> sc(hd.smell_counts.begin(),
                                                     hd.smell_counts.end());
        std::sort(sc.begin(), sc.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        out << "smells:";
        for (const auto& [type, count] : sc) out << " " << type << "=" << count;
        out << "\n";
    }
    if (!hd.detailed_smells.empty()) {
        out << "detailed_smells:\n";
        for (const auto& s : hd.detailed_smells) {
            out << "  [" << s.severity << "] " << s.type << ": " << s.symbol
                << " (" << s.location << ") [o=" << s.object_id << "]\n";
        }
    }
    if (!hd.problematic_symbols.empty()) {
        out << "problematic_symbols:\n";
        for (const auto& ps : hd.problematic_symbols) {
            out << "  " << ps.name << " (" << ps.location << ")"
                << " risk=" << ps.risk_score;
            if (!ps.tags.empty()) {
                out << " [";
                for (size_t i = 0; i < ps.tags.size(); ++i) {
                    if (i) out << ",";
                    out << ps.tags[i];
                }
                out << "]";
            }
            out << " [o=" << ps.object_id << "]\n";
        }
    }
    if (purity) {
        double ratio = purity->total_functions > 0
            ? static_cast<double>(purity->pure_functions) /
                  purity->total_functions
            : 0.0;
        out << "purity:\n"
            << "  total=" << purity->total_functions
            << " pure=" << purity->pure_functions
            << " impure=" << purity->impure_functions
            << " ratio=" << fmt2(ratio) << "\n";
        if (purity->with_io_effects > 0 || purity->with_global_writes > 0 ||
            purity->with_param_writes > 0) {
            out << "  effects: io=" << purity->with_io_effects
                << " global_writes=" << purity->with_global_writes
                << " param_writes=" << purity->with_param_writes
                << " throws=" << purity->with_throws << "\n";
        }
        out << "  query: side_effects {\"mode\": \"impure\", "
               "\"include_reasons\": true}\n";
    }
    out << "---\n";
}

// == MODULES == — aggregate cohesion/coupling + top-10 modules by file count.
// D4: this section's cohesion is ModuleAnalyzer's NAME-PREFIX similarity, a
// different metric from the reference-graph cohesion STATISTICS reports
// (guzzle measured 0.12 vs 0.34 under the same label). Emit it as
// name_cohesion so the two aggregations are never read as one number.
void emit_modules(std::ostringstream& out, const ModuleAnalysis& ma) {
    if (ma.modules.empty()) return;
    // basis=symbol_files: module file counts cover symbol-bearing files only
    // (the analyzer's input), so they can sit below STRUCTURE's full file
    // census (pocketbase core: 123 vs 134). Declared, not silent.
    out << "== MODULES ==\n"
        << "total=" << ma.metrics.total_modules
        << " name_cohesion=" << fmt2(ma.metrics.average_cohesion)
        << " coupling=" << fmt2(ma.metrics.average_coupling)
        << " basis=symbol_files\n";
    size_t lim = std::min(ma.modules.size(), size_t{10});
    for (size_t i = 0; i < lim; ++i) {
        const auto& m = ma.modules[i];
        out << "  " << m.name << ": type=" << m.type
            << " files=" << m.file_count << " funcs=" << m.function_count
            << " name_cohesion=" << fmt2(m.cohesion_score) << "\n";
    }
    if (ma.modules.size() > 10) {
        out << "  ... and " << (ma.modules.size() - 10) << " more modules\n";
    }
    out << "---\n";
}

// == STATISTICS == — complexity, coupling, cohesion, quality, plus the top-3
// high-complexity functions and low-cohesion modules.
void emit_statistics(std::ostringstream& out, const ComplexityMetrics& cm,
                     const CouplingMetrics& cp, const CohesionMetrics& ch,
                     const QualityMetrics& q, double purity_ratio) {
    out << "== STATISTICS ==\n";
    out << "complexity: avg=" << fmt2(cm.average_cc)
        << " median=" << fmt2(cm.median_cc) << "\n";
    if (!cm.distribution.empty()) {
        // Go iterates the distribution map (non-deterministic). C++ emits a
        // fixed low/medium/high order. The population is FUNCTIONS with
        // complexity records, not SUMMARY's all-kinds symbol count — say so
        // (every audit round tripped on the unlabeled denominator).
        int fn_total = 0;
        for (const auto& [k, v] : cm.distribution) fn_total += v;
        out << "  distribution: functions=" << fn_total;
        for (const char* k : {"low", "medium", "high"}) {
            auto it = cm.distribution.find(k);
            if (it != cm.distribution.end())
                out << " " << k << "=" << it->second;
        }
        out << "\n";
    }
    out << "coupling: avg=" << fmt2(cp.average_coupling)
        << " max=" << fmt2(cp.max_coupling) << "\n";
    out << "cohesion: avg=" << fmt2(ch.average_cohesion)
        << " min=" << fmt2(ch.min_cohesion) << "\n";
    out << "quality: maintainability=" << fmt2(q.maintainability_index)
        << "/100 debt=" << fmt2(q.technical_debt_ratio)
        << " purity=" << fmt2(purity_ratio) << "\n";
    if (!cm.high_complexity_funcs.empty()) {
        out << "  high_complexity:\n";
        size_t lim = std::min(cm.high_complexity_funcs.size(), size_t{3});
        for (size_t i = 0; i < lim; ++i) {
            const auto& fn = cm.high_complexity_funcs[i];
            out << "    " << fn.name << " (" << fn.location << ") cc="
                << fmt1(fn.complexity) << "\n";
        }
    }
    if (!ch.low_cohesion_modules.empty()) {
        size_t lim = std::min(ch.low_cohesion_modules.size(), size_t{3});
        out << "  low_cohesion: ";
        for (size_t i = 0; i < lim; ++i) {
            if (i) out << ", ";
            out << ch.low_cohesion_modules[i];
        }
        out << "\n";
    }
    out << "---\n";
}

// Rewrites an absolute path to project-root-relative for compact, stable
// output (mirrors git::report_to_json's normalization).
std::string git_rel(std::string_view path, std::string_view root) {
    if (!root.empty() && path.rfind(root, 0) == 0) {
        path.remove_prefix(root.size());
        while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    }
    return std::string(path);
}

// == GIT CHANGES == — real git change-analysis (duplicates / naming / metrics)
// for the working set. C++ enrichment: Go computes this in git_analyze mode but
// its LCF formatter discards the git fields and prints an all-zero STATISTICS
// block, so this data is unreachable in Go's text output. Sourced from
// git::Analyzer (the same engine behind the git_analysis tool + HTTP
// /git-analyze).
void emit_git_changes(std::ostringstream& out, const git::AnalysisReport& r,
                      std::string_view root) {
    const auto& s = r.summary;
    out << "== GIT CHANGES ==\n";
    out << "scope=" << to_string(r.metadata.scope)
        << " files_changed=" << s.files_changed
        << " added=" << s.symbols_added << " modified=" << s.symbols_modified
        << " deleted=" << s.symbols_deleted << " risk=" << fmt2(s.risk_score)
        << "\n";
    out << "findings: duplicates=" << s.duplicates_found
        << " naming=" << s.naming_issues_found
        << " metrics=" << s.metrics_issues_found << "\n";
    if (!s.top_recommendation.empty())
        out << "top: " << s.top_recommendation << "\n";

    // Top metrics issues, sorted by (file, line) for deterministic output.
    if (!r.metrics_issues.empty()) {
        std::vector<const git::MetricsFinding*> mi;
        mi.reserve(r.metrics_issues.size());
        for (const auto& m : r.metrics_issues) mi.push_back(&m);
        std::sort(mi.begin(), mi.end(), [](const auto* a, const auto* b) {
            if (a->symbol.file_path != b->symbol.file_path)
                return a->symbol.file_path < b->symbol.file_path;
            return a->symbol.line < b->symbol.line;
        });
        out << "metrics_issues:\n";
        size_t lim = std::min(mi.size(), size_t{5});
        for (size_t i = 0; i < lim; ++i) {
            const auto& m = *mi[i];
            out << "  " << m.symbol.name << " ("
                << git_rel(m.symbol.file_path, root) << ":" << m.symbol.line
                << ") " << to_string(m.issue_type)
                << " loc=" << m.symbol.lines_of_code
                << " cc=" << m.symbol.complexity << "\n";
        }
        if (mi.size() > lim)
            out << "  ... and " << (mi.size() - lim) << " more\n";
    }
    out << "---\n";
}

// == GIT HOTSPOTS == — change-frequency / churn analysis: the files that change
// most, multi-author collision zones, and module ownership. C++ enrichment:
// like git_analyze, Go computes this in git_hotspots mode but discards it in the
// LCF formatter. Sourced from git::FrequencyAnalyzer. NOTE: output reflects a
// rolling time window over live git history, so values are environment- and
// time-dependent (not byte-stable across runs) — parity for this mode is
// envelope-only by design.
void emit_git_hotspots(std::ostringstream& out,
                       const git::ChangeFrequencyReport& r,
                       git::TimeWindow window, std::string_view root) {
    const auto& s = r.summary;
    out << "== GIT HOTSPOTS ==\n";
    out << "window=" << to_string(window)
        << " files_analyzed=" << s.total_files_analyzed
        << " commits=" << s.total_commits_analyzed
        << " hotspots=" << s.hotspots_found
        << " anti_patterns=" << s.anti_patterns_found << "\n";

    if (!r.hotspots.empty()) {
        out << "hotspots:\n";
        size_t lim = std::min(r.hotspots.size(), size_t{8});
        for (size_t i = 0; i < lim; ++i) {
            const auto& h = r.hotspots[i];
            const auto it = h.metrics.find(window);
            int changes = it != h.metrics.end() ? it->second.change_count : 0;
            int added = it != h.metrics.end() ? it->second.lines_added : 0;
            int deleted = it != h.metrics.end() ? it->second.lines_deleted : 0;
            out << "  " << git_rel(h.file_path, root) << " changes=" << changes
                << " authors=" << h.contributors.size() << " churn=+" << added
                << "/-" << deleted << "\n";
        }
    }
    if (!r.collisions.empty()) {
        out << "collisions:\n";
        size_t lim = std::min(r.collisions.size(), size_t{5});
        for (size_t i = 0; i < lim; ++i) {
            const auto& c = r.collisions[i];
            out << "  " << git_rel(c.path, root)
                << " score=" << fmt2(c.collision_score)
                << " contributors=" << c.contributors.size()
                << " severity=" << to_string(c.severity) << "\n";
        }
    }
    out << "---\n";
}

// Shared finding-line renderer for the two sections below:
//   [sev] signal: symbol (file:line) detail [o=id]
void emit_eh_finding_lines(std::ostringstream& out,
                           const std::vector<EhFindingEntry>& findings,
                           size_t limit) {
    size_t lim = std::min(findings.size(), limit);
    for (size_t i = 0; i < lim; ++i) {
        const auto& f = findings[i];
        out << "  [" << f.severity << "] " << f.signal << ": " << f.symbol
            << " (" << f.location << ")";
        if (!f.detail.empty()) out << " " << f.detail;
        if (!f.object_id.empty()) out << " [o=" << f.object_id << "]";
        out << "\n";
    }
    if (findings.size() > lim) {
        out << "  ... and " << (findings.size() - lim) << " more\n";
    }
}

// == ERROR HANDLING == — repo/module error-handling scores + swallowed-error
// findings (spec: docs/plans/2026-08-17-error-handling-score-design.md).
// Production-only, deterministically sorted; locations always file:line.
void emit_error_handling(std::ostringstream& out,
                         const ErrorHandlingSummary& s, size_t max_findings) {
    out << "== ERROR HANDLING ==\n";
    out << "score=" << fmt2(s.score);
    if (!s.module_scores.empty()) {
        const auto& worst = s.module_scores.front();
        const auto& best = s.module_scores.back();
        out << " modules: worst=" << worst.first << "(" << fmt1(worst.second)
            << ") best=" << best.first << "(" << fmt1(best.second) << ")";
    }
    out << "\n";
    out << "throwers=" << s.throwers
        << " handled_ratio=" << fmt2(s.handled_ratio)
        << " swallow_sites=" << s.swallow_sites
        << " unchecked_errors=" << s.unchecked_errors;
    // A silenced report must not look like a clean one.
    if (s.suppressed > 0) out << " suppressed=" << s.suppressed;
    out << "\n";
    // Undo cost — what an error would leave half-done. Emitted only when
    // there is some, so a codebase that changes no state keeps the historical
    // two-line shape.
    if (s.uncompensated > 0 || s.irreversible_first > 0) {
        out << "undo_cost: uncompensated=" << s.uncompensated
            << " irreversible_first=" << s.irreversible_first << "\n";
    }
    if (!s.findings.empty()) {
        out << "findings:\n";
        emit_eh_finding_lines(out, s.findings, max_findings);
        // Density de-saturates the headline: 9.99 with density=0.4 and 9.99
        // with density=12.0 are different codebases even though the weighted
        // mean can no longer tell them apart.
        if (s.functions_scored > 0) {
            out << "density: findings=" << s.findings.size() << " per_100_funcs="
                << fmt2(100.0 * static_cast<double>(s.findings.size()) /
                        s.functions_scored)
                << "\n";
        }
    }
    if (!s.exposure.empty()) {
        out << "exposure:\n";
        for (const auto& e : s.exposure) {
            if (e.kind == "cause-loss") {
                // The funnel: the error surfaces from this API, but renamed
                // and chainless — the sink threw a new error without the
                // cause. Incident triage: an error reported at api_symbol
                // may have originated as ANY failure below sink_symbol.
                out << "  api-reaches-cause-loss: " << e.api_symbol << " ("
                    << e.api_location << ") -> " << e.sink_symbol
                    << " rethrow-no-cause depth=" << e.depth << "\n";
            } else {
                out << "  api-reaches-swallow: " << e.api_symbol << " ("
                    << e.api_location << ") -> " << e.sink_symbol
                    << " swallow depth=" << e.depth;
                // What the production log will hold when the swallow fires.
                if (!e.log.empty()) out << " log=" << e.log;
                out << "\n";
            }
        }
    }
    out << "next: code_insight {\"mode\":\"detailed\",\"analysis\":"
           "\"errors\"}\n";
    out << "---\n";
}

// == RESOURCE MANAGEMENT == — acquire/release pairing score + potential-leak
// findings. Same production/determinism rules as ERROR HANDLING.
void emit_resource_management(std::ostringstream& out,
                              const ResourceSummary& s, size_t max_findings) {
    out << "== RESOURCE MANAGEMENT ==\n";
    out << "score=" << fmt2(s.score) << " acquisitions=" << s.acquisitions
        << " released_ratio=" << fmt2(s.released_ratio)
        << " guarded_ratio=" << fmt2(s.guarded_ratio) << "\n";
    if (!s.findings.empty()) {
        out << "findings:\n";
        emit_eh_finding_lines(out, s.findings, max_findings);
    }
    out << "next: code_insight {\"mode\":\"detailed\",\"analysis\":"
           "\"resources\"}\n";
    out << "---\n";
}

// A symbol ranked by how much of the codebase transitively depends on it.
void emit_vocabulary(std::ostringstream& out, const NamingReport& nr) {
    if (nr.outliers.empty() && nr.aliases_in_use.empty() &&
        nr.ambiguous_names.empty())
        return;
    out << "== VOCABULARY ==\n";
    if (nr.vagueness.total_tokens > 0) {
        out << "vagueness=" << fmt2(nr.vagueness.score)
            << " (non-word name tokens: " << nr.vagueness.nonword_tokens
            << "/" << nr.vagueness.total_tokens << ", symbols affected: "
            << nr.vagueness.symbols_with_nonwords << "/"
            << nr.vagueness.total_symbols << ")\n";
        if (!nr.vagueness.top_nonwords.empty()) {
            out << "  top_nonwords:";
            for (const auto& [t, n] : nr.vagueness.top_nonwords)
                out << " " << t << "(" << n << ")";
            out << "\n";
        }
    }
    if (!nr.ambiguous_names.empty()) {
        out << "ambiguous_names (same name, many definitions — a search"
               " returns them all):\n ";
        for (const auto& a : nr.ambiguous_names) {
            out << " " << a.name << "(" << a.definition_count << ")";
        }
        out << "\n";
    }
    out << "outliers=" << nr.outliers.size() << "\n";
    for (const auto& o : nr.outliers) {
        out << "  " << o.name << " (" << o.location << ") fan-in=" << o.fan_in
            << " " << o.reason << "=" << o.odd_term;
        if (!o.suggested.empty()) {
            out << " -> ";
            for (size_t i = 0; i < o.suggested.size(); ++i) {
                if (i) out << ",";
                out << o.suggested[i];
            }
        }
        out << " [o=" << o.object_id << "]\n";
    }
    if (!nr.aliases_in_use.empty()) {
        out << "aliases_in_use:\n";
        for (const auto& a : nr.aliases_in_use) {
            out << "  " << a.canonical << ":";
            for (const auto& [member, n] : a.terms) {
                out << " " << member << "(" << n << ")";
            }
            out << "\n";
        }
    }
    out << "---\n";
}

// == SUMMARY == — one-look orientation: size + language mix. C++-only
// session-startup section (no Go counterpart). lang counts by file extension.
void emit_summary(std::ostringstream& out,
                  const std::vector<FileSymbolData>& files,
                  const std::vector<std::string>& file_paths,
                  std::string_view project_root, int file_count,
                  int symbol_count,
                  const std::vector<ExcludedAttr>* excluded,
                  const PathAttrRegistry* registry,
                  const ErrorHandlingAnalyzer::Result* eh) {
    absl::flat_hash_map<std::string, int> lang_files;
    absl::flat_hash_set<std::string> dirs;
    int max_depth = 0;
    // Language name comes from the centralized extension table
    // (language_map.h); files with no canonical language are not counted.
    auto lang_of = [](std::string_view path) -> std::string_view {
        LangId id = language_info_for_path(path).language;
        if (id == LangId::Unknown) return {};
        return to_string(id);
    };
    // D4 census parity: dirs/depth walk the FULL indexed path set (not just
    // symbol-bearing files) and count root + every ancestor directory — the
    // same census build_structure reports, so SUMMARY dirs= and STRUCTURE
    // dirs= agree. The old parent-dir-only count over symbol-bearing files
    // understated (chi: 18 vs 21).
    for (const auto& path : file_paths) {
        if (path.empty()) continue;
        std::string rel = path;
        if (!project_root.empty() && rel.rfind(project_root, 0) == 0) {
            rel = rel.substr(project_root.size());
            while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
        }
        int depth = 0;
        for (char c : rel) if (c == '/') ++depth;
        if (depth > max_depth) max_depth = depth;
        dirs.insert(".");
        for (size_t pos = rel.find('/'); pos != std::string::npos;
             pos = rel.find('/', pos + 1)) {
            dirs.insert(rel.substr(0, pos));
        }
    }
    for (const auto& f : files) {
        if (std::string_view l = lang_of(f.path); !l.empty())
            lang_files[std::string(l)]++;
    }
    // Two populations meet on this line and the reader has to be able to tell
    // them apart: files/dirs/depth describe the whole indexed corpus, while
    // analyzed/symbols/langs describe only what the current attribute set
    // covers. Printing "files=208 ... langs: javascript=6" with no denominator
    // reads as 202 missing files (express, verified).
    out << "== SUMMARY ==\n"
        << "files=" << file_count << " analyzed=" << static_cast<int>(files.size())
        << " symbols=" << symbol_count
        << " dirs=" << dirs.size() << " depth=" << max_depth << "\n";
    if (!lang_files.empty()) {
        std::vector<std::pair<std::string, int>> langs(lang_files.begin(),
                                                       lang_files.end());
        std::sort(langs.begin(), langs.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second > b.second;
                      return a.first < b.first;
                  });
        out << "langs:";
        for (const auto& [l, n] : langs) out << " " << l << "=" << n;
        out << "\n";
    }
    // Files whose attribute does not activate Analysis sat out every section
    // above. Name the attribute, the count, AND the directories the files
    // came from: a count alone tells a reader that something was left out but
    // not what, and "is my benchmark harness in these numbers?" is the first
    // question any score raises. Omitted entirely when nothing was excluded,
    // keeping the historical SUMMARY shape for untagged corpora.
    if (excluded != nullptr && registry != nullptr) {
        int total = 0;
        for (const auto& e : *excluded) total += e.files;
        if (total > 0) {
            out << "excluded_from_analysis:\n";
            for (size_t i = 0; i < excluded->size(); ++i) {
                const auto& bucket = (*excluded)[i];
                if (bucket.files == 0) continue;
                std::vector<std::pair<std::string, int>> dirs(
                    bucket.dirs.begin(), bucket.dirs.end());
                std::sort(dirs.begin(), dirs.end(),
                          [](const auto& a, const auto& b) {
                              if (a.second != b.second) return a.second > b.second;
                              return a.first < b.first;  // determinism
                          });
                out << "  " << registry->name(static_cast<PathAttrId>(i)) << "="
                    << bucket.files;
                constexpr size_t kMaxDirs = 3;
                if (!dirs.empty()) {
                    out << " (";
                    for (size_t d = 0; d < dirs.size() && d < kMaxDirs; ++d) {
                        if (d > 0) out << " ";
                        out << dirs[d].first << "/";
                    }
                    if (dirs.size() > kMaxDirs) {
                        out << " +" << (dirs.size() - kMaxDirs) << " more";
                    }
                    out << ")";
                }
                out << "\n";
            }
            out << "  (these attributes do not activate \"analysis\"; add "
                   "{\"attributes\":\"all\"} or an attribute name, or tune "
                   ".lci.kdl attributes)\n";
        }
    }
    if (eh && eh->errors.functions_scored > 0) {
        out << "error_handling=" << fmt2(eh->errors.score)
            << " resources=" << fmt2(eh->resources.score) << "\n";
    }
    out << "---\n";
}

// == ENTRY POINTS == — where execution starts / the public surface. main()
// first, then top exported API by importance. C++-only session-startup
// section (Go computes entry points but never emits them).
void emit_entry_points(std::ostringstream& out, const EntryPointsList* ep,
                       std::string_view project_root) {
    if (!ep || ep->main_functions.empty()) return;
    out << "== ENTRY POINTS ==\n";
    // Where the ranking's authority comes from: author annotations, a
    // framework signature, or the heuristic exported-symbol ranking. A
    // heuristic list is a labeled guess and says how to make it
    // authoritative.
    out << "confidence=" << ep->confidence << "\n";
    if (ep->confidence == "heuristic") {
        out << "hint=ranked exports; declare the real front door with "
               "@lci:entry comments or insight { entry_points \"...\" } in "
               ".lci.kdl\n";
    }
    auto rel = [&](const std::string& location) {
        std::string loc = location;
        if (!project_root.empty() && loc.rfind(project_root, 0) == 0) {
            loc = loc.substr(project_root.size());
            while (!loc.empty() && loc.front() == '/') loc.erase(0, 1);
        }
        return loc;
    };
    // The public surface (exported api, library-aware ranked) leads; main()
    // binaries move to a trailing `binaries:` sub-line so demo/example mains
    // stop eating the api slots (a library's front door is its exports).
    std::vector<const EntryPointDef*> apis, mains;
    for (const auto& e : ep->main_functions) {
        // Certification-round exclusions: a Go internal/ package can never
        // be an external entry point, and a bodiless declaration (an
        // abstract method / interface stub, location line == end) is a
        // duplicate of its concrete implementation.
        if (e.type != "main" &&
            (e.location.find("/internal/") != std::string::npos ||
             e.location.rfind("internal/", 0) == 0))
            continue;
        (e.type == "main" ? mains : apis).push_back(&e);
    }
    // Trivially-named exports (a bare `Add`, `Get`, `Run` — C# minimal-API
    // lambdas and interface boilerplate) tell an agent nothing about the
    // codebase's front door; keep them but seat distinctive names first.
    auto is_trivial_name = [](const std::string& n) {
        static const absl::flat_hash_set<std::string> kTrivial = {
            "add",    "get",   "set",    "run",   "new",    "do",
            "open",   "close", "read",   "write", "init",   "start",
            "stop",   "create", "delete", "update", "list",  "find",
            "save",   "load",  "send",   "call",  "next",   "parse",
            "check",  "make",  "map",    "ok",    "handle", "exec",
            "apply",  "index", "count",  "main",  "equals", "tostring",
            "gethashcode", "string", "error", "invoke", "invokeasync"};
        // One- and two-character names (HTML helpers `H`, `U`) say even less.
        if (n.size() <= 2) return true;
        std::string low;
        low.reserve(n.size());
        for (char c : n) low += static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
        return kTrivial.contains(low);
    };
    // Pinned entries (annotated / framework signature) are authoritative:
    // they keep their seats even when trivially named (guzzle's get/post
    // verbs ARE its front door).
    std::stable_partition(apis.begin(), apis.end(),
                          [&](const EntryPointDef* e) {
                              return e->pinned || !is_trivial_name(e->name);
                          });
    size_t lim = std::min(apis.size(), size_t{12});
    for (size_t i = 0; i < lim; ++i) {
        out << "  api: " << apis[i]->name << " (" << rel(apis[i]->location)
            << ")\n";
    }
    if (apis.size() > lim) {
        out << "  ... and " << (apis.size() - lim) << " more exported\n";
    }
    if (!mains.empty()) {
        out << "binaries:";
        size_t blim = std::min(mains.size(), size_t{5});
        for (size_t i = 0; i < blim; ++i) {
            out << (i ? ", " : " ") << mains[i]->name << " ("
                << rel(mains[i]->location) << ")";
        }
        if (mains.size() > blim) out << " (+" << (mains.size() - blim)
                                     << " more)";
        out << "\n";
    }
    out << "---\n";
}


// Import-evidence package dependencies for == DEPENDENCIES ==. The prior
// call-edge counts were materially wrong (pocketbase tools/search claimed
// depended_on_by=27 vs 3 real importers; a chi test-import fabricated a
// mutual root<->middleware dependency): imports ARE the ground truth every
// auditor measured against, so the section now reads them directly.
void emit_next_steps(std::ostringstream& out) {
    out << "== NEXT STEPS ==\n"
        << "1. ENTRY POINTS = where execution starts and the public surface.\n"
        << "2. massive_files = read via search/get_context only, never whole.\n"
        << "3. high-fan-in + GIT HOTSPOTS = load-bearing, conflict-prone code;"
           " edit narrowly.\n"
        << "4. aliases_in_use = search with THIS codebase's vocabulary.\n"
        << "5. get_context {\"id\": \"<o=ID>\"} drills into any [o=..] symbol.\n"
        << "---\n";
}

// == OBJECT IDs == — workflow hint, appended when any smell/problematic
// symbol carried an object id. Matches Go's formatWorkflowHint (note the
// leading "---" producing the doubled separator after HEALTH).
void emit_object_ids_hint(std::ostringstream& out) {
    out << "---\n== OBJECT IDs ==\n"
        << "Use [o=XX] identifiers above with get_context for detailed info:\n"
        << "  get_context {\"id\": \"XX\"}\n"
        << "Example: If you see [o=ABC], use get_context {\"id\": \"ABC\"}\n"
        << "---\n";
}

std::string finalize_lcf(std::ostringstream& out) {
    std::string s = out.str();
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}


}  // namespace insight
}  // namespace mcp
}  // namespace lci
