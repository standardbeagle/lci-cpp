#pragma once

#include "runner/descriptor.h"
#include "runner/modes/cli.h"

namespace lci::parity {

// Starts the binary in server mode using the corpus directory as the project
// root (determining the Unix socket path via the same hash used by both
// binaries). Makes the descriptor's HTTP request over the Unix socket, captures
// the response body and HTTP status, then issues a shutdown and waits for the
// server process to exit.
CapturedOutput run_http(const std::string& binary_path,
                        const Descriptor&  d,
                        const std::string& corpus_path);

} // namespace lci::parity
