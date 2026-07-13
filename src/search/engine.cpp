#include <lci/search/search_engine.h>

#include <lci/core/reference_tracker.h>
#include <lci/core/text.h>
#include <lci/language_map.h>
#include <lci/indexing/master_index.h>
#include <lci/indexing/pipeline_scanner.h>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <re2/re2.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>

namespace lci {

// -- Rich-search helpers ------------------------------------------------------
//
// All hot-path-safe: no allocation in inner loops, RE2 not std::regex, no
// per-call mutex. Karpathy rule 6 (fail fast): regex compile failure surfaces
// to the caller via empty-results, not silent fallback to literal — caller
// guards by validating SearchOptions.use_regex upstream.

namespace {

/// Returns true if pattern contains regex-suggestive syntax (Go
/// looksLikeRegex parity, handlers.go:86). Used by the MCP handler to opt
/// into a regex-fallback pass at reduced score. Pure (no allocation).
bool looks_like_regex_impl(std::string_view p) {
    if (p.empty()) return false;
    if (p.find('|') != std::string_view::npos) return true;
    if (p.find('[') != std::string_view::npos &&
        p.find(']') != std::string_view::npos) return true;
    if (p.front() == '^' || p.back() == '$') return true;
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        char ch = p[i];
        char next = p[i + 1];
        if (ch == '\\') {
            switch (next) {
                case 'd': case 'w': case 's': case 'b':
                case 'D': case 'W': case 'S': case 'B':
                case '.': case '*': case '+': case '?':
                case '(': case ')': case '[': case ']':
                case '{': case '}': case '^': case '$':
                case '|': case '\\':
                    return true;
            }
        }
        if (ch == '.' && (next == '+' || next == '*' || next == '?')) {
            return true;
        }
        if (ch == '(' && next == '?') return true;
    }
    int depth = 0;
    for (char c : p) {
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == '|' && depth > 0) return true;
    }
    if (p.find('{') != std::string_view::npos &&
        p.find('}') != std::string_view::npos) {
        for (size_t i = 0; i + 2 < p.size(); ++i) {
            if (p[i] != '{') continue;
            size_t j = i + 1;
            while (j < p.size() && p[j] >= '0' && p[j] <= '9') ++j;
            if (j > i + 1 && j < p.size() && (p[j] == ',' || p[j] == '}')) {
                return true;
            }
        }
    }
    return false;
}

/// Lowercase a path for RE2 path-filter compile (RE2 has no PCRE-style /i).
/// Used only at compile time on the filter strings — not on every line.
void lower_inplace(std::string& s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Build a path-filter RE2 from include_pattern + exclude_pattern. nullptr
/// when both empty. Returned regexes are pre-compiled once per call.
struct PathFilter {
    std::unique_ptr<RE2> include;
    std::unique_ptr<RE2> exclude;

    bool matches(std::string_view path) const {
        if (include && !RE2::PartialMatch(path, *include)) return false;
        if (exclude && RE2::PartialMatch(path, *exclude)) return false;
        return true;
    }
};

PathFilter make_path_filter(const SearchOptions& opts) {
    PathFilter pf;
    RE2::Options ro(RE2::Quiet);
    ro.set_log_errors(false);
    if (!opts.include_pattern.empty()) {
        pf.include = std::make_unique<RE2>(opts.include_pattern, ro);
        if (!pf.include->ok()) pf.include.reset();
    }
    if (!opts.exclude_pattern.empty()) {
        pf.exclude = std::make_unique<RE2>(opts.exclude_pattern, ro);
        if (!pf.exclude->ok()) pf.exclude.reset();
    }
    return pf;
}

/// Returns true if `enclosing` symbol-type matches any user-requested type.
/// Type-string match (lowercase). Empty allow-list = accept all.
bool symbol_type_matches_filter(const std::vector<std::string>& wanted,
                                std::string_view actual) {
    if (wanted.empty()) return true;
    for (const auto& w : wanted) {
        if (w.size() != actual.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < w.size(); ++i) {
            char a = static_cast<char>(std::tolower(static_cast<unsigned char>(w[i])));
            char b = static_cast<char>(std::tolower(static_cast<unsigned char>(actual[i])));
            if (a != b) { eq = false; break; }
        }
        if (eq) return true;
    }
    return false;
}

}  // namespace

