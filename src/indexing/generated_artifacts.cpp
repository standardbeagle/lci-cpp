#include <lci/indexing/generated_artifacts.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include <absl/container/flat_hash_set.h>
#include <nlohmann/json.hpp>

namespace lci {

namespace fs = std::filesystem;

namespace {

// Reads a whole file; empty string on any failure (manifests are small).
std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Normalizes a manifest-declared output dir to a root-relative glob, or
// empty when the value is unusable or already covered by static defaults.
std::string dir_to_glob(std::string dir) {
    // Strip leading "./" and trailing separators.
    while (dir.rfind("./", 0) == 0) dir = dir.substr(2);
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
        dir.pop_back();
    }
    std::replace(dir.begin(), dir.end(), '\\', '/');
    if (dir.empty() || dir == "." || dir[0] == '/') return {};
    // Escapes the project (absolute or ../) or is drive-lettered: skip.
    if (dir.rfind("..", 0) == 0 ||
        (dir.size() > 1 && dir[1] == ':')) {
        return {};
    }
    // Already covered by the static default excludes.
    static const absl::flat_hash_set<std::string> kCovered = {
        "dist", "build", "out", "target", "bin", "obj", "vendor",
    };
    std::string first = dir.substr(0, dir.find('/'));
    if (kCovered.contains(first) || first.empty() || first[0] == '.') {
        return {};
    }
    return dir + "/**";
}

// tsconfig/jsconfig allow trailing commas and comments (JSONC); nlohmann
// tolerates comments when asked and we ignore parse errors entirely.
void collect_ts_outdir(const fs::path& root, const char* name,
                       std::vector<std::string>& out) {
    auto text = slurp(root / name);
    if (text.empty()) return;
    auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false,
                                   /*ignore_comments=*/true);
    if (j.is_discarded()) return;
    auto co = j.find("compilerOptions");
    if (co == j.end() || !co->is_object()) return;
    auto od = co->find("outDir");
    if (od == co->end() || !od->is_string()) return;
    if (auto g = dir_to_glob(od->get<std::string>()); !g.empty()) {
        out.push_back(std::move(g));
    }
}

void collect_composer_vendor(const fs::path& root,
                             std::vector<std::string>& out) {
    auto text = slurp(root / "composer.json");
    if (text.empty()) return;
    auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false,
                                   /*ignore_comments=*/true);
    if (j.is_discarded()) return;
    auto cfg = j.find("config");
    if (cfg == j.end() || !cfg->is_object()) return;
    auto vd = cfg->find("vendor-dir");
    if (vd == cfg->end() || !vd->is_string()) return;
    if (auto g = dir_to_glob(vd->get<std::string>()); !g.empty()) {
        out.push_back(std::move(g));
    }
}

// MSBuild output dirs from .csproj/.fsproj/.vbproj. Regex over the XML —
// the three properties are simple text elements and a real XML parser is
// not worth a dependency for this.
void collect_msbuild_outputs(const fs::path& proj_file, const fs::path& root,
                             std::vector<std::string>& out) {
    auto text = slurp(proj_file);
    if (text.empty()) return;
    static const std::regex kProp(
        "<(OutputPath|BaseOutputPath|BaseIntermediateOutputPath)>"
        "([^<]+)</\\1>");
    auto begin = std::sregex_iterator(text.begin(), text.end(), kProp);
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        std::string dir = (*it)[2].str();
        // MSBuild vars ($(Configuration) etc.): keep the literal prefix
        // before the first variable; a fully-variable path contributes
        // nothing.
        if (auto dollar = dir.find('$'); dollar != std::string::npos) {
            dir = dir.substr(0, dollar);
        }
        // Root-relativize against the project file's directory.
        std::error_code ec;
        auto rel = fs::relative(proj_file.parent_path() / dir, root, ec);
        if (ec) continue;
        if (auto g = dir_to_glob(rel.generic_string()); !g.empty()) {
            out.push_back(std::move(g));
        }
    }
}

bool is_msbuild_project(const fs::path& p) {
    auto ext = p.extension().string();
    return ext == ".csproj" || ext == ".fsproj" || ext == ".vbproj";
}

}  // namespace

std::vector<std::string> derive_generated_excludes(const std::string& root) {
    std::vector<std::string> out;
    fs::path r(root);
    std::error_code ec;
    if (!fs::is_directory(r, ec) || ec) return out;

    collect_ts_outdir(r, "tsconfig.json", out);
    collect_ts_outdir(r, "jsconfig.json", out);
    collect_composer_vendor(r, out);

    // MSBuild projects: bounded walk to depth 2 (covers the common
    // src/<Project>/<Project>.csproj layout) — a full recursive walk here
    // would duplicate the scanner's job. Dot- and dependency-dirs are
    // skipped; they never hold first-party project files.
    auto it = fs::recursive_directory_iterator(
        r, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        if (entry.is_directory()) {
            auto name = entry.path().filename().string();
            if (it.depth() >= 2 || name.empty() || name[0] == '.' ||
                name == "node_modules" || name == "vendor") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (entry.is_regular_file() && is_msbuild_project(entry.path())) {
            collect_msbuild_outputs(entry.path(), r, out);
        }
    }

    // Deterministic output regardless of directory iteration order.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace lci
