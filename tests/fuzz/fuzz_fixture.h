#pragma once

// Shared fixtures for the index-backed libFuzzer targets. Each fuzz executable
// is its own translation unit, so the small corpus builder lives in a header
// rather than being copy-pasted per target. Kept deliberately tiny: the goal is
// a realistic-but-cheap symbol graph (multiple languages, calls, a type with a
// method) so the fuzzed tool handlers have real data to traverse, not a large
// corpus that slows every iteration.

#include <lci/config.h>
#include <lci/indexing/master_index.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace lci {
namespace fuzz {

/// Per-process temp directory holding the seeded corpus. Also serves as the
/// config's project root (see make_fuzz_config).
inline const std::filesystem::path& fuzz_root() {
    static const std::filesystem::path dir = [] {
        auto d = std::filesystem::temp_directory_path() /
                 ("lci-fuzz-" + std::to_string(::getpid()));
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

/// A default config with a bounded result cap so fuzzed queries can't ask the
/// engine to materialize huge result sets on every iteration.
///
/// project.root is pinned to the seeded temp dir, NOT the inherited CWD.
/// make_default_config() roots at the working directory, which for a fuzzer
/// launched from the repo checkout is the multi-GB repo itself: any fuzzed
/// call reaching a root-scanning path then grows the shared index across
/// iterations until response serialization alone blows the per-input timeout
/// (found as a stateful fuzz_mcp_dispatch libFuzzer timeout whose 387-byte
/// artifact replayed clean in 539 ms), and any root-relative write path
/// (context manifest save) aims at the checkout.
inline Config make_fuzz_config() {
    Config cfg = make_default_config();
    cfg.search.max_results = 16;
    cfg.project.root = fuzz_root().string();
    return cfg;
}

/// Materializes one corpus file on disk and indexes it, aborting loudly on
/// failure: MasterIndex::update_file requires the path to exist on the
/// filesystem, so an unchecked relative-path seed silently no-ops and every
/// fuzz iteration then runs against an EMPTY index — fuzzing nothing.
inline void seed_file(MasterIndex& index, const std::filesystem::path& dir,
                      const char* rel, const std::string& content) {
    const auto path = dir / rel;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << content;
    if (!index.update_file(path.string(), content)) {
        std::fprintf(stderr, "fuzz fixture: failed to seed %s — aborting\n",
                     path.string().c_str());
        std::abort();
    }
}

/// Seeds an index with a small multi-language corpus: Go (type + method +
/// calls), Python, and TypeScript. Enough to exercise symbol lookup, call
/// hierarchy, references, and path matching in the tool handlers. Files are
/// written to a per-process temp directory because indexing verifies disk
/// existence; the directory is process-lifetime, like the index it feeds.
inline void seed_fuzz_corpus(MasterIndex& index) {
    const std::filesystem::path& dir = fuzz_root();
    seed_file(
        index, dir, "router.go",
        "package server\n\n"
        "type Router struct{ routes int }\n\n"
        "func NewRouter() *Router { return &Router{} }\n\n"
        "func (r *Router) Handle(path string) int {\n"
        "    return r.routes + len(path)\n"
        "}\n\n"
        "func main() {\n"
        "    r := NewRouter()\n"
        "    r.Handle(\"/\")\n"
        "}\n");
    seed_file(
        index, dir, "util/strings.py",
        "def normalize(s):\n"
        "    return s.strip().lower()\n\n"
        "def tokenize(text):\n"
        "    return [normalize(w) for w in text.split()]\n");
    seed_file(
        index, dir, "web/app.ts",
        "export class App {\n"
        "  private count = 0;\n"
        "  handle(req: string): number {\n"
        "    return this.count + req.length;\n"
        "  }\n"
        "}\n\n"
        "export function boot(): App { return new App(); }\n");
}

/// Lazily-built, process-lifetime index shared across fuzz iterations. Building
/// tree-sitter parses once (not per input) keeps the fuzzer fast.
inline MasterIndex& shared_fuzz_index() {
    static MasterIndex* index = [] {
        static Config cfg = make_fuzz_config();
        auto* idx = new MasterIndex(cfg);
        seed_fuzz_corpus(*idx);
        return idx;
    }();
    return *index;
}

}  // namespace fuzz
}  // namespace lci
