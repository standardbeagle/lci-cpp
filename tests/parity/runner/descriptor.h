#pragma once

#include "diff_engine/canonicalize.h"
#include "diff_engine/diff.h"
#include <map>
#include <string>
#include <utility>
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

// Descriptor-level text-mode knobs.  These mirror the runtime
// TextCanonicalizeOptions but with raw JSON-friendly types: when the
// runner constructs a TextCanonicalizeOptions for a comparison it copies
// these and fills in the corpus_prefix from the runtime corpus path.
struct DescriptorTextNormalize {
    bool                                                explicitly_set = false;
    bool                                                scrub_timing       = true;
    bool                                                rewrite_corpus_path = true;
    bool                                                strip_emoji_prefix = false;
    std::vector<std::string>                            strip_lines;
    std::vector<std::pair<std::string, std::string>>    replace;
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
    DescriptorTextNormalize text_normalize;
};

// Throws std::runtime_error on schema or type errors. The input is the raw
// JSON text of one .parity.json file.
Descriptor parse_descriptor(const std::string& json_text);

} // namespace lci::parity
