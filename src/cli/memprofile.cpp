// `lci debug memprofile` — incremental per-file memory attribution.
//
// Motivation (2026-08-04): lci servers reached 26 GB RSS on a 2 GB corpus
// (mongodb driver) and >10 GB on next.js — 30-50x amplification against a
// ≤2x budget. This command feeds the SAME processing path the pipeline runs
// (FileProcessor::process_single + FileIntegrator::integrate_file), one file
// at a time on one thread, so growth is attributable to individual files and
// to the post-scan phases.
//
// Two rulers, deliberately: VmRSS for whole-run and phase totals (what the
// host actually pays), and glibc mallinfo2 live-allocated bytes for per-file
// attribution. RSS cannot do the per-file job — the allocator expands arenas
// in large chunks, so a file's RSS delta says whether it tripped the next
// expansion, not what it retained. Flagging still requires both an absolute
// floor and a size-relative ratio; the aggregate ratio remains the
// load-bearing number.

#include <lci/cli/commands.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <malloc.h>
#include <string>
#include <unordered_set>
#include <vector>

#include <lci/core/file_content_store.h>
#include <lci/core/file_service.h>
#include <lci/core/reference_tracker.h>
#include <lci/core/trigram.h>
#include <lci/indexing/pipeline_integrator.h>
#include <lci/indexing/pipeline_processor.h>
#include <lci/indexing/pipeline_scanner.h>

namespace lci {
namespace cli {

namespace {

// Reads VmRSS in kB from /proc/self/status. Returns -1 when unavailable
// (non-Linux) — the command fails fast rather than reporting zeros.
long read_rss_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    long rss_kb = -1;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::sscanf(line, "VmRSS: %ld kB", &rss_kb) == 1) break;
    }
    std::fclose(f);
    return rss_kb;
}

// Live allocated bytes (glibc mallinfo2 uordblks). RSS is the wrong ruler for
// per-file attribution: the allocator grows its arenas in large chunks, so a
// file's RSS delta records whether it happened to trigger the next expansion,
// not what it retained. Two runs over this repo disagreed on the top offender
// while both reported the same 11.1 MB, which is the signature of that
// artifact. uordblks moves only when the program actually holds more.
int64_t read_alloc_bytes() {
#if defined(__GLIBC__)
    return static_cast<int64_t>(mallinfo2().uordblks);
#else
    return -1;
#endif
}

struct FileDelta {
    std::string path;
    int64_t size_bytes{};
    int64_t delta_bytes{};
};

/// One-time cost of standing up a language's parser (tree-sitter grammar
/// tables, query compilation) lands on whichever file of that language the
/// scan happens to reach first. Attributing it to that file is a lie the
/// ranking then amplifies: an ordinary 5 KB C++ header showed 11.1 MB and
/// topped the offender list purely for being first. Bucket it separately.
struct LanguageInit {
    std::string language;
    std::string first_file;
    int64_t delta_bytes{};
};

double mb(int64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

}  // namespace

