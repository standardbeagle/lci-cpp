#include <lci/indexing/pipeline_processor.h>

#include <absl/container/flat_hash_map.h>

#include <lci/analysis/side_effect_analyzer.h>

#include <lci/core/reference_tracker.h>
#include <lci/core/trigram.h>
#include <lci/language_map.h>
#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>
#include <lci/parser/svelte_script.h>
#include <lci/parser/unified_extractor.h>

#include <tree_sitter/api.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace lci {

namespace {

/// Extracts symbols, references, and scopes from a file's content via
/// tree-sitter + UnifiedExtractor. Populates `result` in place; trigram and
/// postings indexing proceed regardless.
///
/// Every early return records WHY it gave up in result.parse_skip_reason.
/// Previously three of them (no grammar, no parser, no tree) returned with
/// the field untouched, so a genuine extraction failure was reported to the
/// caller as "completed with zero symbols" — the file stayed searchable as
/// text while being invisible to every symbol endpoint, and nothing in the
/// pipeline could tell the two apart. Setting an enum costs no allocation,
/// so the hot path is unchanged.
void run_unified_extraction(ProcessedFile& result,
                            std::string_view content,
                            const std::string& path,
                            int64_t max_parse_bytes,
                            SideEffectAnalyzer* side_effect_sink) {
    auto ext = std::filesystem::path(path).extension().string();
    if (ext.empty()) {
        result.parse_skip_reason = ParseSkipReason::UnsupportedGrammar;
        return;
    }

    // Svelte components have no dedicated grammar: mask the markup to
    // spaces (geometry-preserving) and parse the <script> block(s) with the
    // JS/TS grammar. Positions in the masked buffer are positions in the
    // original file. See lci/parser/svelte_script.h.
    const bool is_svelte = (ext == ".svelte");
    std::string svelte_masked;
    if (is_svelte) {
        auto sv = parser::mask_svelte_script(content, svelte_masked);
        ext = sv.typescript ? ".ts" : ".js";
        content = svelte_masked;
        // A script-less component still parses (all-blank buffer, zero
        // symbols) and gets its synthesized component symbol below.
    }

    parser::Language lang{};
    if (!parser::language_from_extension(ext, lang)) {
        // Trigrams still index it for text search.
        result.parse_skip_reason = ParseSkipReason::UnsupportedGrammar;
        return;
    }

    // Oversized source: skip the tree-sitter parse (the expensive stage) but
    // keep trigram text indexing.
    if (max_parse_bytes > 0 &&
        static_cast<int64_t>(content.size()) > max_parse_bytes) {
        result.parse_skip_reason = ParseSkipReason::Oversize;
        return;
    }

    // Minified/generated bundles under the size cutoff are worse than
    // oversized ones: memprofile pinned a 1.36 MB minified babel bundle at
    // 4.4 GB of RSS from symbol extraction alone (thousands of symbols and
    // references on one line, each carried through ProcessedFile and the
    // reference tracker). Same detector the trigram index uses; symbols in
    // generated code have no navigation value. Reported as its own reason,
    // not as Oversize — the file is small, and calling it oversized sent
    // anyone reading the diagnostics to the wrong knob.
    if (is_trigram_hostile(content)) {
        result.parse_skip_reason = ParseSkipReason::MinifiedBundle;
        return;
    }

    parser::PooledParser parser_guard(lang);
    if (!parser_guard) {
        result.parse_skip_reason = ParseSkipReason::ParserUnavailable;
        return;
    }

    parser::UniqueTree tree(ts_parser_parse_string(
        parser_guard.get(), nullptr, content.data(),
        static_cast<uint32_t>(content.size())));
    if (!tree) {
        result.parse_skip_reason = ParseSkipReason::ParseFailed;
        return;
    }

    parser::UnifiedExtractor extractor;
    extractor.init(content, result.file_id, ext, path);
    if (side_effect_sink) extractor.set_side_effect_sink(side_effect_sink);
    extractor.extract(tree.get());
    auto extracted = extractor.get_results();

    // Build a position-keyed metadata index so the integrator can enrich
    // EnhancedSymbol records (complexity, signature, doc comment) without
    // changing the ReferenceTracker API. Symbol coordinates and the
    // declaration / complexity keys all use 1-based lines and columns.
    // Index the complexity table by position once per FILE. Scanning it per
    // symbol made enrichment O(symbols x complexity_points); a symbol-dense
    // file paid that on the indexing hot path. First-wins matches the old
    // front-to-back scan.
    absl::flat_hash_map<uint64_t, int> complexity_by_position;
    complexity_by_position.reserve(extracted.complexity.size());
    for (const auto& [pk, cx] : extracted.complexity) {
        complexity_by_position.try_emplace(pack_position(pk.line, pk.column),
                                           cx);
    }

    result.symbol_metadata.reserve(extracted.symbols.size());
    for (const auto& sym : extracted.symbols) {
        ProcessedSymbolMetadata meta;
        meta.line = sym.line;
        meta.column = sym.column;
        auto cx_it =
            complexity_by_position.find(pack_position(sym.line, sym.column));
        if (cx_it != complexity_by_position.end()) {
            meta.complexity = cx_it->second;
        }
        auto [signature, doc_comment] =
            extractor.lookup_declaration(sym.line, sym.column);
        meta.signature.assign(signature);
        meta.doc_comment.assign(doc_comment);
        result.symbol_metadata.push_back(std::move(meta));
    }

    result.symbols = std::move(extracted.symbols);

    // Every .svelte file IS a component: synthesize a file-level Class
    // symbol named after the file stem so `search Counter` / list_symbols
    // surface the component itself, not only its script internals.
    if (is_svelte) {
        Symbol comp;
        comp.name = std::filesystem::path(path).stem().string();
        comp.type = SymbolType::Class;
        comp.file_id = result.file_id;
        comp.line = 1;
        comp.column = 1;
        comp.end_line = 1;
        comp.end_column = 1;
        comp.visibility = SymbolVisibility::Public;
        ProcessedSymbolMetadata comp_meta;
        comp_meta.line = comp.line;
        comp_meta.column = comp.column;
        result.symbols.push_back(std::move(comp));
        result.symbol_metadata.push_back(std::move(comp_meta));
    }

    result.references = std::move(extracted.references);
    result.field_types = std::move(extracted.field_types);
    result.scopes = std::move(extracted.scopes);
}

}  // namespace

