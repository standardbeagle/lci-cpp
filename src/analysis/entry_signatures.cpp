#include <lci/analysis/entry_signatures.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <absl/container/flat_hash_set.h>
#include <nlohmann/json.hpp>

#include <lci/analysis/framework_signatures_data.h>
#include <lci/config.h>
#include <lci/core/semantic_annotator.h>

namespace lci::analysis {

namespace {

std::string read_small_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The identities this project carries: its own module/package name plus its
// declared dependencies, from whichever manifests exist at the root.
struct ProjectIdentities {
    absl::flat_hash_set<std::string> go_modules;
    absl::flat_hash_set<std::string> composer_names;
    absl::flat_hash_set<std::string> npm_names;
};

ProjectIdentities collect_identities(const std::string& root) {
    namespace fs = std::filesystem;
    ProjectIdentities ids;

    // go.mod: `module <path>` line + `require` paths (both block and inline).
    std::string gomod = read_small_file(fs::path(root) / "go.mod");
    if (!gomod.empty()) {
        std::istringstream in(gomod);
        std::string line;
        bool in_require = false;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string tok;
            ls >> tok;
            if (tok == "module") {
                std::string m;
                ls >> m;
                if (!m.empty()) ids.go_modules.insert(m);
            } else if (tok == "require") {
                std::string next;
                ls >> next;
                if (next == "(") {
                    in_require = true;
                } else if (!next.empty()) {
                    ids.go_modules.insert(next);
                }
            } else if (in_require) {
                if (tok == ")") {
                    in_require = false;
                } else if (!tok.empty() && tok[0] != '/') {
                    ids.go_modules.insert(tok);
                }
            }
        }
    }

    // composer.json / package.json: "name" + dependency map keys.
    auto add_manifest = [&](const char* file,
                            absl::flat_hash_set<std::string>& out) {
        std::string text = read_small_file(fs::path(root) / file);
        if (text.empty()) return;
        auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return;
        if (j.contains("name") && j["name"].is_string())
            out.insert(j["name"].get<std::string>());
        for (const char* dep_key : {"require", "require-dev", "dependencies",
                                    "devDependencies"}) {
            if (j.contains(dep_key) && j[dep_key].is_object()) {
                for (auto it = j[dep_key].begin(); it != j[dep_key].end(); ++it)
                    out.insert(it.key());
            }
        }
    };
    add_manifest("composer.json", ids.composer_names);
    add_manifest("package.json", ids.npm_names);
    return ids;
}

// go.mod identities carry versions/major-suffixes (…/chi/v5); match by
// prefix so a registry key of github.com/go-chi/chi covers them.
bool go_module_matches(const absl::flat_hash_set<std::string>& have,
                       const std::string& want) {
    for (const auto& h : have) {
        if (h == want) return true;
        if (h.size() > want.size() && h.rfind(want, 0) == 0 &&
            h[want.size()] == '/')
            return true;
    }
    return false;
}

std::vector<std::string> registry_pins(const std::string& root) {
    auto reg = nlohmann::json::parse(kFrameworkEntrySignaturesJson, nullptr,
                                     false);
    if (reg.is_discarded() || !reg.contains("frameworks")) return {};
    auto ids = collect_identities(root);
    if (ids.go_modules.empty() && ids.composer_names.empty() &&
        ids.npm_names.empty())
        return {};

    std::vector<std::string> pins;
    absl::flat_hash_set<std::string> seen;
    for (const auto& fw : reg["frameworks"]) {
        if (!fw.contains("match") || !fw.contains("entry_symbols")) continue;
        const auto& m = fw["match"];
        bool hit = false;
        if (m.contains("go_module")) {
            for (const auto& g : m["go_module"])
                if (go_module_matches(ids.go_modules, g.get<std::string>()))
                    hit = true;
        }
        if (!hit && m.contains("composer_name")) {
            for (const auto& c : m["composer_name"])
                if (ids.composer_names.contains(c.get<std::string>()))
                    hit = true;
        }
        if (!hit && m.contains("npm_name")) {
            for (const auto& n : m["npm_name"])
                if (ids.npm_names.contains(n.get<std::string>())) hit = true;
        }
        if (!hit) continue;
        for (const auto& s : fw["entry_symbols"]) {
            auto name = s.get<std::string>();
            if (seen.insert(name).second) pins.push_back(std::move(name));
        }
    }
    return pins;
}

}  // namespace

EntryPointHints resolve_entry_hints(const InsightConfig& insight,
                                    const std::string& project_root,
                                    const SemanticAnnotator* annotator) {
    EntryPointHints hints;

    if (!insight.entry_points.empty()) {
        absl::flat_hash_set<std::string> seen;
        for (const auto& n : insight.entry_points)
            if (seen.insert(n).second) hints.pins.push_back(n);
        hints.confidence = "annotated";
        return hints;
    }

    if (annotator != nullptr) {
        absl::flat_hash_set<std::string> seen;
        for (const auto* sym : annotator->get_symbols_by_label("entry")) {
            if (sym != nullptr && !sym->name.empty() &&
                seen.insert(sym->name).second)
                hints.pins.push_back(sym->name);
        }
        if (!hints.pins.empty()) {
            hints.confidence = "annotated";
            return hints;
        }
    }

    if (!project_root.empty()) {
        hints.pins = registry_pins(project_root);
        if (!hints.pins.empty()) {
            hints.confidence = "framework";
            return hints;
        }
    }

    hints.confidence = "heuristic";
    return hints;
}

}  // namespace lci::analysis
