#include <lci/analysis/coupling_analyzer.h>

#include <lci/reference.h>

#include <algorithm>
#include <cctype>
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

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool CouplingAnalyzer::is_code_file(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return false;

    std::string ext = to_lower(path.substr(dot));

    static const std::string_view code_exts[] = {
        ".go",  ".js",    ".ts",  ".tsx",   ".jsx",  ".py",
        ".java", ".rs",   ".c",   ".cpp",   ".h",    ".hpp",
        ".cs",  ".rb",    ".php", ".swift", ".kt",   ".scala",
        ".ex",  ".exs",
    };
    for (auto e : code_exts) {
        if (ext == e) return true;
    }
    return false;
}

std::string CouplingAnalyzer::get_package_name(std::string_view file_path,
                                                std::string_view project_root) {
    std::string rel(file_path);
    if (!project_root.empty() && file_path.size() > project_root.size() &&
        file_path.substr(0, project_root.size()) == project_root) {
        size_t start = project_root.size();
        if (start < file_path.size() && (file_path[start] == '/' || file_path[start] == '\\')) {
            ++start;
        }
        rel = std::string(file_path.substr(start));
    }

    auto dir = std::filesystem::path(rel).parent_path().string();
    if (dir.empty() || dir == ".") return "(root)";
    return dir;
}

void CouplingAnalyzer::set_exclude_patterns(std::vector<std::string> patterns) {
    exclude_patterns_ = std::move(patterns);
}

bool CouplingAnalyzer::is_excluded(std::string_view name) const {
    for (const auto& pat : exclude_patterns_) {
        if (contains(name, pat)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Main analysis
// ---------------------------------------------------------------------------

CouplingAnalyzer::CouplingResult CouplingAnalyzer::analyze(
    const std::vector<FileSymbolData>& files,
    std::string_view project_root) const {

    CouplingMetrics coupling;
    CohesionMetrics cohesion;

    // Map each symbol to its package and count symbols per package.
    absl::flat_hash_map<const EnhancedSymbol*, std::string> sym_to_pkg;
    absl::flat_hash_map<std::string, int> pkg_symbol_count;

    for (const auto& file : files) {
        if (!is_code_file(file.path)) continue;
        std::string pkg = get_package_name(file.path, project_root);
        for (const auto* sym : file.symbols) {
            sym_to_pkg[sym] = pkg;
            pkg_symbol_count[pkg]++;
        }
    }

    // Count cross-package references using outgoing refs.
    // packageDeps[source][target] = count
    absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, int>> pkg_deps;
    for (const auto& [pkg, _] : pkg_symbol_count) {
        pkg_deps[pkg];  // ensure entry exists
    }

    // Build a symbol-id to package mapping for resolving targets.
    absl::flat_hash_map<SymbolID, std::string> id_to_pkg;
    for (const auto& [sym, pkg] : sym_to_pkg) {
        id_to_pkg[sym->id] = pkg;
    }

    for (const auto& [sym, src_pkg] : sym_to_pkg) {
        for (const auto& ref : sym->outgoing_refs) {
            auto it = id_to_pkg.find(ref.target_symbol);
            if (it != id_to_pkg.end()) {
                pkg_deps[src_pkg][it->second]++;
            }
        }
    }

    // Calculate metrics for each package.
    double total_coupling = 0.0;
    double max_coupling = 0.0;
    double total_cohesion_val = 0.0;
    double min_cohesion = 1.0;
    std::vector<std::string> low_cohesion_pkgs;
    int pkg_count = 0;

    for (const auto& [pkg, deps] : pkg_deps) {
        ++pkg_count;
        int symbol_count = pkg_symbol_count[pkg];

        // Internal refs (self-references)
        int internal_refs = 0;
        if (auto it = deps.find(pkg); it != deps.end()) {
            internal_refs = it->second;
        }

        // Efferent (outgoing to other packages)
        int efferent = 0;
        for (const auto& [target, count] : deps) {
            if (target != pkg) efferent += count;
        }

        // Afferent (incoming from other packages)
        int afferent = 0;
        for (const auto& [src, src_deps] : pkg_deps) {
            if (src != pkg) {
                if (auto it = src_deps.find(pkg); it != src_deps.end()) {
                    afferent += it->second;
                }
            }
        }

        coupling.afferent_coupling[pkg] = afferent;
        coupling.efferent_coupling[pkg] = efferent;

        // Instability: I = Ce / (Ca + Ce)
        double total_for_pkg = static_cast<double>(afferent + efferent);
        double inst = 0.5;
        if (total_for_pkg > 0.0) {
            inst = static_cast<double>(efferent) / total_for_pkg;
        }
        coupling.instability[pkg] = inst;

        // Normalized coupling score
        double norm_coupling = 0.0;
        if (symbol_count > 0) {
            norm_coupling = static_cast<double>(efferent) /
                            static_cast<double>(symbol_count * 5);
            if (norm_coupling > 1.0) norm_coupling = 1.0;
        }
        coupling.module_coupling[pkg] = norm_coupling;
        total_coupling += norm_coupling;
        if (norm_coupling > max_coupling) max_coupling = norm_coupling;

        // Cohesion: ratio of internal refs to total outgoing refs
        int total_refs = internal_refs + efferent;
        double coh = 0.5;
        if (total_refs > 0) {
            coh = static_cast<double>(internal_refs) / static_cast<double>(total_refs);
        }
        cohesion.relational_cohesion[pkg] = coh;
        total_cohesion_val += coh;
        if (coh < min_cohesion) min_cohesion = coh;

        if (coh < 0.3 && !is_excluded(pkg)) {
            low_cohesion_pkgs.push_back(pkg);
        }
    }

    if (pkg_count > 0) {
        coupling.average_coupling = total_coupling / static_cast<double>(pkg_count);
        cohesion.average_cohesion = total_cohesion_val / static_cast<double>(pkg_count);
    }
    coupling.max_coupling = max_coupling;
    cohesion.min_cohesion = min_cohesion;

    // Sort low cohesion by value and limit to 5
    std::sort(low_cohesion_pkgs.begin(), low_cohesion_pkgs.end(),
              [&cohesion](const std::string& a, const std::string& b) {
                  return cohesion.relational_cohesion[a] <
                         cohesion.relational_cohesion[b];
              });
    if (low_cohesion_pkgs.size() > 5) {
        low_cohesion_pkgs.resize(5);
    }
    cohesion.low_cohesion_modules = std::move(low_cohesion_pkgs);

    return {std::move(coupling), std::move(cohesion)};
}

}  // namespace lci