FileProcessor::FileProcessor(
    const Config& config,
    std::shared_ptr<FileService> file_service,
    TrigramIndex* trigram_index)
    : config_(config),
      file_service_(std::move(file_service)),
      trigram_index_(trigram_index) {
    // Warm tree-sitter grammar tables on the main thread so the first
    // parse on a worker thread does not pay a cold-start cost. The
    // grammar init functions are documented thread-safe; touching them
    // and driving a one-byte parse keeps the worker pool's first batch
    // hot without measurable overhead on small corpora.
    for (int i = 0; i < parser::kLanguageCount; ++i) {
        auto lang = static_cast<parser::Language>(i);
        const TSLanguage* ts_lang = parser::get_ts_language(lang);
        if (ts_lang == nullptr) continue;

        parser::UniqueParser p = parser::make_parser(lang);
        if (!p) continue;

        constexpr const char* kWarmInput = " ";
        parser::UniqueTree tree(
            ts_parser_parse_string(p.get(), nullptr, kWarmInput, 1));
    }
}

void FileProcessor::process(
    BoundedQueue<FileTask>& tasks,
    BoundedQueue<ProcessedFile>& results,
    int worker_count) {

    if (worker_count <= 0) {
        worker_count = static_cast<int>(std::thread::hardware_concurrency());
        if (worker_count < 1) worker_count = 4;
    }

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i) {
        workers.emplace_back(&FileProcessor::worker_loop, this,
                             i, std::ref(tasks), std::ref(results));
    }

    for (auto& w : workers) w.join();
    results.close();
}

void FileProcessor::worker_loop(
    int worker_id,
    BoundedQueue<FileTask>& tasks,
    BoundedQueue<ProcessedFile>& results) {

    // Private per-worker analyzer: the extractor's sink hooks are a
    // stateful per-function lifecycle, so workers never share one. Records
    // drain into the shared target after each file under a short lock —
    // a handful of map moves per file, no contention on the parse path.
    std::optional<SideEffectAnalyzer> local_side_effects;
    if (side_effect_target_) local_side_effects.emplace("generic");

    FileTask task;
    while (tasks.pop(task)) {
        auto result = process_file(
            worker_id, task,
            local_side_effects ? &*local_side_effects : nullptr);
        if (local_side_effects) {
            auto batch = local_side_effects->take_results();
            if (!batch.empty()) {
                std::lock_guard lock(side_effect_mu_);
                side_effect_target_->merge_results(std::move(batch));
            }
        }
        if (!results.push(std::move(result))) return;
    }
}

