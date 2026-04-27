#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_options.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

/// Lazy-initialized MasterIndex with a small pre-built index.
/// Kept alive across invocations for efficiency.
lci::MasterIndex& get_index() {
    static auto* index = [] {
        auto cfg = lci::make_default_config();
        cfg.search.max_results = 5;
        auto* idx = new lci::MasterIndex(cfg);
        idx->update_file("test.go", "package main\n\nfunc handler() {}\n");
        idx->update_file("util.go",
                         "package util\n\nfunc process(s string) int {\n"
                         "    return len(s)\n}\n");
        return idx;
    }();
    return *index;
}

}  // namespace

/// libFuzzer target: exercises search input validation and execution
/// with arbitrary query strings.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 2048) {
        return 0;
    }

    std::string pattern(reinterpret_cast<const char*>(data), size);

    lci::SearchOptions opts;
    opts.max_results = 5;

    auto& index = get_index();
    index.search_with_options(pattern, opts);

    return 0;
}
