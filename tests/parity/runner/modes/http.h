#pragma once

#include <string>
#include <vector>

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

// Exposed for unit testing. SUNSET-WHEN-GO-UPGRADES (Dart task
// BNXsh3tUpMSW): when Go reference binary adopts the uid-namespaced
// socket path, drop the second candidate.
std::vector<std::string> candidate_socket_paths_for_test(
    const std::string& abs_corpus);

} // namespace lci::parity
