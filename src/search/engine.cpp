#include <lci/search/search_engine.h>
#include <lci/semantic/name_splitter.h>
#include <lci/search/symbol_type_alias.h>

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

/// Build a path-filter RE2 from include_pattern + exclude_pattern. nullptr
/// when both empty. Returned regexes are pre-compiled once per call.
/// A pattern that fails to compile sets `error` — the caller MUST surface
/// it instead of searching unfiltered (karpathy rule 6: a broken filter is
/// an error, never a silent superset).
struct PathFilter {
    std::unique_ptr<RE2> include;
    std::unique_ptr<RE2> exclude;
    std::string error;

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
        if (!pf.include->ok()) {
            pf.error = "invalid include_pattern regex '" +
                       opts.include_pattern + "': " + pf.include->error();
            pf.include.reset();
            return pf;
        }
    }
    if (!opts.exclude_pattern.empty()) {
        pf.exclude = std::make_unique<RE2>(opts.exclude_pattern, ro);
        if (!pf.exclude->ok()) {
            pf.error = "invalid exclude_pattern regex '" +
                       opts.exclude_pattern + "': " + pf.exclude->error();
            pf.exclude.reset();
            return pf;
        }
    }
    return pf;
}

/// Returns true if `actual` symbol-type matches any user-requested type.
/// Both sides go through canonical_symbol_type, so the aliases the MCP tool
/// description advertises (func, var, cls, ...) compare equal to the
/// SymbolType names they stand for. Comparing the raw strings, as this used
/// to, made every advertised alias filter all rows away.
/// Empty allow-list = accept all. `wanted` is expected to have been validated
/// by SearchEngine::search, so an unknown entry here cannot match anything.
bool symbol_type_matches_filter(const std::vector<std::string>& wanted,
                                std::string_view actual) {
    if (wanted.empty()) return true;
    auto actual_canonical = canonical_symbol_type(actual);
    if (actual_canonical.empty()) return false;
    for (const auto& w : wanted) {
        if (canonical_symbol_type(w) == actual_canonical) return true;
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
                                                 std::vector<SearchPatternMetadata>& metadata) {
    std::vector<std::string> out;
    metadata.clear();
    out.reserve(kMaxSynonymExpansion);

    // 1. Base set: original pattern first, then >2-char split words for
    //    multi-word queries (mirrors the no-synonym overload). Not synonyms.
    out.emplace_back(pattern);
    metadata.push_back({false, false});

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
                metadata.push_back({true, false});
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
                metadata.push_back({true, true});
            }
        }
    }
    return out;
}

bool identifier_contains_all_terms(
    std::string_view line, const std::vector<std::string>& terms) {
    if (terms.size() < 2) return false;

    static const NameSplitter splitter;
    size_t start = 0;
    while (start < line.size()) {
        while (start < line.size()) {
            const auto c = static_cast<unsigned char>(line[start]);
            if (std::isalnum(c) || c == '_') break;
            ++start;
        }
        size_t end = start;
        while (end < line.size()) {
            const auto c = static_cast<unsigned char>(line[end]);
            if (!std::isalnum(c) && c != '_') break;
            ++end;
        }
        if (end > start) {
            auto words = splitter.split_to_set(line.substr(start, end - start));
            const bool covers = std::all_of(
                terms.begin(), terms.end(), [&](const std::string& term) {
                    return words.contains(to_lower_copy(term));
                });
            if (covers) return true;
        }
        start = end + (end < line.size() ? 1 : 0);
    }
    return false;
}

