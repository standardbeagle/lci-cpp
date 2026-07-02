#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_core.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include <fuzzer/FuzzedDataProvider.h>

#include "fuzz_fixture.h"

// libFuzzer target: the find_files pattern matcher. find_files runs several
// hand-rolled matchers over every indexed path — glob/wildcard, exact,
// substring, path-component, Levenshtein fuzzy, and a multi-word coverage pass.
// This target drives handle_find_files with fuzzed pattern/filter/flags/
// directory to shake out crashes, OOB reads, and infinite loops in that logic
// against a fixed small corpus.

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 4096) {
        return 0;
    }

    FuzzedDataProvider fdp(data, size);

    nlohmann::json params;
    // Structured fields peeled off the front, pattern gets the remainder so
    // most of the entropy drives the matchers (which key off `pattern`).
    params["flags"] = fdp.ConsumeRandomLengthString(16);
    params["filter"] = fdp.ConsumeRandomLengthString(16);
    params["directory"] = fdp.ConsumeRandomLengthString(16);
    params["include_hidden"] = fdp.ConsumeBool();
    params["max"] = fdp.ConsumeIntegralInRange<int>(-8, 300);
    params["pattern"] = fdp.ConsumeRemainingBytesAsString();

    lci::mcp::handle_find_files(params, lci::fuzz::shared_fuzz_index());
    return 0;
}
