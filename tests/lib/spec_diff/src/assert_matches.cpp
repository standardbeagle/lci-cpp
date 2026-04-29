// tests/lib/spec_diff/src/assert_matches.cpp
#include "spec_diff/assert_matches.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace spec_diff {

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("spec_diff::assert_matches: cannot open golden: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

DiffOptions diff_opts_from(const SpecDescriptor& d) {
    DiffOptions o;
    o.tiers        = d.tiers;
    o.score_abs    = d.score_abs;
    o.timed_max_ms = d.timed_max_ms;
    o.id_pattern   = d.id_pattern;
    return o;
}

CanonicalizeOptions canon_opts_from(const SpecDescriptor& d) {
    CanonicalizeOptions co;
    co.ignore_paths     = d.tiers.ignore;
    co.sort_array_paths = d.tiers.sort_arrays;
    co.corpus_prefix    = d.corpus_prefix;
    co.preserve_number_paths = d.tiers.ranked;
    co.preserve_number_paths.insert(co.preserve_number_paths.end(),
                                    d.tiers.timed.begin(),
                                    d.tiers.timed.end());
    return co;
}

} // namespace

CanonicalValue canonicalize(const std::string& raw,
                            const SpecDescriptor& desc) {
    if (desc.parse == SpecDescriptor::Parse::Json) {
        std::string cleaned = strip_preamble_lines(raw, desc.json_preamble_strip);
        try {
            return canonicalize_json(nlohmann::json::parse(cleaned), canon_opts_from(desc));
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("spec_diff: JSON parse failed: ") + e.what());
        }
    }

    return canonicalize_text(raw, desc.text);
}

DiffResult diff(const CanonicalValue& expected,
                const CanonicalValue& actual,
                const SpecDescriptor& desc) {
    if (expected.index() != actual.index()) {
        DiffResult r;
        r.passed = false;
        r.reasons.push_back("canonical value type mismatch");
        return r;
    }

    if (desc.parse == SpecDescriptor::Parse::Json) {
        return diff(std::get<nlohmann::json>(expected),
                    std::get<nlohmann::json>(actual),
                    diff_opts_from(desc));
    }

    const auto& expected_text = std::get<std::string>(expected);
    const auto& actual_text = std::get<std::string>(actual);
    DiffResult r;
    if (expected_text == actual_text) return r;
    r.passed = false;
    r.reasons.push_back("text mismatch");
    r.unified_diff = "--- expected\n+++ actual\n" + expected_text + "\n--- vs ---\n" + actual_text;
    return r;
}

DiffResult assert_matches_with_golden(const std::string& actual_raw,
                                      const std::string& golden_raw,
                                      const SpecDescriptor& desc) {
    CanonicalValue actual = canonicalize(actual_raw, desc);
    CanonicalValue golden = canonicalize(golden_raw, desc);
    return diff(golden, actual, desc);
}

DiffResult assert_matches(const std::string& actual_raw,
                          const SpecDescriptor& desc,
                          const std::string& golden_path) {
    return assert_matches_with_golden(actual_raw, slurp(golden_path), desc);
}

} // namespace spec_diff
