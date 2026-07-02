#pragma once

// Shared fixtures for the index-backed libFuzzer targets. Each fuzz executable
// is its own translation unit, so the small corpus builder lives in a header
// rather than being copy-pasted per target. Kept deliberately tiny: the goal is
// a realistic-but-cheap symbol graph (multiple languages, calls, a type with a
// method) so the fuzzed tool handlers have real data to traverse, not a large
// corpus that slows every iteration.

#include <lci/config.h>
#include <lci/indexing/master_index.h>

#include <string_view>

namespace lci {
namespace fuzz {

/// A default config with a bounded result cap so fuzzed queries can't ask the
/// engine to materialize huge result sets on every iteration.
inline Config make_fuzz_config() {
    Config cfg = make_default_config();
    cfg.search.max_results = 16;
    return cfg;
}

/// Seeds an index with a small multi-language corpus: Go (type + method +
/// calls), Python, and TypeScript. Enough to exercise symbol lookup, call
/// hierarchy, references, and path matching in the tool handlers.
inline void seed_fuzz_corpus(MasterIndex& index) {
    index.update_file(
        "router.go",
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
    index.update_file(
        "util/strings.py",
        "def normalize(s):\n"
        "    return s.strip().lower()\n\n"
        "def tokenize(text):\n"
        "    return [normalize(w) for w in text.split()]\n");
    index.update_file(
        "web/app.ts",
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
