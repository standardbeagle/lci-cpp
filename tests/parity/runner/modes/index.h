#pragma once

#include "runner/descriptor.h"
#include "runner/modes/cli.h"

namespace lci::parity {

CapturedOutput run_index_export(const std::string& binary_path,
                                const Descriptor&  d,
                                const std::string& corpus_path);

} // namespace lci::parity
