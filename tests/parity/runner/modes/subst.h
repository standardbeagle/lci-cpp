#pragma once

#include <string>

namespace lci::parity {

// Replace ${CORPUS} occurrences in `s` with `corpus_path`.
std::string substitute(const std::string& s, const std::string& corpus_path);

} // namespace lci::parity
