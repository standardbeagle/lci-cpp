#include <lci/indexing/master_index.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace lci {

namespace {

// -- Path-scope filtering (CLI trailing-path args) ----------------------------
//
// Mirrors the non-glob semantics of SearchEngine's file-local `path_in_scope`
// (src/search/engine.cpp) without lifting that anonymous-namespace helper out
// of an out-of-file scope: a scope entry matches a root-relative path when the
// path equals it (exact file) or is nested under it (directory prefix). Kept
// deliberately duplicated here — the CLI `lci grep/search <path>...` positional
// only ever passes real files or directory prefixes, never globs, so the glob
// branch is unneeded.

/// Normalizes a raw scope token: strips a leading "./" and any trailing '/'.
std::string_view normalize_scope(std::string_view scope) {
    if (scope.size() >= 2 && scope[0] == '.' && scope[1] == '/') {
        scope.remove_prefix(2);
    }
    while (scope.size() > 1 && scope.back() == '/') {
        scope.remove_suffix(1);
    }
    return scope;
}

/// True when `rel` (root-relative path) equals `scope` (exact file) or is
/// nested directly under it (directory prefix).
bool rel_in_scope(std::string_view rel, std::string_view scope) {
    if (scope.empty()) return false;
    if (rel.size() == scope.size()) return rel == scope;
    return rel.size() > scope.size() &&
           rel.substr(0, scope.size()) == scope && rel[scope.size()] == '/';
}

/// True when `rel` matches ANY of the normalized scope entries.
bool rel_in_any_scope(std::string_view rel,
                      const std::vector<std::string>& scopes) {
    for (const auto& s : scopes) {
        if (rel_in_scope(rel, normalize_scope(s))) return true;
    }
    return false;
}

/// Returns the portion of `abs` relative to `root` (root-relative path), or
/// `abs` unchanged when it does not sit under `root`.
///
/// Intentional duplicate of lci::relative_to_root (src/search/engine.cpp) —
/// reusing it would invert the indexing→search layering. Semantics must stay
/// identical except for the trailing-slash tolerance below, which the engine
/// copy does not need (its roots are pre-normalized).
std::string_view relative_to_root(std::string_view abs, std::string_view root) {
    while (root.size() > 1 && root.back() == '/') root.remove_suffix(1);
    if (root.empty() || abs.size() <= root.size()) return abs;
    if (abs.substr(0, root.size()) == root && abs[root.size()] == '/') {
        return abs.substr(root.size() + 1);
    }
    return abs;
}

}  // namespace

// -- Public search methods ----------------------------------------------------

std::vector<SearchResult> MasterIndex::search(const std::string& pattern,
                                               int max_context_lines) const {
    SearchOptions options;
    options.max_context_lines = max_context_lines;
    return search_with_options(pattern, options);
}

std::vector<SearchResult> MasterIndex::search_with_options(
    const std::string& pattern,
    const SearchOptions& options) const {
    SearchOptions opts = options;

    auto err = validate_search_input(pattern, opts);
    if (!err.empty()) return {};

    err = validate_search_components();
    if (!err.empty()) return {};

    auto candidates = searchable_file_ids();
    if (candidates.empty()) return {};

    auto results = execute_search(pattern, candidates, opts);
    search_count_.fetch_add(1, std::memory_order_relaxed);
    return results;
}

std::vector<FileID> MasterIndex::find_candidate_files(
    const std::string& pattern, bool case_insensitive,
    bool* informative) const {
    auto trigram_narrowing = trigram_index_.narrow(pattern, case_insensitive);
    auto postings_narrowing = postings_index_.narrow(pattern);
    if (informative != nullptr) {
        *informative =
            trigram_narrowing.informative() || postings_narrowing.informative;
    }

    auto all = searchable_file_ids();
    std::vector<FileID> scan_set;
    scan_set.reserve(all.size());
    for (FileID fid : all) {
        if (trigram_narrowing.certifies_absent(fid)) continue;
        if (postings_narrowing.informative &&
            !postings_narrowing.possible.contains(fid)) {
            continue;
        }
        scan_set.push_back(fid);
    }
    return scan_set;
}

std::vector<SearchResult> MasterIndex::search_definitions(
    const std::string& pattern) const {
    SearchOptions options;
    options.declaration_only = true;
    options.max_context_lines = 5;
    return search_with_options(pattern, options);
}

