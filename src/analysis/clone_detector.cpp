#include <lci/analysis/clone_detector.h>

#include <algorithm>
#include <numeric>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>

#include <lci/analysis/code_similarity.h>

namespace lci {

namespace {

/// Slices 1-based [start_line, end_line] out of file content without
/// allocating per line.
std::string_view slice_lines(std::string_view content, int start_line,
                             int end_line) {
    if (start_line <= 0 || content.empty() || end_line < start_line) return {};
    int line = 1;
    size_t begin = 0;
    while (line < start_line) {
        auto nl = content.find('\n', begin);
        if (nl == std::string_view::npos) return {};
        begin = nl + 1;
        ++line;
    }
    size_t end = begin;
    while (line <= end_line) {
        auto nl = content.find('\n', end);
        if (nl == std::string_view::npos) {
            end = content.size();
            break;
        }
        end = nl + 1;
        ++line;
    }
    return content.substr(begin, end - begin);
}

int count_lines(std::string_view s) {
    if (s.empty()) return 0;
    return static_cast<int>(std::count(s.begin(), s.end(), '\n')) +
           (s.back() == '\n' ? 0 : 1);
}

std::string lexical_rel(std::string_view path, std::string_view root) {
    if (!root.empty() && path.size() > root.size() &&
        path.substr(0, root.size()) == root &&
        (path[root.size()] == '/' || path[root.size()] == '\\')) {
        return std::string(path.substr(root.size() + 1));
    }
    return std::string(path);
}

struct Candidate {
    CloneMember member;
    std::string normalized;
    int norm_lines{};
};

/// Union-find over candidate indices for the structural stage.
struct UnionFind {
    std::vector<int> parent;
    explicit UnionFind(size_t n) : parent(n) {
        std::iota(parent.begin(), parent.end(), 0);
    }
    int find(int x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    }
    void unite(int a, int b) { parent[find(a)] = find(b); }
};

}  // namespace

CloneReport CloneDetector::analyze(MasterIndex& index,
                                   std::string_view project_root,
                                   const std::vector<bool>& allowed_attrs,
                                   const Options& opts) const {
    CloneReport report;

    auto fids = index.get_all_file_ids();
    std::sort(fids.begin(), fids.end());  // deterministic scan order
    auto snap = index.load_snapshot();
    auto rt_snap = index.ref_tracker().pin();

    std::vector<Candidate> candidates;
    for (auto fid : fids) {
        if (!allowed_attrs.empty()) {
            auto attr = static_cast<size_t>(snap->attr_of(fid));
            if (attr >= allowed_attrs.size() || !allowed_attrs[attr]) continue;
        }
        auto content = index.file_content_store().get_content(fid);
        if (content.empty()) continue;
        std::string rel = lexical_rel(index.get_file_path(fid), project_root);

        for (const auto& sym : rt_snap->get_file_enhanced_symbols(fid)) {
            if (sym == nullptr) continue;
            auto type = sym->symbol.type;
            if (type != SymbolType::Function && type != SymbolType::Method)
                continue;
            int line = static_cast<int>(sym->symbol.line);
            int end_line = static_cast<int>(sym->symbol.end_line);
            if (end_line - line + 1 < opts.min_lines) continue;

            auto body = slice_lines(content, line, end_line);
            if (body.empty()) continue;
            auto normalized = normalize_code_content(body);
            int norm_lines = count_lines(normalized);
            if (norm_lines < opts.min_lines) continue;

            Candidate c;
            c.member = {rel, line, end_line, sym->symbol.name};
            c.normalized = std::move(normalized);
            c.norm_lines = norm_lines;
            report.function_lines += norm_lines;
            ++report.functions_scanned;
            candidates.push_back(std::move(c));
        }
    }

    // --- Exact clone classes: group by normalized-content hash. Hash
    // collisions are resolved by comparing against the group's
    // representative; a mismatch falls through to the structural pool.
    struct ExactGroup {
        std::vector<int> members;  // candidate indices
    };
    absl::flat_hash_map<size_t, ExactGroup> by_hash;
    by_hash.reserve(candidates.size());
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        size_t h = absl::HashOf(candidates[i].normalized);
        auto& group = by_hash[h];
        if (!group.members.empty() &&
            candidates[group.members.front()].normalized !=
                candidates[i].normalized) {
            continue;  // collision: leave it for the structural stage
        }
        group.members.push_back(i);
    }

