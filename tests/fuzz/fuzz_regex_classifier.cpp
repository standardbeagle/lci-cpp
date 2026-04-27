#include <lci/regex_analyzer/engine.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

/// libFuzzer target: exercises RegexClassifier::is_simple and
/// LiteralExtractor::extract_literals with arbitrary byte sequences.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 4096) {
        return 0;
    }

    std::string_view input(reinterpret_cast<const char*>(data), size);

    lci::RegexClassifier classifier;
    classifier.is_simple(input);

    lci::LiteralExtractor extractor;
    extractor.extract_literals(input);

    return 0;
}
