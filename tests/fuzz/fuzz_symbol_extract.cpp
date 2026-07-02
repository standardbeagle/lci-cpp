#include <lci/indexing/master_index.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include <fuzzer/FuzzedDataProvider.h>

#include "fuzz_fixture.h"

// libFuzzer target: the symbol-extraction engine — the most complex internal
// path. Feeds arbitrary bytes as source-file content through the tree-sitter
// extractor for a rotating set of languages, exercising per-language grammar
// parsing, symbol extraction, and reference-graph construction against
// hostile / malformed / truncated source.
//
// One long-lived index is reused: update_file() on a fixed per-language path
// replaces that file's symbols each iteration, so the index does not grow
// unbounded across runs.

namespace {

// Fixed filenames per language extension. update_file on the same path is a
// replace, keeping the working set bounded regardless of iteration count.
constexpr const char* kPaths[] = {
    "fuzz.go",   "fuzz.py",   "fuzz.ts",  "fuzz.tsx", "fuzz.js",
    "fuzz.rs",   "fuzz.java", "fuzz.cpp", "fuzz.c",   "fuzz.cs",
    "fuzz.rb",   "fuzz.kt",   "fuzz.php",
};
constexpr size_t kNumPaths = sizeof(kPaths) / sizeof(kPaths[0]);

// Empty index (no seeded corpus): this target fills it with fuzzed content.
lci::MasterIndex& index() {
    static lci::MasterIndex* idx = [] {
        static lci::Config cfg = lci::fuzz::make_fuzz_config();
        return new lci::MasterIndex(cfg);
    }();
    return *idx;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 65536) {
        return 0;
    }

    FuzzedDataProvider fdp(data, size);
    size_t lang = fdp.ConsumeIntegralInRange<size_t>(0, kNumPaths - 1);
    std::string content = fdp.ConsumeRemainingBytesAsString();

    index().update_file(kPaths[lang], content);
    return 0;
}