    std::vector<CloneClass> classes;
    absl::flat_hash_set<int> in_exact_class;
    for (auto& [h, group] : by_hash) {
        if (group.members.size() < 2) continue;
        CloneClass cc;
        cc.exact = true;
        cc.similarity = 1.0;
        cc.lines = candidates[group.members.front()].norm_lines;
        for (int idx : group.members) {
            cc.members.push_back(candidates[idx].member);
            in_exact_class.insert(idx);
        }
        cc.duplicated_lines =
            cc.lines * static_cast<int>(cc.members.size() - 1);
        classes.push_back(std::move(cc));
    }

    // --- Structural clone classes among the LARGEST remaining functions.
    // The pairwise stage is capped at structural_top_n so a 500k-symbol
    // corpus stays bounded; the size-ratio bound (Jaccard >= t implies
    // min/max set size >= t) skips most pairs before the intersection.
    std::vector<int> pool;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (!in_exact_class.contains(i)) pool.push_back(i);
    }
    std::sort(pool.begin(), pool.end(), [&](int a, int b) {
        if (candidates[a].norm_lines != candidates[b].norm_lines)
            return candidates[a].norm_lines > candidates[b].norm_lines;
        if (candidates[a].member.path != candidates[b].member.path)
            return candidates[a].member.path < candidates[b].member.path;
        return candidates[a].member.line < candidates[b].member.line;
    });
    if (static_cast<int>(pool.size()) > opts.structural_top_n) {
        pool.resize(static_cast<size_t>(opts.structural_top_n));
    }

    std::vector<absl::flat_hash_set<std::string>> tokens(pool.size());
    for (size_t i = 0; i < pool.size(); ++i) {
        tokens[i] = code_token_set(candidates[pool[i]].normalized);
    }

    UnionFind uf(pool.size());
    absl::flat_hash_map<int, double> min_similarity;  // root -> min edge
    for (size_t a = 0; a < pool.size(); ++a) {
        for (size_t b = a + 1; b < pool.size(); ++b) {
            double lo = static_cast<double>(
                std::min(tokens[a].size(), tokens[b].size()));
            double hi = static_cast<double>(
                std::max(tokens[a].size(), tokens[b].size()));
            if (hi == 0.0 || lo / hi < opts.structural_threshold) continue;
            double sim = token_set_similarity(tokens[a], tokens[b]);
            if (sim < opts.structural_threshold) continue;
            uf.unite(static_cast<int>(a), static_cast<int>(b));
            int root = uf.find(static_cast<int>(a));
            auto it = min_similarity.find(root);
            if (it == min_similarity.end() || sim < it->second) {
                min_similarity[root] = sim;
            }
        }
    }

    absl::flat_hash_map<int, std::vector<int>> structural_groups;
    for (size_t i = 0; i < pool.size(); ++i) {
        structural_groups[uf.find(static_cast<int>(i))].push_back(
            static_cast<int>(i));
    }
    for (auto& [root, group] : structural_groups) {
        if (group.size() < 2) continue;
        CloneClass cc;
        cc.exact = false;
        auto sim_it = min_similarity.find(root);
        cc.similarity =
            sim_it != min_similarity.end() ? sim_it->second : 0.0;
        int min_lines_in_group = candidates[pool[group.front()]].norm_lines;
        for (int gi : group) {
            const auto& cand = candidates[pool[gi]];
            cc.members.push_back(cand.member);
            min_lines_in_group = std::min(min_lines_in_group,
                                          cand.norm_lines);
        }
        cc.lines = min_lines_in_group;
        cc.duplicated_lines =
            cc.lines * static_cast<int>(cc.members.size() - 1);
        classes.push_back(std::move(cc));
    }

    // --- Deterministic ordering + totals.
    for (auto& cc : classes) {
        std::sort(cc.members.begin(), cc.members.end(),
                  [](const CloneMember& a, const CloneMember& b) {
                      if (a.path != b.path) return a.path < b.path;
                      return a.line < b.line;
                  });
    }
    std::sort(classes.begin(), classes.end(),
              [](const CloneClass& a, const CloneClass& b) {
                  if (a.duplicated_lines != b.duplicated_lines)
                      return a.duplicated_lines > b.duplicated_lines;
                  if (a.members.front().path != b.members.front().path)
                      return a.members.front().path < b.members.front().path;
                  return a.members.front().line < b.members.front().line;
              });

    report.clone_classes = static_cast<int>(classes.size());
    for (const auto& cc : classes) report.duplicated_lines += cc.duplicated_lines;
    report.duplication_pct =
        report.function_lines > 0
            ? 100.0 * report.duplicated_lines / report.function_lines
            : 0.0;
    if (static_cast<int>(classes.size()) > opts.max_classes) {
        classes.resize(static_cast<size_t>(opts.max_classes));
    }
    report.classes = std::move(classes);
    return report;
}

}  // namespace lci
