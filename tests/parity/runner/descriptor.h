#pragma once

#include "diff_engine/diff.h"
#include <map>
#include <string>
#include <vector>

namespace lci::parity {

enum class Mode { Cli, Mcp, Http, Index };
enum class ParseStyle { Json, Text, ExitOnly };

struct Invocation {
    std::vector<std::string> args;
    std::string stdin_data;
    std::map<std::string, std::string> env;
    std::string cwd;        // may contain "${CORPUS}"
};

struct Descriptor {
    std::string id;
    Mode        mode;
    std::string corpus;     // key into corpora/
    std::string go_binary;  // "${LCI_GO}" placeholder allowed
    std::string cpp_binary;
    Invocation  invocation;
    std::vector<std::string> capture;  // "stdout", "stderr", "exit"
    ParseStyle  parse;
    TierMap     tiers;
    struct Tolerances {
        double    score_abs    = 0.01;
        long long timed_max_ms = 60000;
    } tolerances;
    int         expect_exit = 0;
    std::string id_pattern;  // for ids tier (optional)
};

// Throws std::runtime_error on schema or type errors. The input is the raw
// JSON text of one .parity.json file.
Descriptor parse_descriptor(const std::string& json_text);

} // namespace lci::parity
