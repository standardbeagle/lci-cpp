#include <lci/config.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

// parse_kdl_content overlays $XDG_CONFIG_HOME/lci/config.kdl on every call.
// Point it at a nonexistent directory once so each iteration short-circuits
// the overlay instead of re-reading the developer's real user config.
[[maybe_unused]] const bool kEnvPinned = [] {
    ::setenv("XDG_CONFIG_HOME", "/nonexistent-lci-fuzz", 1);
    return true;
}();

}  // namespace

/// libFuzzer target: the .lci.kdl config parser. A project config is
/// untrusted input (cloned repos carry their own), so the lexer, parser,
/// and typed node-application (get_int / enum mapping / include-exclude
/// lists) must never crash, overflow, or hang on arbitrary bytes.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 64 * 1024) {
        return 0;
    }

    std::string content(reinterpret_cast<const char*>(data), size);
    std::string error;
    auto cfg = lci::parse_kdl_content(content, error);
    (void)cfg;
    return 0;
}
