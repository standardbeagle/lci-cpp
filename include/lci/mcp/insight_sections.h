#pragma once

// LCF section rendering for the code_insight report. Deep module: the
// interface is "render section X from analyzer output Y into the stream";
// everything about LCF layout, ordering, truncation, and labeling lives
// behind it (insight_sections.cpp, insight_graph.cpp).

#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/analysis/codebase_intelligence_types.h>
#include <lci/analysis/entry_signatures.h>
#include <lci/analysis/error_handling_analyzer.h>
#include <lci/analysis/module_analyzer.h>
#include <lci/analysis/naming_analyzer.h>
#include <lci/git/frequency_analyzer.h>
#include <lci/git/types.h>

namespace lci {

class MasterIndex;
class PathAttrRegistry;

class GraphPropagator;

namespace mcp {
namespace insight {

// Fixed-precision number formatting shared by every section.
std::string fmt2(double v);
std::string fmt1(double v);

/// Files excluded from analysis under one attribute, with where they live.
struct ExcludedAttr {
    int files{};
    // Top-level directory -> file count, so the summary can name where the
    // excluded code lives instead of only how much of it there is.
    absl::flat_hash_map<std::string, int> dirs;
};


struct LoadBearingSym {
    std::string name;
    std::string location;  // project-root-relative path:line
    int reach{};           // distinct transitive callers
};

// A graph-detected community (Louvain) with its largest-reach exemplar members
// and, when semantic labels are available, the dominant propagated @lci: label
// across its members (domain) plus the fraction carrying it (coherence).
struct ClusterInfo {
    int size{};
    std::vector<std::string> exemplars;
    std::string domain;     // dominant propagated label, "" if none/weak
    double coherence{};     // members carrying `domain` / size
};

// A broker: a symbol with high betweenness — many shortest paths route through
// it, so it bridges otherwise-separate regions (a chokepoint).
struct BrokerSym {
    std::string name;
    std::string location;
    double score{};  // normalized betweenness
};

// An upward call that violates layered architecture: a deeper layer calling a
// shallower one (e.g. Data -> Presentation).
struct LayerViolation {
    std::string caller, caller_layer;
    std::string callee, callee_layer;
};

// One strongly-connected group of the call graph, kept actionable: the first
// few member names (chain order for display) plus the file the cycle lives in.
struct CycleGroup {
    std::vector<std::string> names;  // first <=3 members, sorted
    int total_size{};                // full SCC size
    std::string file;                // file of the first member
};

// Everything derived from one build of the call graph.
struct GraphSignals {
    std::vector<LoadBearingSym> load_bearing;
    std::vector<BrokerSym> brokers;                // top betweenness, may be empty
    std::vector<CycleGroup> cycles;                // cyclic groups with files
    // Direct self-recursion (name, location) — reported apart from cycles.
    std::vector<std::pair<std::string, std::string>> recursion;
    std::vector<ClusterInfo> clusters;             // communities by size desc
    std::vector<LayerViolation> layer_violations;  // upward calls, may be empty
    double modularity{};
    int community_count{};
};


struct ImportDeps {
    // target package -> distinct importing packages
    absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>> in;
    // source package -> distinct imported packages
    absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>> out;
    // Circular package dependencies (SCCs of the import graph, size >= 2),
    // members sorted, largest first. Import cycles are the fallow-class
    // finding call-graph CYCLES cannot see: mutual imports couple whole
    // packages even when no call edge closes a loop.
    std::vector<std::vector<std::string>> cycles;
};


int lcf_token_count(int n_modules, int n_dep_edges, bool has_health,
                    int n_entry, bool has_stats);
void emit_lcf_header(std::ostringstream& out, std::string_view mode, int tier,
                     int tokens, std::string_view scope = {});
inline void emit_lcf_header_scoped(std::ostringstream& out,
                                   std::string_view scope,
                                   std::string_view mode, int tier,
                                   int tokens) {
    emit_lcf_header(out, mode, tier, tokens, scope);
}
std::string git_rel(std::string_view path, std::string_view root);
void emit_repository_map(std::ostringstream& out,
                         const std::vector<ModuleBoundary>& mods);
void emit_health(std::ostringstream& out, const HealthDashboard& hd,
                 const PuritySummary* purity);
void emit_modules(std::ostringstream& out, const ModuleAnalysis& ma);
void emit_statistics(std::ostringstream& out, const ComplexityMetrics& cm,
                     const CouplingMetrics& cp, const CohesionMetrics& ch,
                     const QualityMetrics& q, double purity_ratio);
void emit_git_changes(std::ostringstream& out, const git::AnalysisReport& r,
                      std::string_view root);
void emit_git_hotspots(std::ostringstream& out,
                       const git::ChangeFrequencyReport& r,
                       git::TimeWindow window, std::string_view root);
void emit_eh_finding_lines(std::ostringstream& out,
                           const std::vector<EhFindingEntry>& findings,
                           size_t limit);
void emit_error_handling(std::ostringstream& out,
                         const ErrorHandlingSummary& s, size_t max_findings);
void emit_resource_management(std::ostringstream& out,
                              const ResourceSummary& s, size_t max_findings);
void emit_vocabulary(std::ostringstream& out, const NamingReport& nr);
void emit_summary(std::ostringstream& out,
                  const std::vector<FileSymbolData>& files,
                  const std::vector<std::string>& file_paths,
                  std::string_view project_root, int file_count,
                  int symbol_count,
                  const std::vector<ExcludedAttr>* excluded = nullptr,
                  const PathAttrRegistry* registry = nullptr,
                  const ErrorHandlingAnalyzer::Result* eh = nullptr);
void emit_entry_points(std::ostringstream& out, const EntryPointsList* ep,
                       std::string_view project_root);
void emit_dependencies(std::ostringstream& out, const ImportDeps& deps);
void emit_next_steps(std::ostringstream& out);
void emit_object_ids_hint(std::ostringstream& out);
std::string finalize_lcf(std::ostringstream& out);

// Call-graph / import-graph signal computation + emission (insight_graph.cpp).
GraphSignals compute_graph_signals(const MasterIndex& indexer,
                                   const std::vector<FileSymbolData>& files,
                                   std::string_view project_root, size_t top_n,
                                   const GraphPropagator* propagator);
ImportDeps compute_import_dependencies(MasterIndex& indexer,
                                       const std::vector<FileSymbolData>& files,
                                       std::string_view project_root);
void emit_load_bearing(std::ostringstream& out, const GraphSignals& sig);
void emit_cycles(std::ostringstream& out,
                 const std::vector<CycleGroup>& cycles,
                 const std::vector<std::pair<std::string, std::string>>&
                     recursion);
void emit_layer_violations(std::ostringstream& out,
                           const std::vector<LayerViolation>& v);
void emit_clusters(std::ostringstream& out, const GraphSignals& sig);

}  // namespace insight
}  // namespace mcp
}  // namespace lci
