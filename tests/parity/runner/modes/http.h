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

// Send /shutdown over each candidate socket for `corpus_path`, then poll
// for the sockets to disappear. Used by CLI and MCP modes after each
// descriptor: those modes invoke the lci binary which, via
// ensure_server_running() in src/cli/server.cpp, forks a detached
// `setsid()` daemon that survives the CLI subcommand and would otherwise
// be flagged as an orphan by tests/parity/scripts/orphan_cleanup.sh.
// HTTP mode does not need this — it spawns the server itself and reaps
// it via kill_server() and the local RAII guard. Best-effort: never
// throws, no error is propagated; followed by the test-fixture verify
// step which is the actual gate. Karpathy rule 4 — determinism.
void shutdown_corpus_servers(const std::string& corpus_path);

} // namespace lci::parity
