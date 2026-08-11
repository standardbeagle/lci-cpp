// `lci debug memprofile` — incremental per-file memory attribution.
//
// Motivation (2026-08-04): lci servers reached 26 GB RSS on a 2 GB corpus
// (mongodb driver) and >10 GB on next.js — 30-50x amplification against a
// ≤2x budget. This command feeds the SAME processing path the pipeline runs
// (FileProcessor::process_single + FileIntegrator::integrate_file), one file
// at a time on one thread, and samples VmRSS around each file so the growth
// is attributable to individual files and to the post-scan phases.
//
// RSS is allocator-noisy per file (arena growth lands on whichever file
// triggered it), so flagging requires both an absolute floor and a
// size-relative ratio; the aggregate ratio at the end is the load-bearing
// number.

#include <lci/cli/commands.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
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

struct FileDelta {
    std::string path;
    int64_t size_bytes{};
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
    long rss_prev_kb = rss_start_kb;

    std::vector<FileDelta> deltas;
    deltas.reserve(tasks.size());
    int flagged = 0;
    size_t processed = 0;

    for (const auto& task : tasks) {
        ProcessedFile pf = processor.process_single(task);
        if (!pf.has_error) integrator.integrate_file(pf);

        const long rss_kb = read_rss_kb();
        const int64_t delta = (rss_kb - rss_prev_kb) * 1024;
        rss_prev_kb = rss_kb;
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
            std::fprintf(stderr, "  %zu/%zu files, rss %.1f MB\n", processed,
                         tasks.size(), mb(rss_kb * 1024));
        }
    }

    const long rss_after_files_kb = read_rss_kb();

    // Post-scan phases, attributed separately: closing the bulk RCU windows
    // publishes the staged snapshots; reference resolution walks the whole
    // symbol table. Either can dominate growth without any single file
    // looking guilty.
    trigram_index.set_bulk_indexing(false);
    ref_tracker.set_bulk_indexing(false);
    symbol_location_index.set_bulk_indexing(false);
    postings_index.set_bulk_indexing(false);
    const long rss_after_publish_kb = read_rss_kb();

    ref_tracker.process_all_references();
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

    // Structure census: where the resident bytes actually sit. Counts, not
    // byte estimates — pairing them with the phase deltas above localizes
    // the dominant per-entry costs without an allocator profiler.
    {
        auto snap = ref_tracker.pin();
        size_t file_scope_entries = 0;
        for (const auto& [fid, v] : snap->scopes_by_file) {
            file_scope_entries += v.size();
        }
        size_t ref_string_bytes = 0;
        for (const auto& [rid, r] : snap->references) {
            ref_string_bytes += r.referenced_name.size();
        }
        std::printf("\n== structure census ==\n");
        std::printf("symbols:                 %d\n", snap->symbols.size());
        std::printf("references:              %zu (sizeof %zu B each, "
                    "%.1f MB strings)\n",
                    snap->references.size(), sizeof(Reference),
                    mb(static_cast<int64_t>(ref_string_bytes)));
        std::printf("scopes_by_file entries:  %zu\n", file_scope_entries);

        size_t chain_entries = 0, chain_strings = 0, sym_strings = 0,
               annotations = 0;
        snap->symbols.range([&](SymbolID, const EnhancedSymbol& es) {
            chain_entries += es.scope_chain.size();
            for (const auto& sc : es.scope_chain) {
                chain_strings +=
                    sc.name.size() + sc.full_path.size() + sc.language.size();
            }
            sym_strings += es.symbol.name.size() + es.type_info.size() +
                           es.doc_comment.size() + es.signature.size();
            annotations += es.annotations.size();
            return true;
        });
        size_t line_entries = 0;
        for (const auto& [fid, lm] : snap->line_to_symbols_by_file) {
            for (const auto& [line, v] : lm) line_entries += v.size();
        }
        std::printf("scope_chain entries:     %zu (%.1f MB strings, "
                    "sizeof(ScopeInfo)=%zu)\n",
                    chain_entries, mb(static_cast<int64_t>(chain_strings)),
                    sizeof(ScopeInfo));
        std::printf("symbol strings:          %.1f MB "
                    "(sizeof(EnhancedSymbol)=%zu, %zu annotations)\n",
                    mb(static_cast<int64_t>(sym_strings)),
                    sizeof(EnhancedSymbol), annotations);
        std::printf("line_to_symbols entries: %zu\n", line_entries);
    }

    const int top_n =
        std::min<int>(opts.top, static_cast<int>(deltas.size()));
    std::printf("\ntop %d files by rss delta:\n", top_n);
    for (int i = 0; i < top_n; ++i) {
        const auto& d = deltas[i];
        std::printf("  %8.1f MB (%8" PRId64 " B) %s\n", mb(d.delta_bytes),
                    d.size_bytes, d.path.c_str());
    }
    return 0;
}

}  // namespace cli
}  // namespace lci