bool looks_like_regex(std::string_view pattern) {
    return looks_like_regex_impl(pattern);
}

void split_on_spaces(std::string_view input, std::vector<std::string>& out) {
    size_t i = 0;
    while (i < input.size()) {
        while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i]))) ++i;
        size_t start = i;
        while (i < input.size() && !std::isspace(static_cast<unsigned char>(input[i]))) ++i;
        if (i > start) out.emplace_back(input.substr(start, i - start));
    }
}

std::vector<std::string> expand_pattern_semantic(std::string_view pattern) {
    // Mirror Go performSemanticExpansion's word-split component
    // (handlers.go:1271). Original pattern first (preserves score priority),
    // then >2-char unique words.
    std::vector<std::string> out;
    out.reserve(8);
    out.emplace_back(pattern);
    std::vector<std::string> words;
    split_on_spaces(pattern, words);
    if (words.size() <= 1) return out;
    absl::flat_hash_set<std::string> seen;
    seen.insert(out.front());
    for (auto& w : words) {
        if (w.size() <= 2) continue;
        if (seen.insert(w).second) {
            out.emplace_back(std::move(w));
        }
    }
    return out;
}

namespace {

std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

}  // namespace

const SynonymTable& default_synonym_table() {
    static const SynonymTable table = SynonymTable::build_default();
    return table;
}

std::vector<std::string> expand_pattern_semantic(std::string_view pattern,
                                                 const SynonymTable& table,
                                                 std::vector<bool>& synonym_flags) {
    std::vector<std::string> out;
    synonym_flags.clear();
    out.reserve(kMaxSynonymExpansion);

    // 1. Base set: original pattern first, then >2-char split words for
    //    multi-word queries (mirrors the no-synonym overload). Not synonyms.
    out.emplace_back(pattern);
    synonym_flags.push_back(false);

    std::vector<std::string> words;
    split_on_spaces(pattern, words);

    absl::flat_hash_set<std::string> seen;
    seen.insert(out.front());

    // Words whose synonyms we look up: the bare word for a single-word query,
    // else each retained >2-char split word.
    std::vector<std::string> retained;
    if (words.size() <= 1) {
        if (!words.empty()) retained.push_back(std::move(words.front()));
    } else {
        for (auto& w : words) {
            if (w.size() <= 2) continue;
            if (seen.insert(w).second) {
                out.push_back(w);
                synonym_flags.push_back(false);
                retained.push_back(std::move(w));
            }
        }
    }

    if (table.empty()) return out;

    // 2. Append synonyms of each retained word, deduped, bounded by the cap.
    //    Synonyms are lowercase word-concepts; flagged so the engine matches
    //    them case-insensitively.
    for (const auto& w : retained) {
        if (out.size() >= kMaxSynonymExpansion) break;
        std::string lw = to_lower_copy(w);
        for (const auto& syn : table.synonyms_of(lw)) {
            if (out.size() >= kMaxSynonymExpansion) break;
            if (seen.insert(syn).second) {
                out.push_back(syn);
                synonym_flags.push_back(true);
            }
        }
    }
    return out;
}


// -- File classification ------------------------------------------------------