ProcessedFile FileProcessor::process_file(
    int /*worker_id*/, const FileTask& task,
    SideEffectAnalyzer* side_effect_sink) {
    auto start = std::chrono::steady_clock::now();
    ProcessedFile result;
    result.path = task.path;
    result.language = task.language;
    result.stage = "parsing";

    // Producer-assigned FileID skips the redundant store_->add_file
    // snapshot copy. Fallback path covers single-file callers that
    // bypass the producer pipeline (tests, ad-hoc).
    FileID file_id = task.preloaded_id;
    if (file_id == 0) {
        auto load_result = file_service_->load_file_from_disk(task.path);
        if (!load_result.has_value()) {
            result.has_error = true;
            result.error = load_result.error();
            result.stage = "loading";
            result.duration = std::chrono::steady_clock::now() - start;
            return result;
        }
        file_id = load_result.value();
        if (file_id == 0) {
            result.stage = "directory_skipped";
            result.duration = std::chrono::steady_clock::now() - start;
            return result;
        }
    }

    auto content = file_service_->get_content(file_id);

    // Defense-in-depth: binary check on loaded content
    if (binary_detector_.is_binary_by_magic_number(content)) {
        result.has_error = true;
        result.error.type = ErrorType::Indexing;
        result.error.message = "binary file detected by magic number";
        result.error.file_path = task.path;
        result.stage = "binary_detection";
        result.duration = std::chrono::steady_clock::now() - start;
        return result;
    }

    result.file_id = file_id;

    // Parse the file and extract symbols, references, and scopes via
    // tree-sitter. This populates the symbol-aware data the integrator
    // feeds into ReferenceTracker. Without this step, browse-file,
    // list-symbols, references, and tree endpoints all return empty.
    run_unified_extraction(result, content, task.path,
                           config_.index.max_parse_file_size,
                           side_effect_sink);

    // Per-occurrence trigram bucketing removed (2026-08-04): its sole
    // consumer, ShardedTrigramStorage (fed via the merger pipeline), had no
    // production readers — find_candidates reads the snapshot maps, which the
    // bulk path never populated. Building it cost ~8 bytes per corpus byte
    // plus hash overhead and drove servers past 26 GB RSS on large repos.
    //
    // The file-granular replacement: a per-file bloom over the DISTINCT
    // trigram set (~1 byte per distinct trigram corpus-wide), built here in
    // the parallel worker and installed by the integrator. It certifies
    // pattern absence per file (TrigramIndex::narrow), which the
    // per-occurrence store never managed to do in production. Hostile
    // content (minified/high-entropy) gets no bloom and self-nominates via
    // the unfiltered set — the same gates the incremental index_file uses.
    const bool payload_content = has_high_entropy_section(content);
    if (is_trigram_hostile(content) || payload_content) {
        result.trigram_hostile = true;
    } else {
        result.trigram_bloom = TrigramBloom::build(content);
    }

    // Tokenize for PostingsIndex inline so the per-byte scan + dedup
    // runs in parallel here instead of serially on the integrator
    // thread. FileIntegrator::merge_postings consumes the result via
    // index_file_pretokenized — pure merge, no re-walk of content.
    {
        // Unique-token cap policy (see postings_token_cap): data files and
        // detected payload sections get the configured cap; code files get
        // a 4x harm ceiling so a payload the classifier cannot recognize
        // is still bounded by its own measured cost. Capped files are
        // marked PARTIAL and self-nominate in postings lookups, so search
        // stays exact either way.
        const size_t token_cap = postings_token_cap(
            language_info_for_path(task.path).is_code, payload_content,
            config_.index.data_file_token_cap);
        auto pi_tokens = lci::PostingsIndex::tokenize_content(
            content, token_cap, &result.postings_truncated);
        result.postings_tokens.reserve(pi_tokens.size());
        for (auto& pt : pi_tokens) {
            ProcessedToken t;
            t.token = std::move(pt.token);
            t.offset = pt.offset;
            result.postings_tokens.push_back(std::move(t));
        }
    }

    result.stage = "completed";
    result.duration = std::chrono::steady_clock::now() - start;
    return result;
}

}  // namespace lci
