#include <lci/indexing/master_index.h>

#include <algorithm>
#include <string_view>

namespace lci {

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

    auto candidates = get_all_file_ids();
    if (candidates.empty()) return {};

    auto results = execute_search(pattern, candidates, opts);
    search_count_.fetch_add(1, std::memory_order_relaxed);
    return results;
}

std::vector<FileID> MasterIndex::find_candidate_files(
    const std::string& pattern, bool case_insensitive) const {
    return trigram_index_.find_candidates_with_options(pattern, case_insensitive);
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

std::vector<FileID> MasterIndex::get_all_file_ids() const {
    auto snap = load_snapshot();
    std::vector<FileID> ids;
    ids.reserve(snap->file_map.size());
    for (const auto& [path, fid] : snap->file_map) {
        ids.push_back(fid);
    }
    return ids;
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

    // Read locks are needed ONLY for indexes whose reads hand back references
    // into shared storage, so the lock must outlive the dereference (lifetime
    // guards, not lookup-atomicity):
    //   - Reference: find_symbols_by_name returns raw const EnhancedSymbol*
    //     that the rest of this function sorts and dereferences.
    //   - Content:   get_content returns a string_view into a snapshotted
    //     FileContent that the candidate scan reads below.
    // Trigram and Postings now serve reads from internal RCU snapshots and
    // return file IDs BY VALUE (find_candidates_with_options -> vector<FileID>,
    // find -> fills caller vectors), so a concurrent reindex cannot dangle
    // them. Their read locks were pure lookup-atomicity and are now redundant
    // (prereq: TrigramIndex+PostingsIndex RCU snapshots, 01KSRKRW8VZB3AEJ97GGNJDMJW).
    IndexLockManager::ReadGuard ref_lock(lock_manager_, IndexType::Reference);
    IndexLockManager::ReadGuard content_lock(lock_manager_, IndexType::Content);

    if (!ref_lock.locked() || !content_lock.locked()) {
        return {};
    }

    if (options.declaration_only) {
        auto symbols = ref_tracker_.find_symbols_by_name(pattern);
        std::sort(symbols.begin(), symbols.end(),
                  [](const EnhancedSymbol* lhs, const EnhancedSymbol* rhs) {
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
        for (const auto* sym : symbols) {
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
            r.path = id_to_path(sym->symbol.file_id);
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
        auto symbols = ref_tracker_.find_symbols_by_name(pattern);
        std::sort(symbols.begin(), symbols.end(),
                  [](const EnhancedSymbol* lhs, const EnhancedSymbol* rhs) {
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
        for (const auto* sym : symbols) {
            auto refs = ref_tracker_.get_symbol_references(sym->id, "incoming");
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
                r.path = id_to_path(ref.file_id);
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

    // Try trigram candidates first to narrow the file list.
    auto trigram_candidates = trigram_index_.find_candidates_with_options(
        pattern, options.case_insensitive);

    absl::flat_hash_set<FileID> candidate_set(candidates.begin(),
                                               candidates.end());
    std::vector<FileID> filtered;

    if (!trigram_candidates.empty()) {
        filtered.reserve(trigram_candidates.size());
        for (FileID fid : trigram_candidates) {
            if (candidate_set.contains(fid)) {
                filtered.push_back(fid);
            }
        }
    }

    // If trigram filtering yielded nothing, try the postings index.
    if (filtered.empty()) {
        std::vector<FileID> postings_files;
        absl::flat_hash_map<FileID, int> postings_offsets;
        postings_index_.find(pattern, options.case_insensitive,
                             postings_files, postings_offsets);
        for (FileID fid : postings_files) {
            if (candidate_set.contains(fid)) {
                filtered.push_back(fid);
            }
        }
    }

    // If neither index narrows candidates, scan all candidates directly.
    // This handles short patterns and ensures text search always works.
    if (filtered.empty() && !options.declaration_only && !options.usage_only) {
        filtered = candidates;
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
        auto content_sv = file_content_store_->get_content(fid);
        if (content_sv.empty()) continue;

        // Single disciplined matcher, shared with SearchEngine::find_matches.
        // Replaces the former bespoke O(content×pattern) tolower double-loop;
        // thread_local lowercase buffers keep the case-insensitive path
        // allocation-free across the candidate scan.
        auto matches = find_content_matches(content_sv, pattern, options);
        if (matches.empty()) continue;

        // Resolve line numbers via binary search over the precomputed
        // line-start offsets instead of rescanning from offset 0 per match
        // (the former O(matches×filesize) quadratic).
        const std::vector<uint32_t>* line_offsets =
            file_content_store_->get_line_offsets(fid);

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
            r.path = id_to_path(fid);
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

    auto content_sv = file_content_store_->get_content(file_id);
    if (content_sv.empty()) return ctx;

    // Split content into lines. Mirrors Go's reference behavior: each
    // intermediate line is stored without its trailing '\n' separator,
    // but the last line of a file that ends with '\n' keeps the
    // trailing newline. This makes /search and /references context
    // arrays bit-identical to the Go output.
    std::vector<std::string_view> lines;
    size_t start = 0;
    for (size_t i = 0; i < content_sv.size(); ++i) {
        if (content_sv[i] == '\n') {
            lines.emplace_back(content_sv.data() + start, i - start);
            start = i + 1;
        }
    }
    if (start < content_sv.size()) {
        lines.emplace_back(content_sv.data() + start,
                           content_sv.size() - start);
    } else if (!lines.empty()) {
        // File ended with '\n'. Re-attach it to the final line so the
        // last context line is the only one that carries a trailing
        // newline (Go's encoder behavior).
        auto& last = lines.back();
        last = std::string_view(last.data(), last.size() + 1);
    }

    int total_lines = static_cast<int>(lines.size());
    int line_idx = match_line - 1;  // Convert 1-based to 0-based.
    if (line_idx < 0) line_idx = 0;
    if (line_idx >= total_lines) line_idx = total_lines - 1;

    int ctx_start = std::max(0, line_idx - max_context_lines);
    int ctx_end = std::min(total_lines - 1, line_idx + max_context_lines);

    ctx.start_line = ctx_start + 1;  // Back to 1-based.
    ctx.end_line = ctx_end + 1;

    for (int i = ctx_start; i <= ctx_end; ++i) {
        ctx.lines.emplace_back(lines[static_cast<size_t>(i)]);
    }

    return ctx;
}

}  // namespace lci