std::vector<SearchResult> MasterIndex::search_references(
    const std::string& symbol) const {
    SearchOptions options;
    options.usage_only = true;
    options.max_context_lines = 5;
    return search_with_options(symbol, options);
}

std::string MasterIndex::get_file_path(FileID file_id) const {
    return id_to_path(file_id);
}

std::vector<std::string> MasterIndex::scopes_without_indexed_match(
    const std::vector<std::string>& scopes) const {
    std::vector<std::string> unmatched;
    if (scopes.empty()) return unmatched;

    // Membership is checked against the FULL indexed file set (not the
    // pattern-narrowed trigram candidates) so this answers "is this path
    // indexed at all?" independent of whether the current pattern happens to
    // occur inside it. A scope that matches an indexed file but whose pattern
    // has zero hits is a legitimate empty result, NOT an unindexed-path error.
    auto file_snap = load_snapshot();
    std::string_view root = config().project.root;
    for (const auto& scope : scopes) {
        std::string_view norm = normalize_scope(scope);
        bool found = false;
        for (const auto& [abs_path, fid] : file_snap->file_map) {
            (void)fid;
            std::string_view rel = relative_to_root(abs_path, root);
            if (rel_in_scope(rel, norm)) {
                found = true;
                break;
            }
        }
        if (!found) unmatched.push_back(scope);
    }
    return unmatched;
}

std::vector<FileID> MasterIndex::get_all_file_ids() const {
    auto snap = load_snapshot();
    std::vector<FileID> ids;
    ids.reserve(snap->file_map.size());
    for (const auto& [path, fid] : snap->file_map) {
        ids.push_back(fid);
    }
    return ids;
}

// The files search may return: those whose attribute activates Search. Every
// shipped attribute does, so a default corpus is searched exactly as before;
// a project that switches it off for a tree (a vendored bundle, generated
// protobufs) gets those files out of every result while they stay in the
// index, where refs and file listing still need them.
//
// The set is precomputed at snapshot publish. Deriving it here cost ~10ms per
// search on the fastapi corpus — a file_attrs probe per file, paid twice
// because search() and find_candidate_files() each derived it.
std::vector<FileID> MasterIndex::searchable_file_ids() const {
    return load_snapshot()->searchable_ids;
}

// -- Validation helpers -------------------------------------------------------

std::string MasterIndex::validate_search_input(
    const std::string& pattern, SearchOptions& options) const {
    if (pattern.empty()) {
        return "search pattern cannot be empty";
    }
    if (pattern.size() > 1000) {
        return "search pattern too long";
    }
    if (options.max_results < 0) {
        return "max results cannot be negative";
    }
    if (options.max_results == 0) {
        options.max_results = 100;
    }
    return {};
}

std::string MasterIndex::validate_search_components() const {
    // Trigram index is always available (owned by value).
    // Reference tracker is always available (owned by value).
    return {};
}

// -- Search execution ---------------------------------------------------------

