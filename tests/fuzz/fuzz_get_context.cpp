#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_core.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include <fuzzer/FuzzedDataProvider.h>

#include "fuzz_fixture.h"

// libFuzzer target: get_context — the most intricate tool. It decodes packed
// base-63 object IDs, extracts trailing `oid=...` prefixes, normalizes legacy
// param aliases, applies mode presets, resolves symbols by name/id/location,
// and walks the call hierarchy to a bounded depth. Each of those consumes
// caller-controlled strings/ints. This target fuzzes both the raw ID codec and
// the full handle_get_context path over a live symbol graph.

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 4096) {
        return 0;
    }

    FuzzedDataProvider fdp(data, size);

    // Raw base-63 decode on an arbitrary token (the innermost codec).
    std::string id_token = fdp.ConsumeRandomLengthString(32);
    (void)lci::decode_symbol_id(id_token);

    // Full handler path with fuzzed lookup keys and traversal knobs.
    nlohmann::json params;
    params["id"] = id_token;
    params["name"] = fdp.ConsumeRandomLengthString(32);
    params["mode"] = fdp.ConsumeRandomLengthString(12);
    params["file_id"] = fdp.ConsumeIntegralInRange<int>(-4, 64);
    params["line"] = fdp.ConsumeIntegralInRange<int>(-4, 512);
    params["column"] = fdp.ConsumeIntegralInRange<int>(-4, 512);
    params["max_depth"] = fdp.ConsumeIntegralInRange<int>(-2, 32);
    params["include_call_hierarchy"] = fdp.ConsumeBool();
    params["include_all_references"] = fdp.ConsumeBool();

    lci::mcp::handle_get_context(params, lci::fuzz::shared_fuzz_index());
    return 0;
}
