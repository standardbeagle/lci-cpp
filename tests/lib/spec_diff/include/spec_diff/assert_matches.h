// tests/lib/spec_diff/include/spec_diff/assert_matches.h
//
// Convenience wrapper for the common "load a golden file, canonicalize
// my actual output, diff under the descriptor's tier map" pattern that
// integration gtests will use.
//
// This intentionally returns a DiffResult rather than throwing or calling
// gtest macros: the integration suite wires it into ASSERT_TRUE /
// EXPECT_TRUE on its own so spec_diff has zero gtest dependency.
#pragma once

#include "spec_diff/canonicalize.h"
#include "spec_diff/diff.h"

#include <nlohmann/json.hpp>
#include <string>
#include <variant>

namespace spec_diff {

// A bundle of all knobs an integration spec needs to drive canonicalize +
// diff: only the data spec_diff actually consumes — no binary paths, no
// invocation, no mode enum.
struct SpecDescriptor {
    enum class Parse { Json, Text };

    Parse                    parse = Parse::Json;
    TierMap                  tiers;
    TextCanonicalizeOptions  text;          // used when parse == Text
    std::string              corpus_prefix; // used when parse == Json
    double                   score_abs    = 0.01;
    long long                timed_max_ms = 60000;
    std::string              id_pattern;
    // Lines to strip before JSON parsing (parity with the runner's
    // strip_preamble_lines pre-clean). No-op when empty.
    std::vector<std::string> json_preamble_strip;
};

using CanonicalValue = std::variant<nlohmann::json, std::string>;

// Descriptor-driven canonicalization entrypoint for future integration
// tests: normalize one raw output payload into the comparison form implied
// by the spec (JSON value for Parse::Json, normalized text for Parse::Text).
//
// Throws std::runtime_error when Parse::Json input is not valid JSON.
CanonicalValue canonicalize(const std::string& raw,
                            const SpecDescriptor& desc);

// Descriptor-driven diff wrapper over CanonicalValue. Both inputs must be
// produced with the same SpecDescriptor::parse mode.
DiffResult diff(const CanonicalValue& expected,
                const CanonicalValue& actual,
                const SpecDescriptor& desc);

// Load `golden_path` (raw bytes, not canonicalized) and diff against
// `actual_raw` per `desc`. JSON-mode parses both sides, canonicalizes
// per descriptor, then runs tier-aware diff. Text-mode canonicalizes
// both sides via canonicalize_text and does a string equality check
// surfaced as a DiffResult.
//
// Throws std::runtime_error if the golden file cannot be read or if
// JSON parse fails on either side.
DiffResult assert_matches(const std::string& actual_raw,
                          const SpecDescriptor& desc,
                          const std::string& golden_path);

// Same as above but accepts the golden contents directly (useful when
// the caller already loaded them, or wants in-test fixtures without a
// file on disk).
DiffResult assert_matches_with_golden(const std::string& actual_raw,
                                      const std::string& golden_raw,
                                      const SpecDescriptor& desc);

} // namespace spec_diff