static std::string_view line_at_1_based(std::string_view content, int line) {
    if (line < 1) return {};
    size_t start = 0;
    for (int current = 1; current < line; ++current) {
        start = content.find('\n', start);
        if (start == std::string_view::npos) return {};
        ++start;
    }
    size_t end = content.find('\n', start);
    if (end == std::string_view::npos) end = content.size();
    if (end > start && content[end - 1] == '\r') --end;
    return content.substr(start, end - start);
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

// Go's structure-mode categorizer (categorizeFile, codebase_intelligence_tools
// .go:813) buckets a file as Test when its lowercased path contains a "/test/"
// or "/tests/" directory SEGMENT, in addition to classify_file's basename
// markers. A bare ".py" under src/tests/ therefore counts as a test here even
// though the search classifier (engine.go classifyFile, basename-only) would
// call it code. "/testing/" has no "/test/" substring (needs the trailing
// slash), so it correctly stays out — matching Go's exact string set.
FileCategory categorize_file(std::string_view path) {
    if (text::ascii_contains_ci(path, "/test/") ||
        text::ascii_contains_ci(path, "/tests/")) {
        return FileCategory::Test;
    }
    return classify_file(path);
}

// -- SearchEngine -------------------------------------------------------------

SearchEngine::SearchEngine(MasterIndex& index, const SynonymTable& synonyms)
    : index_(index),
      synonyms_(synonyms),
      context_extractor_(index.file_content_store()) {}

// Root-relative view of an absolute indexed path. Returns `abs` unchanged
// when it does not live under `root`.
//
// The two sides reach here in different spellings on Windows: indexed paths
// are generic (the scanner stores generic_string()), while `root` carries
// whatever the platform handed us — config.project.root from a CLI arg, or a
// caller pasting "C:\repo\internal" back out of another tool. A byte-equal
// prefix compare then misses and every path stays absolute, which silently
// broke directory scopes, hidden-file detection and glob filters. Compare
// separator-insensitively so a native root still strips a generic path.
std::string_view relative_to_root(std::string_view abs, std::string_view root) {
    if (root.empty() || abs.size() <= root.size()) return abs;
    auto sep = [](char c) { return c == '/' || c == '\\'; };
    for (size_t i = 0; i < root.size(); ++i) {
        if (abs[i] == root[i]) continue;
        if (!sep(abs[i]) || !sep(root[i])) return abs;
    }
    if (!sep(abs[root.size()])) return abs;
    return abs.substr(root.size() + 1);
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

    if (pattern.empty() || pattern.size() > kMaxSearchPatternBytes) {
        // Fail fast with a distinguishable signal. An empty result vector on
        // its own is indistinguishable from "valid query, no matches", so the
        // rejection reason rides out on SearchStats.
        if (stats != nullptr) {
            stats->error = pattern.empty()
                ? "search pattern cannot be empty"
                : "search pattern too long (max " +
                      std::to_string(kMaxSearchPatternBytes) + " bytes)";
        }
        return {};
    }

    // Fail fast on an uncompilable regex. Without this the pattern is handed
    // to find_content_matches, which fails to compile it once per candidate
    // file and returns no matches -- the caller then cannot distinguish a
    // broken query from an absent one (Karpathy rule 6). Compiling here also
    // means the per-file cache only ever sees patterns known to be good.
    if (options.use_regex) {
        RE2::Options ro(RE2::Quiet);
        ro.set_log_errors(false);
        ro.set_case_sensitive(!options.case_insensitive);
        RE2 probe(pattern, ro);
        if (!probe.ok()) {
            if (stats != nullptr) {
                stats->error = "invalid regex: " + probe.error();
            }
            return {};
        }
    }

    // An unrecognized symbol type is a caller mistake, not an empty corpus.
    // Left unvalidated it filtered every row away and returned zero matches --
    // the same answer a correct query gives, so the caller could not tell the
    // two apart and the handler's hint blamed its pattern (Karpathy rule 6).
    for (const auto& t : options.symbol_types) {
        if (canonical_symbol_type(t).empty()) {
            if (stats != nullptr) {
                stats->error = "unknown symbol_type '" + t + "' (valid: " +
                               canonical_symbol_type_list() + ")";
            }
            return {};
        }
    }

    // Karpathy rule 2: build path-filter regexes once per call, not per file.
    auto path_filter = make_path_filter(options);
    if (!path_filter.error.empty()) {
        // A filter that cannot compile must not degrade into "no filter" —
        // that silently searches a superset of what the caller asked for.
        if (stats != nullptr) stats->error = path_filter.error;
        return {};
    }
    const std::string& proj_root = index_.config().project.root;
    const bool scope_is_glob =
        options.path_scope.find_first_of("*?") != std::string::npos;

    // Trigram candidate set is meaningful only for literal patterns. For
    // regex queries we must scan all indexed files because the literal
    // shortlist cannot represent character classes / anchors.
    std::vector<FileID> candidates;
    if (options.use_regex || options.invert_match) {
        // Invert reports the lines that do NOT match, so a file containing no
        // match at all contributes every one of its lines -- exactly the files
        // the trigram shortlist is designed to exclude. Scoping invert to the
        // candidate set would drop them silently (rg -v does not).
        candidates = index_.get_all_file_ids();
    } else {
        // find_candidate_files returns the certified scan set: every file
        // except those an index with coverage proves pattern-free. An empty
        // set with `informative` true means "certified nowhere" — returning
        // no results is correct, not a reason to scan everything.
        candidates = index_.find_candidate_files(pattern,
                                                  options.case_insensitive);
    }
    if (candidates.empty()) return {};

    // Deterministic scan order (Karpathy rule 4). Both sources above are
    // built by walking an absl hash map (TrigramIndex's per-file counts,
    // MasterIndex::file_map), whose iteration order is randomized per
    // process. That order decides WHICH matches survive the collection cap
    // below, so without this sort the ranked top-N of a capped query differs
    // between runs on an identical corpus.
    std::sort(candidates.begin(), candidates.end());

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
    auto file_snap = index_.load_snapshot();

    // Symbol-type filter runs INSIDE the collection loop (before the cap):
    // filtering after collection let non-matching results consume the
    // collection budget, silently starving matching files that sorted later.
    std::shared_ptr<const ReferenceTracker::Snapshot> type_filter_snap;
    if (!options.symbol_types.empty()) {
        type_filter_snap = index_.ref_tracker().pin();
    }

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
        size_t before = results.size();
        process_file(fid, pattern, options, effective_cap, results, *file_snap);
        if (type_filter_snap && results.size() > before) {
            results.erase(
                std::remove_if(
                    results.begin() +
                        static_cast<std::ptrdiff_t>(before),
                    results.end(),
                    [&](const SearchResult& r) {
                        auto sym = type_filter_snap->get_symbol_at_line(
                            r.file_id, r.line);
                        if (sym == nullptr) return true;
                        return !symbol_type_matches_filter(
                            options.symbol_types,
                            to_string(sym->symbol.type));
                    }),
                results.end());
        }
    }

    // Score and rank results. process_file has already seeded each row with
    // its match-quality bonus (word boundary / line start / exact case) --
    // the only place the match's byte range and the file's bytes are both in
    // hand -- so this ADDS the file-and-pattern component rather than
    // overwriting it. Overwriting is what made every hit for one pattern in
    // one file tie, leaving rank() to order them positionally.
    for (auto& r : results) {
        r.score += score_result(r, pattern);
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
    static const std::vector<SearchPatternMetadata> kNoMetadata;
    return search(patterns, kNoMetadata, options, stats);
}

std::vector<SearchResult> SearchEngine::search(
    const std::vector<std::string>& patterns,
    const std::vector<SearchPatternMetadata>& metadata,
    const SearchOptions& options, SearchStats* stats) const {

    if (patterns.empty()) return {};
    if (patterns.size() == 1) {
        if (!metadata.empty() && metadata[0].case_insensitive) {
            SearchOptions po = options;
            po.case_insensitive = true;
            auto rs = search(patterns[0], po, stats);
            for (auto& r : rs) r.from_synonym = metadata[0].synonym;
            return rs;
        }
        return search(patterns[0], options, stats);
    }

    struct ResultKey {
        FileID file_id;
        int line;
        bool operator==(const ResultKey& o) const {
            return file_id == o.file_id && line == o.line;
        }
    };
    struct ResultKeyHash {
        size_t operator()(const ResultKey& k) const {
            // FNV-ish mix; deterministic across runs.
            size_t h = std::hash<uint64_t>()(static_cast<uint64_t>(k.file_id));
            h ^= std::hash<int>()(k.line) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct Slot {
        SearchResult result;
        int pattern_count{1};
    };

    auto append_match = [](std::vector<std::string>& matches,
                           const std::string& match) {
        if (std::find(matches.begin(), matches.end(), match) == matches.end()) {
            matches.push_back(match);
        }
    };

    absl::flat_hash_map<ResultKey, Slot, ResultKeyHash> acc;
    if (options.max_results > 0) {
        acc.reserve(static_cast<size_t>(options.max_results) * patterns.size());
    }

    // Per-pattern search uses the SAME options (use_regex / filter / etc.).
    SearchOptions per_opts = options;
    // Multi-pattern coverage takes over scoring; do not double-cap per call.
    per_opts.max_results = options.max_results > 0 ? options.max_results * 4
                                                    : 0;

    bool any_sub_hit_cap = false;
    for (size_t i = 0; i < patterns.size(); ++i) {
        bool duplicate_pattern = false;
        for (size_t j = 0; j < i; ++j) {
            const auto left = i < metadata.size() ? metadata[i]
                                                  : SearchPatternMetadata{};
            const auto right = j < metadata.size() ? metadata[j]
                                                    : SearchPatternMetadata{};
            if (patterns[i] == patterns[j] &&
                left.case_insensitive == right.case_insensitive &&
                left.synonym == right.synonym) {
                duplicate_pattern = true;
                break;
            }
        }
        if (duplicate_pattern) continue;

        // Semantic split terms and synonyms use concept matching, so an
        // expanded `dialog` matches the identifier component in ExportDialog.
        SearchOptions p_opts = per_opts;
        if (i < metadata.size() && metadata[i].case_insensitive) {
            p_opts.case_insensitive = true;
        }
        SearchStats sub_stats;
        auto rs = search(patterns[i], p_opts,
                         stats != nullptr ? &sub_stats : nullptr);
        if (stats != nullptr) {
            if (sub_stats.hit_collection_cap ||
                sub_stats.total_found > static_cast<int>(rs.size())) {
                any_sub_hit_cap = true;
            }
            if (stats->error.empty() && !sub_stats.error.empty()) {
                stats->error = sub_stats.error;
            }
        }
        const bool is_synonym = i < metadata.size() && metadata[i].synonym;
        for (auto& r : rs) {
            r.from_synonym = is_synonym;
            append_match(r.match_texts, r.match_text);
            ResultKey k{r.file_id, r.line};
            auto it = acc.find(k);
            if (it == acc.end()) {
                acc.emplace(std::move(k), Slot{std::move(r), 1});
            } else {
                ++it->second.pattern_count;
                const bool all_synonyms =
                    it->second.result.from_synonym && r.from_synonym;
                if (r.score > it->second.result.score) {
                    auto matches = std::move(it->second.result.match_texts);
                    for (const auto& match : r.match_texts) {
                        append_match(matches, match);
                    }
                    it->second.result = std::move(r);
                    it->second.result.match_texts = std::move(matches);
                } else {
                    for (const auto& match : r.match_texts) {
                        append_match(it->second.result.match_texts, match);
                    }
                }
                // A row reached by BOTH an original and an expanded pattern is
                // an original-pattern hit: the caller's own word found it.
                it->second.result.from_synonym = all_synonyms;
            }
        }
    }

    // Coverage boost: +15% per extra match, cap +50%. Final score clamp ≤ 1.0
    // is intentionally NOT applied here — engine scores are not normalized to
    // [0,1] in C++ (kBaseMatchScore = 100). The boost is multiplicative on the
    // already-scored value, matching Go's relative behavior.
    std::vector<std::string> identifier_terms;
    for (size_t i = 0; i < patterns.size() && i < metadata.size(); ++i) {
        if (metadata[i].case_insensitive && !metadata[i].synonym &&
            patterns[i].find_first_of(" \t\r\n") == std::string::npos) {
            identifier_terms.push_back(patterns[i]);
        }
    }

    std::vector<SearchResult> out;
    out.reserve(acc.size());
    auto file_snapshot = identifier_terms.empty() ? nullptr : index_.load_snapshot();
    absl::flat_hash_map<FileID, std::string> reloaded_content;
    for (auto& [_, slot] : acc) {
        if (slot.pattern_count > 1) {
            double extra = static_cast<double>(slot.pattern_count - 1) *
                           kAdditionalPatternCoverageBoost;
            if (extra > kPatternCoverageBoostCap) {
                extra = kPatternCoverageBoostCap;
            }
            slot.result.score *= (1.0 + extra);
        }
        if (!identifier_terms.empty()) {
            auto line = index_.file_content_store().get_line_view(
                slot.result.file_id, slot.result.line - 1);
            if (line.empty()) {
                auto [it, inserted] = reloaded_content.try_emplace(
                    slot.result.file_id);
                if (inserted) {
                    it->second = index_.reload_evicted_content(
                        *file_snapshot, slot.result.file_id);
                }
                line = line_at_1_based(it->second, slot.result.line);
            }
            if (identifier_contains_all_terms(line, identifier_terms)) {
                slot.result.score *= (1.0 + kIdentifierCoverageBoost);
            }
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

    // kMaxMatchesPerFile applies ONLY when the caller set no explicit
    // per-file budget. The previous else-if chain re-applied the constant
    // whenever an explicit larger budget was not yet exceeded, silently
    // truncating dense files to 100 regardless of the caller's cap.
    {
        const int cap = options.max_count_per_file > 0
                            ? options.max_count_per_file
                            : kMaxMatchesPerFile;
        if (static_cast<int>(matches.size()) > cap) {
            matches.resize(static_cast<size_t>(cap));
        }
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
                                   std::string_view pattern) const {
    // kNonSymbolPenalty is unconditional: see its declaration. Search never
    // resolves a match to a symbol here, so the `has_symbol` parameter this
    // used to take was always false and the branch was dead.
    double score = kBaseMatchScore + kNonSymbolPenalty;
    score += score_file_type(result.path);
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
    // LRU-evicted-but-searchable: the file is still a trigram/postings
    // candidate but its bytes were evicted. Silently skipping it is a false
    // negative — reload into a request-local buffer (read path stays
    // lock-free; the store is not mutated).
    std::string reloaded;
    if (content_sv.empty()) {
        reloaded = index_.reload_evicted_content(snap, file_id);
        if (reloaded.empty()) return;
        content_sv = reloaded;
    }

    // Resolve the path once per file as a string_view into the pinned snapshot;
    // each match copies it into its own SearchResult::path. Previously this was
    // re-fetched (atomic load + map lookup + string copy) after every match.
    std::string_view path = index_.id_to_path(snap, file_id);

    if (options.exclude_tests && is_test_file(path)) return;

    // Resolved once per file, not per match: '#' means a comment in some
    // languages and a preprocessor directive in others, and the comment filter
    // below needs to tell them apart. language_info_for_path is a constexpr
    // table lookup on the extension, so this is a few byte compares.
    const LangId file_lang = language_info_for_path(path).language;

    // Per-file collection bounded by the remaining collection budget, not
    // the hidden kMaxMatchesPerFile constant (silent 100-per-file cap).
    SearchOptions scan_options = options;
    if (scan_options.max_count_per_file <= 0 && effective_cap > 0) {
        scan_options.max_count_per_file =
            effective_cap - static_cast<int>(results.size());
    }
    auto matches = find_matches(content_sv, pattern, scan_options);
    // Invert reports non-matching lines, so a file with zero matches is the
    // most productive input there is -- it contributes all of its lines.
    if (matches.empty() && !options.invert_match) return;

    // Block-aware context is not yet wired; context_extractor falls back to
    // line-window extraction with an empty block list. (Previously this fetched
    // symbol_location_index().get_file_symbols(file_id) — a full per-file Symbol
    // vector copy on the hot path — and discarded it.)
    std::vector<BlockBoundary> blocks;

    // Comment-exclusion needs block-comment state the per-line predicate lacks:
    // a continuation line (" * prose") is indistinguishable from a continued
    // expression ("* stats.confidence);") on its own. Scan the file once (only
    // when the flag is on) to get the byte spans of every /* */ block; both
    // filter loops then classify a line by containment with a monotonic cursor,
    // O(lines + spans) and no per-line allocation (Karpathy rule 2).
    std::vector<CommentByteRange> comment_spans;
    if (options.exclude_comments) {
        comment_spans = scan_block_comment_ranges(content_sv, file_lang);
    }

    // Deduplicate by line within this file.
    absl::flat_hash_set<int> seen_lines;

    // Monotonic cursor into comment_spans for the match loop (lines are
    // visited in ascending order via cursor_line_start).
    size_t comment_span_pos = 0;

    // Incremental line cursor. find_matches emits matches with strictly
    // ascending start offsets, so the line number and line start of the next
    // match are always at or after the previous one. Resolving each match
    // with search_line_number + search_line_start restarted the scan at byte
    // 0 twice per match -- O(file_size x matches) per file, which on a file
    // with many hits dominated the whole query. Walking forward once makes it
    // O(file_size) per file. Emitted line/column values are unchanged: both
    // helpers define line as 1 + newlines strictly before the offset and line
    // start as the byte after the last preceding newline, which is exactly
    // what the cursor accumulates.
    int cursor = 0;
    int cursor_line = 1;
    int cursor_line_start = 0;
    const int content_len = static_cast<int>(content_sv.size());

    // Returns the text of the line starting at `line_start`, newline trimmed.
    auto line_text_at = [&](int line_start) -> std::string_view {
        int end = line_start;
        while (end < content_len && content_sv[static_cast<size_t>(end)] != '\n') {
            ++end;
        }
        if (end > line_start &&
            content_sv[static_cast<size_t>(end - 1)] == '\r') {
            --end;
        }
        return content_sv.substr(static_cast<size_t>(line_start),
                                 static_cast<size_t>(end - line_start));
    };

    if (options.invert_match) {
        // Mark every line carrying a match, then emit the rest. One forward
        // pass each, no per-line allocation (Karpathy rule 2).
        absl::flat_hash_set<int> matching_lines;
        matching_lines.reserve(matches.size());
        for (const auto& match : matches) {
            int ms = match.start < 0 ? 0 : match.start;
            if (ms > content_len) ms = content_len;
            for (; cursor < ms; ++cursor) {
                if (content_sv[static_cast<size_t>(cursor)] == '\n') {
                    ++cursor_line;
                }
            }
            matching_lines.insert(cursor_line);
        }

        int line_no = 1;
        int line_start = 0;
        size_t span_pos = 0;
        for (int i = 0; i <= content_len; ++i) {
            bool at_end = (i == content_len);
            if (!at_end && content_sv[static_cast<size_t>(i)] != '\n') continue;
            if (at_end && i == line_start) break;  // no trailing partial line

            if (effective_cap > 0 &&
                static_cast<int>(results.size()) >= effective_cap) {
                return;
            }
            if (!matching_lines.contains(line_no)) {
                auto text = line_text_at(line_start);
                if (!options.exclude_comments ||
                    !line_is_comment_only_with_spans(
                        content_sv, line_start, i, file_lang, comment_spans,
                        span_pos)) {
                    SearchContext ctx;
                    if (options.max_context_lines > 0) {
                        ctx = context_extractor_.extract(
                            file_id, blocks, line_no,
                            options.max_context_lines, content_sv);
                    }
                    results.push_back(SearchResult{
                        file_id, std::string(path), line_no, 0,
                        std::string(text), 0.0, std::move(ctx)});
                }
            }
            ++line_no;
            line_start = i + 1;
        }
        return;
    }

    for (const auto& match : matches) {
        if (effective_cap > 0 &&
            static_cast<int>(results.size()) >= effective_cap) {
            break;
        }

        int match_start = match.start < 0 ? 0 : match.start;
        if (match_start > content_len) match_start = content_len;
        for (; cursor < match_start; ++cursor) {
            if (content_sv[static_cast<size_t>(cursor)] == '\n') {
                ++cursor_line;
                cursor_line_start = cursor + 1;
            }
        }
        int line = cursor_line;

        if (seen_lines.contains(line)) continue;
        seen_lines.insert(line);

        // A comment-only line is dropped; a code line with a TRAILING comment
        // is kept, matching the CLI's rule. Block-comment continuation lines
        // need the enclosing span, so classify with the per-file block spans.
        if (options.exclude_comments) {
            const int line_end =
                search_line_end(content_sv, cursor_line_start);
            if (line_is_comment_only_with_spans(
                    content_sv, cursor_line_start, line_end, file_lang,
                    comment_spans, comment_span_pos)) {
                continue;
            }
        }

        int col = match_start - cursor_line_start;

        std::string match_text;
        if (match.end > match.start &&
            match.end <= static_cast<int>(content_sv.size())) {
            match_text = std::string(
                content_sv.substr(static_cast<size_t>(match.start),
                                  static_cast<size_t>(match.end - match.start)));
        }

        SearchContext ctx;
        if (options.max_context_lines > 0) {
            // Hand over the bytes we already hold. Re-resolving by FileID
            // costs a second store read, and for a file reloaded above after
            // LRU eviction the store has nothing to return -- the row would
            // carry a match with an empty context block.
            ctx = context_extractor_.extract(file_id, blocks, line,
                                              options.max_context_lines,
                                              content_sv);
        }

        // Match-quality bonus, seeded here because this is the only point
        // where the match offsets and the file bytes are both available.
        // kBaseMatchScore is subtracted out: score_result contributes it once,
        // in SearchEngine::search.
        double quality = calculate_match_quality(content_sv, match.start,
                                                 match.end, pattern) -
                         kBaseMatchScore;

        results.push_back(SearchResult{
            file_id, std::string(path), line, col,
            std::move(match_text), quality, std::move(ctx)});
    }
}

}  // namespace lci