namespace {

bool has_extension(std::string_view path, std::string_view ext) {
    if (path.size() < ext.size()) return false;
    auto suffix = path.substr(path.size() - ext.size());
    for (size_t i = 0; i < ext.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(suffix[i])) !=
            std::tolower(static_cast<unsigned char>(ext[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view file_base(std::string_view path) {
    auto slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        slash = path.rfind('\\');
    }
    if (slash == std::string_view::npos) return path;
    return path.substr(slash + 1);
}

bool is_code_extension(std::string_view ext) {
    // Single source of truth: the centralized extension table (language_map.h).
    return language_info(ext).is_code;
}

bool is_doc_extension(std::string_view ext) {
    static constexpr std::string_view exts[] = {
        ".md", ".markdown", ".txt", ".rst", ".adoc",
        ".asciidoc", ".rdoc", ".org", ".wiki", ".textile",
    };
    for (auto e : exts) {
        if (has_extension(ext, e)) return true;
    }
    return false;
}

bool is_config_extension(std::string_view ext) {
    static constexpr std::string_view exts[] = {
        ".json", ".yaml", ".yml", ".toml", ".ini",
        ".cfg", ".conf", ".xml", ".kdl", ".env", ".properties",
    };
    for (auto e : exts) {
        if (has_extension(ext, e)) return true;
    }
    return false;
}

}  // namespace

std::string_view file_extension(std::string_view path) {
    // Match Go filepath.Ext: the extension is the suffix beginning at the final
    // dot in the FINAL path element. Strip the directory first so a dot in a
    // parent directory name (e.g. "dir.v1/Makefile") cannot leak in.
    auto base = file_base(path);
    auto dot = base.rfind('.');
    if (dot == std::string_view::npos) return {};
    return base.substr(dot);
}

FileCategory classify_file(std::string_view path) {
    auto base = file_base(path);

    if (text::ascii_contains_ci(base, "_test.") || text::ascii_contains_ci(base, ".test.") ||
        text::ascii_contains_ci(base, ".spec.") ||
        (base.size() >= 5 && text::ascii_contains_ci(base.substr(0, 5), "test_"))) {
        return FileCategory::Test;
    }

    auto ext = file_extension(path);
    if (is_code_extension(ext)) return FileCategory::Code;
    if (is_doc_extension(ext)) return FileCategory::Documentation;
    if (is_config_extension(ext)) return FileCategory::Config;
    return FileCategory::Unknown;
}

double score_file_type(std::string_view path) {
    auto cat = classify_file(path);
    switch (cat) {
        case FileCategory::Code: return kCodeFileBoost;
        case FileCategory::Documentation: return kDocFilePenalty;
        case FileCategory::Config: return kConfigFileBoost;
        case FileCategory::Test: return kCodeFileBoost * 0.8;
        case FileCategory::Unknown: return 0.0;
    }
    return 0.0;
}

bool is_test_file(std::string_view path) {
    return classify_file(path) == FileCategory::Test;
}

// -- SearchEngine -------------------------------------------------------------

SearchEngine::SearchEngine(MasterIndex& index, const SynonymTable& synonyms)
    : index_(index),
      synonyms_(synonyms),
      context_extractor_(index.file_content_store()) {}

// Root-relative view of an absolute indexed path. Returns `abs` unchanged
// when it does not live under `root`.
std::string_view relative_to_root(std::string_view abs, std::string_view root) {
    if (!root.empty() && abs.size() > root.size() &&
        abs.substr(0, root.size()) == root && abs[root.size()] == '/') {
        return abs.substr(root.size() + 1);
    }
    return abs;
}

namespace {

/// True when the root-relative path falls inside the requested scope.
/// Non-glob scope = directory prefix; glob scope = FileScanner::match_glob.
bool path_in_scope(std::string_view rel, const std::string& scope,
                   bool scope_is_glob) {
    if (scope_is_glob) return FileScanner::match_glob(scope, rel);
    if (rel.size() == scope.size()) return rel == scope;
    return rel.size() > scope.size() &&
           rel.substr(0, scope.size()) == scope && rel[scope.size()] == '/';
}

/// Builds SearchStats.dir_counts: top-level-dir histogram over the full
/// pre-truncation result set. Deterministic order: count desc, then name.
void fill_dir_counts(const std::vector<SearchResult>& results,
                     std::string_view root, SearchStats& stats) {
    absl::flat_hash_map<std::string, int> counts;
    for (const auto& r : results) {
        auto rel = relative_to_root(r.path, root);
        auto slash = rel.find('/');
        std::string_view dir = slash == std::string_view::npos
                                   ? std::string_view(".")
                                   : rel.substr(0, slash);
        ++counts[std::string(dir)];
    }
    stats.dir_counts.assign(counts.begin(), counts.end());
    std::sort(stats.dir_counts.begin(), stats.dir_counts.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
}

}  // namespace

std::vector<SearchResult> SearchEngine::search(
    const std::string& pattern, const SearchOptions& options,
    SearchStats* stats) const {

    if (pattern.empty() || pattern.size() > 1000) return {};

    // Karpathy rule 2: build path-filter regexes once per call, not per file.
    auto path_filter = make_path_filter(options);
    const std::string& proj_root = index_.config().project.root;
    const bool scope_is_glob =
        options.path_scope.find_first_of("*?") != std::string::npos;

    // Trigram candidate set is meaningful only for literal patterns. For
    // regex queries we must scan all indexed files because the literal
    // shortlist cannot represent character classes / anchors.
    std::vector<FileID> candidates;
    if (options.use_regex) {
        candidates = index_.get_all_file_ids();
    } else {
        candidates = index_.find_candidate_files(pattern,
                                                  options.case_insensitive);
        if (candidates.empty()) {
            candidates = index_.get_all_file_ids();
        }
    }
    if (candidates.empty()) return {};

    // Output cap (what the caller asked for) vs collection cap (how many raw
    // matches we gather before scoring). They must differ: if we stop
    // collecting at the output cap, the kept set is the first-N matches in
    // candidate-file order and the later score+rank only reorders those N — so
    // on doc-heavy repos the docs/ files fill the cap before code files are
    // reached and code matches are never collected (measured: fastapi
    // "APIRouter" -> 96/100 markdown, 3/100 code). Over-collect, then rank,
    // then truncate, so high-value matches (code scores ~2x prose) win the cap.
    int output_cap = options.max_results;
    int effective_cap = options.max_results;
    if (effective_cap <= 0) {
        effective_cap = (static_cast<int>(candidates.size()) >= 400) ? 25 : 0;
        output_cap = effective_cap;
    } else {
        effective_cap = std::min(effective_cap * 8, 2000);
    }

    std::vector<SearchResult> results;

    // Pin the file snapshot once for the whole query: path resolution below
    // (filter + per-file path) reads a string_view into it, no per-call atomic
    // load or string copy.
    auto file_snap = index_.read_snapshot();

    bool hit_collection_cap = false;
    for (FileID fid : candidates) {
        if (effective_cap > 0 &&
            static_cast<int>(results.size()) >= effective_cap) {
            hit_collection_cap = true;
            break;
        }
        // Path scope (`path` param): root-relative prefix or glob.
        if (!options.path_scope.empty() || !options.filter_globs.empty()) {
            auto rel = relative_to_root(index_.id_to_path(*file_snap, fid),
                                        proj_root);
            if (!options.path_scope.empty() &&
                !path_in_scope(rel, options.path_scope, scope_is_glob)) {
                continue;
            }
            // Include filter (`filter` param): any-glob match survives.
            if (!options.filter_globs.empty()) {
                bool any = false;
                for (const auto& g : options.filter_globs) {
                    if (FileScanner::match_glob(g, rel)) { any = true; break; }
                }
                if (!any) continue;
            }
        }
        // Path filter (languages/filter). Cheap per-file string scan.
        if (path_filter.include || path_filter.exclude) {
            if (!path_filter.matches(index_.id_to_path(*file_snap, fid))) {
                continue;
            }
        }
        process_file(fid, pattern, options, effective_cap, results, *file_snap);
    }

    // Symbol-type filter — apply after match, before scoring.
    if (!options.symbol_types.empty()) {
        auto& tracker = index_.ref_tracker();
        auto rt_snap = tracker.pin();
        results.erase(std::remove_if(results.begin(), results.end(),
            [&](const SearchResult& r) {
                auto sym =
                    rt_snap->get_symbol_at_line(r.file_id, r.line);
                if (sym == nullptr) return true;
                return !symbol_type_matches_filter(options.symbol_types,
                                                   to_string(sym->symbol.type));
            }), results.end());
    }

    // Score and rank results.
    for (auto& r : results) {
        r.score = score_result(r, pattern, false);
    }

    SearchCoordinator::rank(results);

    // Record the TRUE universe before truncation — the handler reports this
    // instead of the old total==max cap-saturation lie.
    if (stats != nullptr) {
        stats->total_found = static_cast<int>(results.size());
        stats->hit_collection_cap = hit_collection_cap;
        fill_dir_counts(results, proj_root, *stats);
    }

    // Truncate to the requested cap AFTER ranking, so the returned set is the
    // top-scored matches across all candidates (not the first-found).
    if (output_cap > 0 && static_cast<int>(results.size()) > output_cap) {
        results.resize(static_cast<size_t>(output_cap));
    }
    return results;
}

// Multi-pattern OR-merge with per-result coverage tracking. Mirrors Go's
// searchAndDeduplicate (handlers.go:1372). Karpathy rule 2: results map
// pre-reserved; we move rather than copy results into the accumulator.
std::vector<SearchResult> SearchEngine::search(
    const std::vector<std::string>& patterns,
    const SearchOptions& options, SearchStats* stats) const {
    static const std::vector<bool> kNoFlags;
    return search(patterns, kNoFlags, options, stats);
}

std::vector<SearchResult> SearchEngine::search(
    const std::vector<std::string>& patterns,
    const std::vector<bool>& synonym_flags,
    const SearchOptions& options, SearchStats* stats) const {

    if (patterns.empty()) return {};
    if (patterns.size() == 1) {
        if (!synonym_flags.empty() && synonym_flags[0]) {
            SearchOptions po = options;
            po.case_insensitive = true;
            return search(patterns[0], po, stats);
        }
        return search(patterns[0], options, stats);
    }

    struct ResultKey {
        FileID file_id;
        int line;
        std::string match;
        bool operator==(const ResultKey& o) const {
            return file_id == o.file_id && line == o.line && match == o.match;
        }
    };
    struct ResultKeyHash {
        size_t operator()(const ResultKey& k) const {
            // FNV-ish mix; deterministic across runs.
            size_t h = std::hash<uint64_t>()(static_cast<uint64_t>(k.file_id));
            h ^= std::hash<int>()(k.line) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<std::string>()(k.match) + 0x9e3779b97f4a7c15ULL +
                 (h << 6) + (h >> 2);
            return h;
        }
    };

    struct Slot {
        SearchResult result;
        int pattern_count{1};
    };

    absl::flat_hash_map<ResultKey, Slot, ResultKeyHash> acc;
    acc.reserve(static_cast<size_t>(options.max_results) * patterns.size());

    // Per-pattern search uses the SAME options (use_regex / filter / etc.).
    SearchOptions per_opts = options;
    // Multi-pattern coverage takes over scoring; do not double-cap per call.
    per_opts.max_results = options.max_results > 0 ? options.max_results * 4
                                                    : 0;

    bool any_sub_hit_cap = false;
    for (size_t i = 0; i < patterns.size(); ++i) {
        // Synonym-injected patterns force case-insensitive so an expanded
        // `signin` matches code `signIn` regardless of the base query's flag.
        SearchOptions p_opts = per_opts;
        if (i < synonym_flags.size() && synonym_flags[i]) {
            p_opts.case_insensitive = true;
        }
        SearchStats sub_stats;
        auto rs = search(patterns[i], p_opts,
                         stats != nullptr ? &sub_stats : nullptr);
        if (stats != nullptr &&
            (sub_stats.hit_collection_cap ||
             sub_stats.total_found > static_cast<int>(rs.size()))) {
            any_sub_hit_cap = true;
        }
        for (auto& r : rs) {
            ResultKey k{r.file_id, r.line, r.match_text};
            auto it = acc.find(k);
            if (it == acc.end()) {
                acc.emplace(std::move(k), Slot{std::move(r), 1});
            } else {
                ++it->second.pattern_count;
                if (r.score > it->second.result.score) {
                    it->second.result.score = r.score;
                }
            }
        }
    }

    // Coverage boost: +15% per extra match, cap +50%. Final score clamp ≤ 1.0
    // is intentionally NOT applied here — engine scores are not normalized to
    // [0,1] in C++ (kBaseMatchScore = 100). The boost is multiplicative on the
    // already-scored value, matching Go's relative behavior.
    constexpr double kCoveragePerWord = 0.15;
    constexpr double kCoverageCap = 0.5;

    std::vector<SearchResult> out;
    out.reserve(acc.size());
    for (auto& [_, slot] : acc) {
        if (slot.pattern_count > 1) {
            double extra = static_cast<double>(slot.pattern_count - 1) *
                           kCoveragePerWord;
            if (extra > kCoverageCap) extra = kCoverageCap;
            slot.result.score *= (1.0 + extra);
        }
        out.emplace_back(std::move(slot.result));
    }

    SearchCoordinator::rank(out);

    if (stats != nullptr) {
        stats->total_found = static_cast<int>(out.size());
        stats->hit_collection_cap = any_sub_hit_cap;
        fill_dir_counts(out, index_.config().project.root, *stats);
    }

    if (options.max_results > 0 &&
        static_cast<int>(out.size()) > options.max_results) {
        out.resize(static_cast<size_t>(options.max_results));
    }
    return out;
}

// Shared literal/regex content matcher. Single source of truth for both the
// SearchEngine::find_matches read path and MasterIndex::execute_search (which
// previously hand-rolled an O(content×pattern) duplicate). thread_local RE2
// cache + lowercase buffers keep it allocation-free across files in a scan.
std::vector<SearchMatch> find_content_matches(
    std::string_view content,
    std::string_view pattern,
    const SearchOptions& options) {

    std::vector<SearchMatch> matches;
    if (pattern.empty() || content.empty()) return matches;

    // Regex path uses RE2 (Karpathy rule: no std::regex). RE2 is compiled per
    // call here — for hot multi-file scans a future iteration can lift the
    // compile up to the SearchEngine::search caller. For now we follow Go's
    // shape: one match pass per file, compile once per pass via thread_local
    // cache keyed on (pattern, ci) to skip redundant compiles when the same
    // pattern is reused across files in the candidate loop.
    if (options.use_regex) {
        thread_local std::string cached_key;
        thread_local std::unique_ptr<RE2> cached_re;
        std::string key;
        key.reserve(pattern.size() + 3);
        key.append(pattern);
        key.push_back('|');
        key.push_back(options.case_insensitive ? 'i' : 's');
        if (key != cached_key || !cached_re || !cached_re->ok()) {
            RE2::Options ro(RE2::Quiet);
            ro.set_log_errors(false);
            ro.set_case_sensitive(!options.case_insensitive);
            cached_re = std::make_unique<RE2>(pattern, ro);
            cached_key = std::move(key);
        }
        if (!cached_re->ok()) {
            // Fail fast on bad regex — caller sees empty results plus an
            // error surfaced upstream by validation. No silent fallback.
            return matches;
        }
        re2::StringPiece input(content.data(), content.size());
        re2::StringPiece m;
        size_t cursor = 0;
        while (cursor <= content.size() &&
               cached_re->Match(input, cursor, content.size(),
                                RE2::UNANCHORED, &m, 1)) {
            int start = static_cast<int>(m.data() - content.data());
            int end = start + static_cast<int>(m.size());
            if (end <= start) {
                // Zero-width match — advance one byte to avoid infinite loop.
                cursor = static_cast<size_t>(start) + 1;
                continue;
            }
            if (options.word_boundary) {
                if (!is_word_boundary(content, start) ||
                    !is_word_boundary(content, end)) {
                    cursor = static_cast<size_t>(start) + 1;
                    continue;
                }
            }
            bool exact = is_word_boundary(content, start) &&
                         is_word_boundary(content, end);
            matches.push_back({start, end, exact});
            cursor = static_cast<size_t>(end);
        }

        int cap = options.max_count_per_file > 0
                      ? options.max_count_per_file
                      : kMaxMatchesPerFile;
        if (static_cast<int>(matches.size()) > cap) {
            matches.resize(static_cast<size_t>(cap));
        }
        return matches;
    }

    // Per-query lowercase buffers are thread_local so case-insensitive
    // searches don't alloc per call (Karpathy rule 2 — no allocation in
    // inner loops). resize() shrinks when the next call's content is
    // smaller without freeing capacity.
    thread_local std::string lower_content;
    thread_local std::string lower_pattern;
    std::string_view search_content = content;
    std::string_view search_pattern = pattern;

    if (options.case_insensitive) {
        lower_content.resize(content.size());
        for (size_t i = 0; i < content.size(); ++i) {
            lower_content[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(content[i])));
        }
        lower_pattern.resize(pattern.size());
        for (size_t i = 0; i < pattern.size(); ++i) {
            lower_pattern[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(pattern[i])));
        }
        search_content = lower_content;
        search_pattern = lower_pattern;
    }

    size_t offset = 0;
    while (offset < search_content.size()) {
        auto found = search_content.find(search_pattern, offset);
        if (found == std::string_view::npos) break;

        int start = static_cast<int>(found);
        int end = start + static_cast<int>(pattern.size());

        if (options.word_boundary) {
            if (!is_word_boundary(content, start) ||
                !is_word_boundary(content, end)) {
                offset = found + 1;
                continue;
            }
        }

        bool exact = is_word_boundary(content, start) &&
                     is_word_boundary(content, end);

        matches.push_back({start, end, exact});
        offset = found + 1;
    }

    if (options.max_count_per_file > 0 &&
        static_cast<int>(matches.size()) > options.max_count_per_file) {
        matches.resize(static_cast<size_t>(options.max_count_per_file));
    } else if (static_cast<int>(matches.size()) > kMaxMatchesPerFile) {
        matches.resize(kMaxMatchesPerFile);
    }

    return matches;
}

std::vector<SearchMatch> SearchEngine::find_matches(
    std::string_view content,
    std::string_view pattern,
    const SearchOptions& options) const {
    return find_content_matches(content, pattern, options);
}

double SearchEngine::score_result(const SearchResult& result,
                                   std::string_view pattern,
                                   bool has_symbol) const {
    double score = kBaseMatchScore;
    score += score_file_type(result.path);

    if (!has_symbol) {
        score += kNonSymbolPenalty;
    }

    score += static_cast<double>(calculate_pattern_complexity(pattern)) * 0.5;
    return score;
}

void SearchEngine::process_file(
    FileID file_id,
    std::string_view pattern,
    const SearchOptions& options,
    int effective_cap,
    std::vector<SearchResult>& results,
    const FileSnapshot& snap) const {

    auto content_sv = index_.file_content_store().get_content(file_id);
    if (content_sv.empty()) return;

    // Resolve the path once per file as a string_view into the pinned snapshot;
    // each match copies it into its own SearchResult::path. Previously this was
    // re-fetched (atomic load + map lookup + string copy) after every match.
    std::string_view path = index_.id_to_path(snap, file_id);

    if (options.exclude_tests && is_test_file(path)) return;

    auto matches = find_matches(content_sv, pattern, options);
    if (matches.empty()) return;

    // Block-aware context is not yet wired; context_extractor falls back to
    // line-window extraction with an empty block list. (Previously this fetched
    // symbol_location_index().get_file_symbols(file_id) — a full per-file Symbol
    // vector copy on the hot path — and discarded it.)
    std::vector<BlockBoundary> blocks;

    // Deduplicate by line within this file.
    absl::flat_hash_set<int> seen_lines;

    for (const auto& match : matches) {
        if (effective_cap > 0 &&
            static_cast<int>(results.size()) >= effective_cap) {
            break;
        }

        int line = search_line_number(content_sv, match.start);

        if (seen_lines.contains(line)) continue;
        seen_lines.insert(line);

        int col = match.start - search_line_start(content_sv, match.start);

        std::string match_text;
        if (match.end > match.start &&
            match.end <= static_cast<int>(content_sv.size())) {
            match_text = std::string(
                content_sv.substr(static_cast<size_t>(match.start),
                                  static_cast<size_t>(match.end - match.start)));
        }

        SearchContext ctx;
        if (options.max_context_lines > 0) {
            ctx = context_extractor_.extract(file_id, blocks, line,
                                              options.max_context_lines);
        }

        results.push_back(SearchResult{
            file_id, std::string(path), line, col,
            std::move(match_text), 0.0, std::move(ctx)});
    }
}

}  // namespace lci
