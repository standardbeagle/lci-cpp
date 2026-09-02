#include <lci/core/callers_report.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace lci {

namespace {

using Snapshot = ReferenceTracker::Snapshot;
using CallSite = Snapshot::CallSite;

/// One confirmed caller group: an enclosing symbol plus its call sites.
struct CallerGroup {
    SymbolID caller{};
    std::string caller_name;   // "<file scope>" when caller == 0
    std::string caller_type;   // "" for file scope
    std::string file_path;     // caller's file (site file for file scope)
    int line{};                // caller's declaration line (first site line
                               // for file scope)
    std::vector<int> call_lines;
};

/// Emits an unattributable site list (dynamic / unresolved), capped like
/// the confirmed groups. Caller name resolved when the site has an
/// enclosing symbol; the site's own location always emitted.
nlohmann::json build_site_list(const Snapshot& snap,
                               const std::vector<CallSite>& sites,
                               size_t cap,
                               const std::function<std::string(FileID)>& path_of) {
    nlohmann::json arr = nlohmann::json::array();
    const size_t n = std::min(sites.size(), cap);
    for (size_t i = 0; i < n; ++i) {
        const auto& s = sites[i];
        nlohmann::json j;
        if (s.caller != 0) {
            if (auto h = snap.get_enhanced_symbol(s.caller)) {
                j["caller"] = h->symbol.name;
            }
        }
        j["file_path"] = path_of(s.file_id);
        j["line"] = s.line;
        arr.push_back(std::move(j));
    }
    return arr;
}

}  // namespace

nlohmann::json build_callers_report(
    const Snapshot& snap, std::string_view name, int max_callers,
    const std::function<std::string(FileID)>& path_of) {
    auto result = snap.collect_callers(name);

    nlohmann::json out;
    out["symbol"] = std::string(name);

    nlohmann::json defs = nlohmann::json::array();
    for (const auto& d : result.definitions) {
        nlohmann::json j;
        j["name"] = d->symbol.name;
        j["type"] = std::string(to_string(d->symbol.type));
        j["file_path"] = path_of(d->symbol.file_id);
        j["line"] = d->symbol.line;
        defs.push_back(std::move(j));
    }
    out["definitions"] = std::move(defs);

    // Group confirmed sites by enclosing caller. Sites arrive sorted by
    // (file_id, line, column); groups re-sort by emitted path for
    // cross-machine determinism.
    // File-scope sites (caller == 0) group PER FILE — one shared key would
    // merge top-level callers across files into a single bogus group.
    absl::flat_hash_map<std::pair<SymbolID, FileID>, size_t> group_of;
    std::vector<CallerGroup> groups;
    groups.reserve(16);
    for (const auto& s : result.confirmed) {
        const FileID scope_file = s.caller == 0 ? s.file_id : FileID{};
        auto [it, inserted] =
            group_of.emplace(std::make_pair(s.caller, scope_file),
                             groups.size());
        if (inserted) {
            CallerGroup g;
            g.caller = s.caller;
            if (s.caller != 0) {
                if (auto h = snap.get_enhanced_symbol(s.caller)) {
                    g.caller_name = h->symbol.name;
                    g.caller_type = std::string(to_string(h->symbol.type));
                    g.file_path = path_of(h->symbol.file_id);
                    g.line = h->symbol.line;
                }
            }
            if (g.caller_name.empty()) {
                g.caller_name = "<file scope>";
                g.file_path = path_of(s.file_id);
                g.line = s.line;
            }
            groups.push_back(std::move(g));
        }
        groups[it->second].call_lines.push_back(s.line);
    }
    std::sort(groups.begin(), groups.end(),
              [](const CallerGroup& a, const CallerGroup& b) {
                  if (a.file_path != b.file_path)
                      return a.file_path < b.file_path;
                  if (a.line != b.line) return a.line < b.line;
                  return a.caller_name < b.caller_name;
              });

    const auto cap = static_cast<size_t>(std::max(max_callers, 1));
    const size_t shown = std::min(groups.size(), cap);

    nlohmann::json callers = nlohmann::json::array();
    for (size_t i = 0; i < shown; ++i) {
        auto& g = groups[i];
        nlohmann::json j;
        j["caller"] = std::move(g.caller_name);
        if (!g.caller_type.empty()) j["caller_type"] = g.caller_type;
        j["file_path"] = std::move(g.file_path);
        j["line"] = g.line;
        j["call_lines"] = g.call_lines;
        j["call_count"] = static_cast<int>(g.call_lines.size());
        callers.push_back(std::move(j));
    }
    out["callers"] = std::move(callers);
    out["total_callers"] = static_cast<int>(groups.size());
    out["total_call_sites"] = static_cast<int>(result.confirmed.size());
    out["truncated"] = shown < groups.size();

    // Unattributable sites, split and labeled — never mixed with confirmed.
    if (!result.dynamic.empty()) {
        out["dynamic_call_sites"] =
            build_site_list(snap, result.dynamic, cap, path_of);
        out["dynamic_count"] = static_cast<int>(result.dynamic.size());
    }
    if (!result.unresolved.empty()) {
        out["unresolved_call_sites"] =
            build_site_list(snap, result.unresolved, cap, path_of);
        out["unresolved_count"] = static_cast<int>(result.unresolved.size());
    }
    return out;
}

}  // namespace lci
