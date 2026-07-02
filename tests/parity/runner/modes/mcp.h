#pragma once

#include "runner/descriptor.h"
#include "runner/modes/cli.h"
#include <string>

namespace lci::parity {

// Drives one MCP descriptor: spawns the binary with `mcp` subcommand,
// performs JSON-RPC initialize, sends the descriptor's tool call,
// captures the response, terminates cleanly. The descriptor's
// invocation.stdin_data is the JSON-RPC request body for the call;
// invocation.args[0] is expected to be "mcp".
CapturedOutput run_mcp(const std::string& binary_path,
                       const Descriptor&  d,
                       const std::string& corpus_path,
                       int                timeout_seconds = 60);

} // namespace lci::parity
