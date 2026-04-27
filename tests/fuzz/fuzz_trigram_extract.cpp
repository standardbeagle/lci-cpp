#include <lci/core/trigram.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

/// libFuzzer target: exercises trigram extraction (ASCII and Unicode)
/// with arbitrary byte sequences.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 8192) {
        return 0;
    }

    std::string_view input(reinterpret_cast<const char*>(data), size);

    lci::is_pure_ascii(input);
    lci::extract_simple_trigrams(input);

    if (!lci::is_pure_ascii(input)) {
        lci::extract_unicode_trigrams(input);
    }

    return 0;
}
