#pragma once

#include "runner/descriptor.h"
#include <string>
#include <map>

namespace lci::parity {

struct CapturedOutput {
    std::string stdout_data;
    std::string stderr_data;
    int         exit_code = -1;
    bool        timed_out = false;
};

// Runs the binary at `binary_path` with the descriptor's invocation,
// substituting placeholder env vars. corpus_path is the resolved
// absolute path to the corpus directory (replaces ${CORPUS} in cwd/args).
// timeout_seconds = SIGKILL after this many seconds.
CapturedOutput run_cli(const std::string& binary_path,
                      const Invocation&  inv,
                      const std::string& corpus_path,
                      int                timeout_seconds = 60);

} // namespace lci::parity
