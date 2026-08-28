#include <lci/analysis/scope_set.h>

#include <algorithm>
#include <charconv>

#include <re2/re2.h>

#include <lci/symbol.h>

namespace lci {

namespace {

// Same glob semantics as find_files: '*' matches any run (including '/'),
// '?' one char. Allocation-free two-pointer scan with star-backtracking.
bool glob_match(std::string_view str, std::string_view pat) {
    size_t s = 0, p = 0, star_p = std::string_view::npos, star_s = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++s;
            ++p;
        } else if (p < pat.size() && pat[p] == '*') {
            star_p = p++;
            star_s = s;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            s = ++star_s;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

void merge_ranges(std::vector<LineRange>& ranges) {
    if (ranges.size() < 2) return;
    std::sort(ranges.begin(), ranges.end(),
              [](const LineRange& a, const LineRange& b) {
                  if (a.start != b.start) return a.start < b.start;
                  return a.end < b.end;
              });
    std::vector<LineRange> merged;
    merged.reserve(ranges.size());
    for (const auto& r : ranges) {
        if (!merged.empty() && r.start <= merged.back().end + 1) {
            merged.back().end = std::max(merged.back().end, r.end);
        } else {
            merged.push_back(r);
        }
    }
    ranges = std::move(merged);
}

bool overlaps(const std::vector<LineRange>& ranges, int start, int end) {
    // Whole-file entry: empty ranges overlap everything.
    if (ranges.empty()) return true;
    for (const auto& r : ranges) {
        if (r.start <= end && start <= r.end) return true;
    }
    return false;
}

}  // namespace

ScopeSet ScopeSet::all() {
    ScopeSet s;
    s.all_ = true;
    return s;
}

ScopeSet ScopeSet::none() { return ScopeSet{}; }

void ScopeSet::add_file(std::string path) {
    if (all_) return;
    files_[std::move(path)].clear();
}

void ScopeSet::add_lines(std::string path, LineRange range) {
    if (all_) return;
    if (range.end < range.start) return;
    auto it = files_.find(path);
    if (it != files_.end() && it->second.empty()) return;  // already whole
    auto& ranges = files_[std::move(path)];
    ranges.push_back(range);
    merge_ranges(ranges);
}

bool ScopeSet::contains_file(std::string_view path) const {
    if (all_) return true;
    return files_.contains(path);
}

bool ScopeSet::contains_lines(std::string_view path, int start, int end) const {
    if (all_) return true;
    auto it = files_.find(path);
    if (it == files_.end()) return false;
    return overlaps(it->second, start, end);
}

bool ScopeSet::contains_symbol(std::string_view path,
                               const EnhancedSymbol& sym) const {
    int start = sym.symbol.line;
    int end = std::max(sym.symbol.end_line, start);
    return contains_lines(path, start, end);
}

ScopeSet ScopeSet::unite(const ScopeSet& other) const {
    if (all_ || other.all_) return all();
    ScopeSet out = *this;
    for (const auto& [path, ranges] : other.files_) {
        if (ranges.empty()) {
            out.add_file(path);
        } else {
            for (const auto& r : ranges) out.add_lines(path, r);
        }
    }
    return out;
}

ScopeSet ScopeSet::intersect(const ScopeSet& other) const {
    if (all_) return other;
    if (other.all_) return *this;
    ScopeSet out;
    for (const auto& [path, ranges] : files_) {
        auto it = other.files_.find(path);
        if (it == other.files_.end()) continue;
        const auto& theirs = it->second;
        if (ranges.empty()) {
            // We are whole-file: their entry wins verbatim.
            if (theirs.empty()) {
                out.add_file(path);
            } else {
                for (const auto& r : theirs) out.add_lines(path, r);
            }
        } else if (theirs.empty()) {
            for (const auto& r : ranges) out.add_lines(path, r);
        } else {
            for (const auto& a : ranges) {
                for (const auto& b : theirs) {
                    int s = std::max(a.start, b.start);
                    int e = std::min(a.end, b.end);
                    if (s <= e) out.add_lines(path, LineRange{s, e});
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Populators
// ---------------------------------------------------------------------------

ScopeSet scope_from_paths(const std::vector<std::string>& paths) {
    ScopeSet s;
    for (const auto& p : paths) s.add_file(p);
    return s;
}

ScopeSet scope_from_globs(const std::vector<std::string>& globs,
                          const std::vector<std::string>& candidate_paths) {
    ScopeSet s;
    for (const auto& path : candidate_paths) {
        for (const auto& g : globs) {
            if (glob_match(path, g)) {
                s.add_file(path);
                break;
            }
        }
    }
    return s;
}

ScopeSet scope_from_regex(const std::string& pattern,
                          const std::vector<std::string>& candidate_paths,
                          std::string& error) {
    RE2 re(pattern);
    if (!re.ok()) {
        error = "invalid regex: " + re.error();
        return ScopeSet::none();
    }
    ScopeSet s;
    for (const auto& path : candidate_paths) {
        if (RE2::PartialMatch(path, re)) s.add_file(path);
    }
    return s;
}

ScopeSet scope_from_symbols(
    const std::vector<std::pair<std::string, const EnhancedSymbol*>>& symbols) {
    ScopeSet s;
    for (const auto& [path, sym] : symbols) {
        if (sym == nullptr) continue;
        int start = sym->symbol.line;
        int end = std::max(sym->symbol.end_line, start);
        s.add_lines(path, LineRange{start, end});
    }
    return s;
}

ScopeSet scope_from_unified_diff(std::string_view diff_text) {
    ScopeSet s;
    std::string current_file;
    size_t pos = 0;
    while (pos < diff_text.size()) {
        size_t eol = diff_text.find('\n', pos);
        std::string_view line = diff_text.substr(
            pos, eol == std::string_view::npos ? std::string_view::npos
                                               : eol - pos);
        if (line.rfind("+++ ", 0) == 0) {
            std::string_view target = line.substr(4);
            if (target == "/dev/null") {
                current_file.clear();  // pure deletion: nothing on new side
            } else {
                if (target.rfind("b/", 0) == 0) target.remove_prefix(2);
                current_file.assign(target);
            }
        } else if (line.rfind("@@", 0) == 0 && !current_file.empty()) {
            // "@@ -a[,b] +c[,d] @@": new-side start c, count d (default 1).
            size_t plus = line.find('+');
            if (plus != std::string_view::npos) {
                std::string_view rest = line.substr(plus + 1);
                int start = 0, count = 1;
                auto [p1, ec1] = std::from_chars(
                    rest.data(), rest.data() + rest.size(), start);
                if (ec1 == std::errc{}) {
                    if (p1 < rest.data() + rest.size() && *p1 == ',') {
                        std::from_chars(p1 + 1, rest.data() + rest.size(),
                                        count);
                    }
                    if (count > 0) {
                        s.add_lines(current_file,
                                    LineRange{start, start + count - 1});
                    } else {
                        // Pure deletion hunk (d==0): the change touches the
                        // boundary line on the new side.
                        int anchor = std::max(start, 1);
                        s.add_lines(current_file, LineRange{anchor, anchor});
                    }
                }
            }
        }
        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }
    return s;
}

}  // namespace lci