int run_debug_memprofile(const GlobalFlags& flags,
                         const MemprofileOptions& opts) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::fprintf(stderr, "Error: %s\n", err.c_str());
        return 1;
    }
    if (read_rss_kb() < 0) {
        std::fprintf(stderr,
                     "Error: /proc/self/status not readable — "
                     "memprofile is Linux-only\n");
        return 1;
    }

    // Same sub-index wiring as MasterIndex / index_directory, minus the
    // server: one content store, one file service, bulk windows open.
    auto content_store = std::make_shared<FileContentStore>(
        static_cast<int64_t>(cfg.performance.max_memory_mb) * 1024 * 1024);
    auto file_service =
        std::make_shared<FileService>(content_store, cfg.index.max_file_size);

    TrigramIndex trigram_index;
    SymbolLocationIndex symbol_location_index;
    ReferenceTracker ref_tracker(&symbol_location_index);
    PostingsIndex postings_index;

    trigram_index.set_bulk_indexing(true);
    ref_tracker.set_bulk_indexing(true);
    symbol_location_index.set_bulk_indexing(true);
    postings_index.set_bulk_indexing(true);

    FileProcessor processor(cfg, file_service, &trigram_index);
    FileIntegrator integrator(&trigram_index, &ref_tracker, &postings_index);
    integrator.set_file_content_store(content_store.get());
    integrator.set_symbol_location_index(&symbol_location_index);

    FileScanner scanner(cfg);
    // Budget-exempt: the whole point is measuring what unbudgeted indexing
    // costs, including the files a budgeted run would skip.
    auto tasks = scanner.scan(/*apply_budget=*/false).tasks;

    int64_t corpus_bytes = 0;
    for (const auto& t : tasks) corpus_bytes += t.size;

    const int64_t flag_floor =
        static_cast<int64_t>(opts.flag_floor_mb) * 1024 * 1024;

    std::fprintf(stderr,
                 "memprofile: %zu files, %.1f MB corpus, flag = delta > %d MB "
                 "AND delta > %.1fx file size\n",
                 tasks.size(), mb(corpus_bytes), opts.flag_floor_mb,
                 opts.flag_ratio);

    const long rss_start_kb = read_rss_kb();
    int64_t alloc_prev = read_alloc_bytes();

    std::vector<FileDelta> deltas;
    deltas.reserve(tasks.size());
    std::vector<LanguageInit> lang_inits;
    std::unordered_set<std::string> languages_seen;
    int flagged = 0;
    size_t processed = 0;

    for (const auto& task : tasks) {
        const bool first_of_language =
            !task.language.empty() && languages_seen.insert(task.language).second;

        ProcessedFile pf = processor.process_single(task);
        if (!pf.has_error) integrator.integrate_file(pf);

        const int64_t alloc_now = read_alloc_bytes();
        const int64_t delta = alloc_now - alloc_prev;
        alloc_prev = alloc_now;

        // Charged to the language, not the file, and kept out of the ranking
        // and the FLAG line so neither points at an innocent file.
        if (first_of_language) {
            lang_inits.push_back({task.language, task.path, delta});
            ++processed;
            continue;
        }

        deltas.push_back({task.path, task.size, delta});

        if (delta > flag_floor &&
            static_cast<double>(delta) >
                opts.flag_ratio * static_cast<double>(task.size)) {
            ++flagged;
            std::printf("FLAG %8.1f MB (%6.1fx of %8" PRId64 " B) %s\n",
                        mb(delta),
                        task.size > 0
                            ? static_cast<double>(delta) /
                                  static_cast<double>(task.size)
                            : 0.0,
                        task.size, task.path.c_str());
            std::fflush(stdout);
        }

        if (++processed % 1000 == 0) {
            std::fprintf(stderr, "  %zu/%zu files, rss %.1f MB, live alloc "
                                 "%.1f MB\n",
                         processed, tasks.size(), mb(read_rss_kb() * 1024),
                         mb(alloc_now));
        }
    }

    const long rss_after_files_kb = read_rss_kb();

    // Post-scan phases, attributed separately: closing the bulk RCU windows
    // publishes the staged snapshots; reference resolution walks the whole
    // symbol table. Either can dominate growth without any single file
    // looking guilty.
    // Ordering mirrors MasterIndex::index_directory: the symbol-location
    // index publishes BEFORE resolution (resolution reads it), but the
    // ref tracker's bulk window stays open THROUGH resolution so it
    // mutates staging in place. The previous order closed the tracker
    // first, so resolution's write_snapshot cloned the entire snapshot --
    // a full-index copy production never pays, misreported here as a
    // 79-109 MB "reference-res delta" at next.js scale.
    trigram_index.set_bulk_indexing(false);
    symbol_location_index.set_bulk_indexing(false);
    postings_index.set_bulk_indexing(false);
    const long rss_after_publish_kb = read_rss_kb();

    ref_tracker.process_all_references();
    ref_tracker.set_bulk_indexing(false);
    const long rss_after_refs_kb = read_rss_kb();

    std::sort(deltas.begin(), deltas.end(),
              [](const FileDelta& a, const FileDelta& b) {
                  return a.delta_bytes > b.delta_bytes;
              });

    const int64_t total_growth = (rss_after_refs_kb - rss_start_kb) * 1024;
    std::printf("\n== memprofile summary ==\n");
    std::printf("files indexed:        %zu (%d flagged)\n", processed, flagged);
    std::printf("corpus size:          %10.1f MB\n", mb(corpus_bytes));
    std::printf("rss at start:         %10.1f MB\n", mb(rss_start_kb * 1024));
    std::printf("rss after files:      %10.1f MB\n",
                mb(rss_after_files_kb * 1024));
    std::printf("bulk-publish delta:   %10.1f MB\n",
                mb((rss_after_publish_kb - rss_after_files_kb) * 1024));
    std::printf("reference-res delta:  %10.1f MB\n",
                mb((rss_after_refs_kb - rss_after_publish_kb) * 1024));
    std::printf("total rss growth:     %10.1f MB  (%.1fx corpus)\n",
                mb(total_growth),
                corpus_bytes > 0 ? static_cast<double>(total_growth) /
                                       static_cast<double>(corpus_bytes)
                                 : 0.0);

    std::sort(lang_inits.begin(), lang_inits.end(),
              [](const LanguageInit& a, const LanguageInit& b) {
                  return a.delta_bytes > b.delta_bytes;
              });
    int64_t lang_init_total = 0;
    for (const auto& li : lang_inits) lang_init_total += li.delta_bytes;
    std::printf("\n== one-time parser init (%zu languages, %.1f MB) ==\n",
                lang_inits.size(), mb(lang_init_total));
    std::printf("Charged to the language, not to the first file of it. "
                "Fixed cost per language, not per corpus size.\n");
    for (const auto& li : lang_inits) {
        if (li.delta_bytes <= 0) continue;
        std::printf("  %8.1f MB  %-12s (first seen: %s)\n", mb(li.delta_bytes),
                    li.language.c_str(), li.first_file.c_str());
    }

    // Structure census: where the resident bytes actually sit. Counts, not
    // byte estimates — pairing them with the phase deltas above localizes
    // the dominant per-entry costs without an allocator profiler.
    {
        auto snap = ref_tracker.pin();
        size_t file_scope_entries = 0;
        for (const auto& [fid, v] : snap->scopes_by_file) {
            file_scope_entries += v.size();
        }
        size_t ref_count = 0;
        snap->for_each_live_ref(
            [&](lci::FileID, uint32_t, const lci::StoredRef&) { ++ref_count; });
        size_t ref_name_pool_bytes = 0;
        for (const auto& n : snap->ref_names) ref_name_pool_bytes += n.size();
        std::printf("\n== structure census ==\n");
        std::printf("symbols:                 %d\n", snap->symbols.size());
        std::printf("references:              %zu (sizeof %zu B stored, "
                    "%.1f MB name pool of %zu)\n",
                    ref_count, sizeof(lci::StoredRef),
                    mb(static_cast<int64_t>(ref_name_pool_bytes)),
                    snap->ref_names.size());
        std::printf("scopes_by_file entries:  %zu\n", file_scope_entries);
        const auto pstats = postings_index.memory_stats();
        std::printf("postings tokens:         %d (%d files, %d partial; "
                    "%.1f MB strings, %zu postings, %zu reverse keys)\n",
                    postings_index.token_count(), postings_index.file_count(),
                    postings_index.partial_file_count(),
                    mb(static_cast<int64_t>(pstats.token_string_bytes)),
                    pstats.posting_entries, pstats.reverse_key_entries);
        size_t in_ref_ids = 0, out_ref_ids = 0;
        for (const auto& [sid, v] : snap->incoming_refs) in_ref_ids += v.size();
        for (const auto& [sid, v] : snap->outgoing_refs) out_ref_ids += v.size();
        std::printf("ref id lists:            %zu incoming / %zu outgoing "
                    "(%zu symbols mapped)\n",
                    in_ref_ids, out_ref_ids,
                    snap->incoming_refs.size() + snap->outgoing_refs.size());
        std::printf("content store:           %.1f MB\n",
                    mb(content_store->get_memory_usage()));

        // Chains are interned (hash-consed): count unique storage once,
        // and report how many symbol references share it.
        size_t chain_entries = 0, chain_strings = 0, chain_refs = 0,
               sym_strings = 0;
        // Fill rates for EnhancedSymbol's remaining per-instance strings,
        // paid whether populated or not. These counts decide whether a
        // side-table / interning slim is worth it. The zero-fill audit of
        // 2026-08-11 already deleted five never-written containers.
        size_t fill_type_info = 0, fill_doc = 0, fill_sig = 0;
        std::unordered_set<const void*> chains_seen;
        snap->symbols.range([&](SymbolID, const EnhancedSymbol& es) {
            chain_refs += es.scope_chain.size();
            if (es.scope_chain.storage_key() != nullptr &&
                chains_seen.insert(es.scope_chain.storage_key()).second) {
                chain_entries += es.scope_chain.size();
                for (const auto& sc : es.scope_chain) {
                    chain_strings += sc.name.size() + sc.full_path.size();
                }
            }
            sym_strings += es.symbol.name.size() + es.type_info.size() +
                           es.doc_comment.size() + es.signature.size();
            if (!es.type_info.empty()) ++fill_type_info;
            if (!es.doc_comment.empty()) ++fill_doc;
            if (!es.signature.empty()) ++fill_sig;
            return true;
        });

        std::printf("scope_chain entries:     %zu unique (%zu referenced, "
                    "%.1f MB strings, sizeof(ScopeInfo)=%zu)\n",
                    chain_entries, chain_refs,
                    mb(static_cast<int64_t>(chain_strings)),
                    sizeof(ScopeInfo));
        std::printf("symbol field fill:       type_info %zu, doc %zu, "
                    "sig %zu\n",
                    fill_type_info, fill_doc, fill_sig);
        std::printf("symbol strings:          %.1f MB "
                    "(sizeof(EnhancedSymbol)=%zu)\n",
                    mb(static_cast<int64_t>(sym_strings)),
                    sizeof(EnhancedSymbol));
    }

    const int top_n =
        std::min<int>(opts.top, static_cast<int>(deltas.size()));
    // Caveat, proven with a 400-identical-file corpus: a small file with an
    // outsized delta may just be the file that crossed a global container's
    // growth threshold (trigram/postings rehash) -- the growth is real but
    // belongs to the corpus, not the file. Cross-check a suspect by indexing
    // it in isolation before treating it as a leak.
    std::printf("\ntop %d files by live-allocation delta "
                "(includes global container growth charged to the file that "
                "triggered it -- verify suspects in isolation):\n",
                top_n);
    for (int i = 0; i < top_n; ++i) {
        const auto& d = deltas[i];
        std::printf("  %8.1f MB (%8" PRId64 " B) %s\n", mb(d.delta_bytes),
                    d.size_bytes, d.path.c_str());
    }
    return 0;
}

}  // namespace cli
}  // namespace lci