std::vector<SearchResult> MasterIndex::execute_search(
    const std::string& pattern,
    const std::vector<FileID>& candidates,
    const SearchOptions& options) const {

    // execute_search is fully lock-free. Each index it reads serves a stable,
    // pinned snapshot for the dereference window:
    //   - Reference: pin() holds the ReferenceTracker RCU snapshot, so the raw
    //     const EnhancedSymbol* returned by find_symbols_by_name (sorted and
    //     dereferenced below) stay valid even as a concurrent reindex publishes
    //     a new snapshot (01KSWHQ742 phases 2-3).
    //   - Content: every content access pins the FileContent shared_ptr via
    //     get_file (01KSWHQ742 phase 1).
    //   - Trigram/Postings: internal RCU, return file IDs BY VALUE
    //     (prereq 01KSRKRW8VZB3AEJ97GGNJDMJW).
    // No lock is taken here (the IndexLockManager has since been retired).
    auto refs_snap = ref_tracker_.pin();

    // Pin the file snapshot once for the whole query so id_to_path resolves to
    // a string_view into reverse_file_map with no per-result atomic load or
    // string copy (path is copied into SearchResult::path exactly once).
    auto file_snap = load_snapshot();

    if (options.declaration_only) {
        auto symbols = refs_snap->find_symbols_by_name(pattern);
        std::sort(symbols.begin(), symbols.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if (lhs->symbol.file_id != rhs->symbol.file_id) {
                          return lhs->symbol.file_id < rhs->symbol.file_id;
                      }
                      if (lhs->symbol.line != rhs->symbol.line) {
                          return lhs->symbol.line < rhs->symbol.line;
                      }
                      if (lhs->symbol.column != rhs->symbol.column) {
                          return lhs->symbol.column < rhs->symbol.column;
                      }
                      return lhs->id < rhs->id;
                  });

        std::vector<SearchResult> results;
        results.reserve(symbols.size());
        for (const auto& sym : symbols) {
            if (static_cast<int>(results.size()) >= options.max_results) break;

            SearchContext ctx =
                extract_context(sym->symbol.file_id, sym->symbol.line,
                                options.max_context_lines);
            int column = sym->symbol.column;
            if (!ctx.lines.empty()) {
                const int line_idx = sym->symbol.line - ctx.start_line;
                if (line_idx >= 0 &&
                    line_idx < static_cast<int>(ctx.lines.size())) {
                    const std::string& line_text =
                        ctx.lines[static_cast<size_t>(line_idx)];
                    const size_t pos = line_text.find(sym->symbol.name);
                    if (pos != std::string::npos) {
                        column = static_cast<int>(pos);
                    }
                }
            }

            SearchResult r;
            r.file_id = sym->symbol.file_id;
            r.path = std::string(id_to_path(*file_snap, sym->symbol.file_id));
            r.line = sym->symbol.line;
            r.column = column;
            r.match_text = sym->symbol.name;
            r.context = std::move(ctx);
            r.context.block_name = sym->symbol.name;
            r.context.block_type = "lines";
            results.push_back(std::move(r));
        }
        return results;
    }

    if (options.usage_only) {
        auto symbols = refs_snap->find_symbols_by_name(pattern);
        std::sort(symbols.begin(), symbols.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if (lhs->symbol.file_id != rhs->symbol.file_id) {
                          return lhs->symbol.file_id < rhs->symbol.file_id;
                      }
                      if (lhs->symbol.line != rhs->symbol.line) {
                          return lhs->symbol.line < rhs->symbol.line;
                      }
                      if (lhs->symbol.column != rhs->symbol.column) {
                          return lhs->symbol.column < rhs->symbol.column;
                      }
                      return lhs->id < rhs->id;
                  });

        std::vector<SearchResult> results;
        for (const auto& sym : symbols) {
            auto refs = refs_snap->get_symbol_references(sym->id, "incoming");
            std::sort(refs.begin(), refs.end(),
                      [](const auto& lhs, const auto& rhs) {
                          if (lhs.file_id != rhs.file_id) {
                              return lhs.file_id < rhs.file_id;
                          }
                          if (lhs.line != rhs.line) {
                              return lhs.line < rhs.line;
                          }
                          return lhs.column < rhs.column;
                      });

            for (const auto& ref : refs) {
                if (static_cast<int>(results.size()) >= options.max_results) {
                    return results;
                }

                SearchResult r;
                r.file_id = ref.file_id;
                r.path = std::string(id_to_path(*file_snap, ref.file_id));
                r.line = ref.line;
                r.column = ref.column;
                r.match_text = pattern;
                r.context =
                    extract_context(ref.file_id, ref.line,
                                    options.max_context_lines);
                results.push_back(std::move(r));
            }
        }
        return results;
    }

    // Candidate selection is CERTIFIED-ABSENCE narrowing: a file is skipped
    // only when an index that actually covers it proves the pattern cannot
    // occur there. Anything else — postings-PARTIAL residue, trigram data
    // covering a different subset of the corpus, a pattern the tokenizer
    // cannot represent (phrase with a space, substring of an identifier,
    // mixed-case token searched case-sensitively) — falls through to the
    // verify scan. The former layering treated any non-empty index result
    // as a narrowing, so residue candidate sets suppressed the scan-all
    // fallback and those pattern classes returned empty with no error.
    auto trigram_narrowing =
        trigram_index_.narrow(pattern, options.case_insensitive);
    auto postings_narrowing = postings_index_.narrow(pattern);

    std::vector<FileID> filtered;
    filtered.reserve(candidates.size());
    for (FileID fid : candidates) {
        if (trigram_narrowing.certifies_absent(fid)) continue;
        if (postings_narrowing.informative &&
            !postings_narrowing.possible.contains(fid)) {
            // Postings cover every indexed file: a file with no postings
            // entry and no PARTIAL mark has no >=3-char token run at all,
            // so it cannot contain any of the pattern's usable runs.
            continue;
        }
        filtered.push_back(fid);
    }

    // Path-scope filter (CLI trailing-path args). Applied INDEX-SIDE, before
    // scoring/truncation, so a scoped query never loses in-scope hits to the
    // max_results cap being spent on out-of-scope files. Root-relative match:
    // exact file or directory prefix, OR across all path_scopes entries.
    if (!options.path_scopes.empty()) {
        std::string_view root = config().project.root;
        filtered.erase(
            std::remove_if(filtered.begin(), filtered.end(),
                           [&](FileID fid) {
                               std::string_view rel = relative_to_root(
                                   id_to_path(*file_snap, fid), root);
                               return !rel_in_any_scope(rel,
                                                        options.path_scopes);
                           }),
            filtered.end());
    }

    // Trigram and postings indexes return file IDs in hash-table order,
    // which is non-deterministic across runs. Sort ascending so the
    // search path emits stable, reproducible results across runs. Note
    // that Go's reference iterates the same indexes in *its* hash-map
    // order, which differs file-for-file (Go's file_id assignment for
    // the corpus does not match C++'s scanner-priority order). Ordering
    // parity therefore needs descriptor-level handling — we keep the
    // C++ output deterministic here and let the descriptor decide
    // whether to mask file_id / path.
    std::sort(filtered.begin(), filtered.end());

    std::vector<SearchResult> results;

    for (FileID fid : filtered) {
        if (static_cast<int>(results.size()) >= options.max_results) break;

        // General text search: scan file content for pattern matches.
        // Pin the FileContent shared_ptr for the whole scan: get_content /
        // get_line_offsets each return a view / pointer into a snapshot whose
        // local shared_ptr dies at the call's return, so a concurrent
        // invalidate_file swap could free the FileContent mid-scan. Holding the
        // shared_ptr keeps both the content view and line_offsets alive
        // lock-free — this is the lifetime role the Content ReadGuard played.
        auto fc = file_content_store_->get_file(fid);
        std::string_view content_sv;
        std::string reloaded;
        std::vector<uint32_t> reloaded_offsets;
        const std::vector<uint32_t>* line_offsets = nullptr;
        if (fc) {
            content_sv = fc->view();
            line_offsets = &fc->line_offsets;
        } else {
            // LRU-evicted-but-searchable candidate: skipping it would be a
            // silent false negative. Reload from disk into a request-local
            // buffer (store untouched, read path lock-free).
            reloaded = reload_evicted_content(*file_snap, fid);
            if (reloaded.empty()) continue;
            content_sv = reloaded;
            reloaded_offsets = compute_line_offsets(content_sv);
            line_offsets = &reloaded_offsets;
        }
        if (content_sv.empty()) continue;

        // Single disciplined matcher, shared with SearchEngine::find_matches.
        // Replaces the former bespoke O(content×pattern) tolower double-loop;
        // thread_local lowercase buffers keep the case-insensitive path
        // allocation-free across the candidate scan.
        //
        // Per-file collection is bounded by the REMAINING result budget, not
        // the hidden kMaxMatchesPerFile constant: that default silently
        // returned 100 of N matches for dense files (an error-table file
        // with 1200 hits yielded 101 results against -n 1000000) — the
        // silent-cap class this codebase keeps re-fixing.
        SearchOptions scan_options = options;
        if (scan_options.max_count_per_file <= 0) {
            scan_options.max_count_per_file =
                options.max_results - static_cast<int>(results.size());
        }
        auto matches = find_content_matches(content_sv, pattern, scan_options);
        if (matches.empty()) continue;

        // Line numbers resolve via binary search over the precomputed
        // line-start offsets (line_offsets, set above: pinned fc's offsets
        // or the reloaded buffer's) instead of rescanning from offset 0 per
        // match (the former O(matches×filesize) quadratic).

        // Resolve the path once per file (was once per match — every match in a
        // file shares the same path).
        std::string_view path_view = id_to_path(*file_snap, fid);

        for (const auto& m : matches) {
            if (static_cast<int>(results.size()) >= options.max_results) break;

            int line;
            int col;
            if (line_offsets != nullptr && !line_offsets->empty()) {
                line = search_binary_line_offset(*line_offsets, m.start);
                col = m.start - static_cast<int>(
                          (*line_offsets)[static_cast<size_t>(line - 1)]);
            } else {
                line = search_line_number(content_sv, m.start);
                col = m.start - search_line_start(content_sv, m.start);
            }

            SearchResult r;
            r.file_id = fid;
            r.path = std::string(path_view);
            r.line = line;
            r.column = col;
            r.match_text = pattern;
            // Baseline score parity with Go's literal-match scorer:
            // a flat 855.5 for plain-substring hits keeps text-mode
            // result ordering deterministic and matches the Go output.
            r.score = 855.5;
            r.context = extract_context(fid, line, options.max_context_lines);
            results.push_back(std::move(r));
        }
    }

    return results;
}

SearchContext MasterIndex::extract_context(FileID file_id, int match_line,
                                            int max_context_lines) const {
    SearchContext ctx;
    if (max_context_lines <= 0) return ctx;

    // Pin the FileContent for this function: the line views below alias into
    // it until they are copied into ctx.lines. A bare get_content view could be
    // freed by a concurrent invalidate_file before that copy.
    auto fc = file_content_store_->get_file(file_id);
    std::string_view content_sv;
    std::string reloaded;
    if (fc) {
        content_sv = fc->view();
    } else {
        // LRU-evicted content: reload so results from evicted-but-searchable
        // files still carry context (see the candidate scan above).
        reloaded = reload_evicted_content(*load_snapshot(), file_id);
        if (reloaded.empty()) return ctx;
        content_sv = reloaded;
    }
    if (content_sv.empty()) return ctx;

    // Slice the context window straight out of the precomputed line-start
    // offsets. This used to re-split the WHOLE file into a lines vector on
    // every call, which is once per search result — O(results x filesize) of
    // scanning and one vector allocation each, to read at most
    // 2*max_context_lines+1 lines.
    //
    // The offsets carry the same line decomposition the split produced:
    // offsets[0] is 0 and each subsequent entry is the byte after a '\n' that
    // is not the file's last byte, so offsets.size() equals the old
    // lines.size() and a trailing '\n' never opens a new line.
    static const std::vector<uint32_t> kNoOffsets;
    const std::vector<uint32_t>* offsets = fc ? &fc->line_offsets : &kNoOffsets;
    std::vector<uint32_t> computed;
    if (offsets->empty()) {
        // Content stored without offsets (non-store callers). Derive them
        // with the canonical routine rather than a second private splitter.
        computed = compute_line_offsets(content_sv);
        offsets = &computed;
    }

    int total_lines = static_cast<int>(offsets->size());
    if (total_lines == 0) return ctx;

    int line_idx = match_line - 1;  // Convert 1-based to 0-based.
    if (line_idx < 0) line_idx = 0;
    if (line_idx >= total_lines) line_idx = total_lines - 1;

    int ctx_start = std::max(0, line_idx - max_context_lines);
    int ctx_end = std::min(total_lines - 1, line_idx + max_context_lines);

    ctx.start_line = ctx_start + 1;  // Back to 1-based.
    ctx.end_line = ctx_end + 1;

    // Mirrors Go's reference behavior: each intermediate line is stored
    // without its trailing '\n' separator, but the last line of a file that
    // ends with '\n' keeps the trailing newline. This makes /search and
    // /references context arrays bit-identical to the Go output.
    ctx.lines.reserve(static_cast<size_t>(ctx_end - ctx_start + 1));
    for (int i = ctx_start; i <= ctx_end; ++i) {
        size_t begin = (*offsets)[static_cast<size_t>(i)];
        size_t end = (i + 1 < total_lines)
                         // Drop the '\n' that opened the next line.
                         ? (*offsets)[static_cast<size_t>(i + 1)] - 1
                         // Final line: runs to EOF, trailing '\n' included.
                         : content_sv.size();
        ctx.lines.emplace_back(content_sv.substr(begin, end - begin));
    }

    return ctx;
}

}  // namespace lci
