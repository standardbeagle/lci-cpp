// tests/parity/runner/parity_runner.cpp
#include "diff_engine/canonicalize.h"
#include "diff_engine/diff.h"
#include "runner/descriptor.h"
#include "runner/modes/cli.h"
#include "runner/modes/index.h"
#include "runner/modes/http.h"
#include "runner/modes/mcp.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using namespace lci::parity;

namespace {

std::string env_or(const char* name, const std::string& dflt = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : dflt;
}

std::string substitute_binary(const std::string& spec) {
    if (spec == "${LCI_GO}")  return env_or("LCI_GO");
    if (spec == "${LCI_CPP}") return env_or("LCI_CPP");
    return spec;
}

std::string resolve_corpus(const std::string& key) {
    std::string base = env_or("PARITY_CORPORA");
    if (base.empty()) {
        throw std::runtime_error("PARITY_CORPORA env not set");
    }
    fs::path syn = fs::path(base) / "synthetic" / key;
    if (fs::is_directory(syn)) return syn.string();
    return (fs::path(base) / key).string();
}

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Cli:   return "cli";
        case Mode::Mcp:   return "mcp";
        case Mode::Http:  return "http";
        case Mode::Index: return "index";
    }
    return "unknown";
}

McpFraming framing_for_binary(const std::string& bin) {
    auto go = env_or("LCI_GO");
    if (!go.empty() && bin == go) return McpFraming::Ndjson;
    return McpFraming::ContentLength;
}

// Build text-mode canonicalize options from a descriptor, threading the
// runtime corpus path in for path rewriting.  When the descriptor omits
// a text_normalize block we still apply the safe defaults — trailing-
// whitespace trim, timing scrub, and corpus-path rewrite — because these
// are universally correct for text-mode parity comparison.
TextCanonicalizeOptions text_opts_for(const Descriptor& d,
                                      const std::string& corpus_path) {
    TextCanonicalizeOptions o;
    o.scrub_timing       = d.text_normalize.scrub_timing;
    o.strip_emoji_prefix = d.text_normalize.strip_emoji_prefix;
    o.strip_lines        = d.text_normalize.strip_lines;
    o.replace            = d.text_normalize.replace;
    if (d.text_normalize.rewrite_corpus_path) {
        o.corpus_prefix = corpus_path;
    }
    return o;
}

// Drop any line whose content contains one of the strip patterns.  Used
// to pre-clean stdout before JSON parsing when one binary emits debug or
// banner preamble lines that the other does not (e.g. Go's
// "DEBUG: verbose=..." print in `lci search --json`).  Reuses
// canonicalize_text with a strip-only options block so the line-matching
// rules stay identical to text-mode behavior.  No-op when patterns is
// empty.
std::string strip_preamble_lines(const std::string& in,
                                 const std::vector<std::string>& patterns) {
    if (patterns.empty()) return in;
    TextCanonicalizeOptions opts;
    opts.scrub_timing = false;        // leave numeric tokens alone
    opts.strip_emoji_prefix = false;
    opts.strip_lines = patterns;
    // corpus_prefix and replace left empty.
    return canonicalize_text(in, opts);
}

void write_dump(const fs::path& dump_dir,
                const Descriptor& d,
                const std::string& go_raw,
                const std::string& cpp_raw,
                const nlohmann::json& go_canon,
                const nlohmann::json& cpp_canon,
                const DiffResult& dr) {
    fs::create_directories(dump_dir);
    {
        std::ofstream f(dump_dir / "desc.json");
        f << nlohmann::json{{"id", d.id}, {"corpus", d.corpus},
                            {"mode", mode_name(d.mode)}}.dump(2);
    }
    std::ofstream(dump_dir / "go.raw")  << go_raw;
    std::ofstream(dump_dir / "cpp.raw") << cpp_raw;
    std::ofstream(dump_dir / "go.canon.json")  << go_canon.dump(2);
    std::ofstream(dump_dir / "cpp.canon.json") << cpp_canon.dump(2);
    std::ofstream(dump_dir / "diff.txt") << dr.unified_diff;
    std::ofstream rep(dump_dir / "report.txt");
    rep << "test_id: " << d.id << "\n";
    rep << "passed: " << (dr.passed ? "true" : "false") << "\n\n";
    for (const auto& reason : dr.reasons) rep << "- " << reason << "\n";
}

int run_cli_descriptor(const Descriptor& d) {
    std::string corpus_path = resolve_corpus(d.corpus);
    std::string go_bin  = substitute_binary(d.go_binary);
    std::string cpp_bin = substitute_binary(d.cpp_binary);

    if (go_bin.empty() || cpp_bin.empty()) {
        std::cerr << "infra: LCI_GO or LCI_CPP not set\n";
        return 2;
    }

    auto go_out  = run_cli(go_bin,  d.invocation, corpus_path);
    auto cpp_out = run_cli(cpp_bin, d.invocation, corpus_path);

    if (go_out.timed_out || cpp_out.timed_out) {
        std::cerr << "infra: timeout\n";
        return 2;
    }
    if (d.expect_exit != go_out.exit_code || d.expect_exit != cpp_out.exit_code) {
        std::string reason = "exit-code mismatch: expected " + std::to_string(d.expect_exit)
                           + " got go=" + std::to_string(go_out.exit_code)
                           + " cpp=" + std::to_string(cpp_out.exit_code);
        std::cerr << reason << "\n";
        DiffResult dr;
        dr.passed = false;
        dr.reasons.push_back(reason);
        fs::path dump_dir =
            fs::path(env_or("PARITY_FAILURES", "build/parity-failures")) / d.id;
        write_dump(dump_dir, d, go_out.stdout_data, cpp_out.stdout_data,
                   nlohmann::json(nullptr), nlohmann::json(nullptr), dr);
        std::cerr << "dump: " << dump_dir << "\n";
        return 1;
    }

    nlohmann::json go_canon, cpp_canon;
    DiffOptions opts;
    opts.tiers        = d.tiers;
    opts.score_abs    = d.tolerances.score_abs;
    opts.timed_max_ms = d.tolerances.timed_max_ms;
    opts.id_pattern   = d.id_pattern;

    DiffResult dr;
    if (d.parse == ParseStyle::Json) {
        try {
            // Pre-strip preamble lines (e.g. Go's "DEBUG: verbose=..."
            // print in `lci search --json`) before JSON parse.  The
            // descriptor opts in via text_normalize.strip_lines; with no
            // patterns this is a no-op and existing descriptors are
            // unaffected.
            std::string go_raw  = strip_preamble_lines(go_out.stdout_data,
                                                       d.text_normalize.strip_lines);
            std::string cpp_raw = strip_preamble_lines(cpp_out.stdout_data,
                                                       d.text_normalize.strip_lines);
            auto go_j  = nlohmann::json::parse(go_raw);
            auto cpp_j = nlohmann::json::parse(cpp_raw);
            CanonicalizeOptions co;
            co.ignore_paths  = d.tiers.ignore;
            co.sort_array_paths = d.tiers.sort_arrays;
            co.corpus_prefix = corpus_path;
            co.preserve_number_paths = d.tiers.ranked;
            co.preserve_number_paths.insert(co.preserve_number_paths.end(),
                                            d.tiers.timed.begin(),
                                            d.tiers.timed.end());
            go_canon  = canonicalize_json(go_j,  co);
            cpp_canon = canonicalize_json(cpp_j, co);
            dr = compare(go_canon, cpp_canon, opts);
        } catch (const std::exception& e) {
            std::cerr << "infra: json parse failed: " << e.what() << "\n";
            return 2;
        }
    } else if (d.parse == ParseStyle::Text) {
        auto tco = text_opts_for(d, corpus_path);
        std::string a = canonicalize_text(go_out.stdout_data,  tco);
        std::string b = canonicalize_text(cpp_out.stdout_data, tco);
        dr.passed = (a == b);
        if (!dr.passed) {
            dr.reasons.push_back("text mismatch");
            dr.unified_diff = a + "\n--- vs ---\n" + b;
        }
        go_canon  = a;
        cpp_canon = b;
    } else {
        // ExitOnly — already checked above
        dr.passed = true;
    }

    if (!dr.passed) {
        fs::path dump_dir =
            fs::path(env_or("PARITY_FAILURES", "build/parity-failures")) / d.id;
        write_dump(dump_dir, d, go_out.stdout_data, cpp_out.stdout_data,
                   go_canon, cpp_canon, dr);
        std::cerr << "FAIL " << d.id << " (" << dr.reasons.size() << " reasons)\n";
        for (const auto& r : dr.reasons) std::cerr << "  - " << r << "\n";
        std::cerr << "dump: " << dump_dir << "\n";
        return 1;
    }
    std::cout << "PASS " << d.id << "\n";
    return 0;
}

int run_mcp_descriptor(const Descriptor& d) {
    std::string corpus_path = resolve_corpus(d.corpus);
    std::string go_bin  = substitute_binary(d.go_binary);
    std::string cpp_bin = substitute_binary(d.cpp_binary);

    if (go_bin.empty() || cpp_bin.empty()) {
        std::cerr << "infra: LCI_GO or LCI_CPP not set\n";
        return 2;
    }

    auto go  = run_mcp(go_bin,  d, corpus_path, framing_for_binary(go_bin));
    auto cpp = run_mcp(cpp_bin, d, corpus_path, framing_for_binary(cpp_bin));

    if (go.timed_out || cpp.timed_out) {
        std::cerr << "infra: mcp timeout\n";
        return 2;
    }

    DiffOptions opts;
    opts.tiers        = d.tiers;
    opts.score_abs    = d.tolerances.score_abs;
    opts.timed_max_ms = d.tolerances.timed_max_ms;
    opts.id_pattern   = d.id_pattern;

    nlohmann::json go_canon, cpp_canon;
    DiffResult dr;
    try {
        auto gj = nlohmann::json::parse(go.stdout_data);
        auto cj = nlohmann::json::parse(cpp.stdout_data);
        CanonicalizeOptions co;
        co.ignore_paths  = d.tiers.ignore;
        co.sort_array_paths = d.tiers.sort_arrays;
        co.corpus_prefix = corpus_path;
        co.preserve_number_paths = d.tiers.ranked;
        co.preserve_number_paths.insert(co.preserve_number_paths.end(),
                                        d.tiers.timed.begin(),
                                        d.tiers.timed.end());
        go_canon  = canonicalize_json(gj, co);
        cpp_canon = canonicalize_json(cj, co);
        dr = compare(go_canon, cpp_canon, opts);
    } catch (const std::exception& e) {
        std::cerr << "infra: mcp parse failed: " << e.what()
                  << "\ngo: "  << go.stdout_data
                  << "\ncpp: " << cpp.stdout_data << "\n";
        return 2;
    }

    if (!dr.passed) {
        fs::path dump_dir =
            fs::path(env_or("PARITY_FAILURES", "build/parity-failures")) / d.id;
        write_dump(dump_dir, d, go.stdout_data, cpp.stdout_data,
                   go_canon, cpp_canon, dr);
        std::cerr << "FAIL " << d.id << " (" << dr.reasons.size() << " reasons)\n";
        for (const auto& r : dr.reasons) std::cerr << "  - " << r << "\n";
        std::cerr << "dump: " << dump_dir << "\n";
        return 1;
    }
    std::cout << "PASS " << d.id << "\n";
    return 0;
}

int run_index_descriptor(const Descriptor& d) {
    std::string corpus_path = resolve_corpus(d.corpus);
    std::string go_bin  = substitute_binary(d.go_binary);
    std::string cpp_bin = substitute_binary(d.cpp_binary);

    if (go_bin.empty() || cpp_bin.empty()) {
        std::cerr << "infra: LCI_GO or LCI_CPP not set\n";
        return 2;
    }

    auto go_out  = run_index_export(go_bin,  d, corpus_path);
    auto cpp_out = run_index_export(cpp_bin, d, corpus_path);

    if (go_out.timed_out || cpp_out.timed_out) {
        std::cerr << "infra: index export timeout\n";
        return 2;
    }
    if (d.expect_exit != go_out.exit_code || d.expect_exit != cpp_out.exit_code) {
        std::string reason = "exit-code mismatch: expected " + std::to_string(d.expect_exit)
                           + " got go=" + std::to_string(go_out.exit_code)
                           + " cpp=" + std::to_string(cpp_out.exit_code);
        std::cerr << reason << "\n";
        DiffResult dr;
        dr.passed = false;
        dr.reasons.push_back(reason);
        fs::path dump_dir =
            fs::path(env_or("PARITY_FAILURES", "build/parity-failures")) / d.id;
        write_dump(dump_dir, d, go_out.stdout_data, cpp_out.stdout_data,
                   nlohmann::json(nullptr), nlohmann::json(nullptr), dr);
        std::cerr << "dump: " << dump_dir << "\n";
        return 1;
    }

    nlohmann::json go_canon, cpp_canon;
    DiffOptions opts;
    opts.tiers        = d.tiers;
    opts.score_abs    = d.tolerances.score_abs;
    opts.timed_max_ms = d.tolerances.timed_max_ms;
    opts.id_pattern   = d.id_pattern;

    DiffResult dr;
    try {
        auto go_j  = nlohmann::json::parse(go_out.stdout_data);
        auto cpp_j = nlohmann::json::parse(cpp_out.stdout_data);
        CanonicalizeOptions co;
        co.ignore_paths  = d.tiers.ignore;
        co.sort_array_paths = d.tiers.sort_arrays;
        co.corpus_prefix = corpus_path;
        co.preserve_number_paths = d.tiers.ranked;
        co.preserve_number_paths.insert(co.preserve_number_paths.end(),
                                        d.tiers.timed.begin(),
                                        d.tiers.timed.end());
        go_canon  = canonicalize_json(go_j,  co);
        cpp_canon = canonicalize_json(cpp_j, co);
        dr = compare(go_canon, cpp_canon, opts);
    } catch (const std::exception& e) {
        std::cerr << "infra: index export json parse failed: " << e.what()
                  << "\ngo: "  << go_out.stdout_data.substr(0, 200)
                  << "\ncpp: " << cpp_out.stdout_data.substr(0, 200) << "\n";
        return 2;
    }

    if (!dr.passed) {
        fs::path dump_dir =
            fs::path(env_or("PARITY_FAILURES", "build/parity-failures")) / d.id;
        write_dump(dump_dir, d, go_out.stdout_data, cpp_out.stdout_data,
                   go_canon, cpp_canon, dr);
        std::cerr << "FAIL " << d.id << " (" << dr.reasons.size() << " reasons)\n";
        for (const auto& r : dr.reasons) std::cerr << "  - " << r << "\n";
        std::cerr << "dump: " << dump_dir << "\n";
        return 1;
    }
    std::cout << "PASS " << d.id << "\n";
    return 0;
}

int run_http_descriptor(const Descriptor& d) {
    std::string corpus_path = resolve_corpus(d.corpus);
    std::string go_bin  = substitute_binary(d.go_binary);
    std::string cpp_bin = substitute_binary(d.cpp_binary);

    if (go_bin.empty() || cpp_bin.empty()) {
        std::cerr << "infra: LCI_GO or LCI_CPP not set\n";
        return 2;
    }

    // Run sequentially: Go then C++. Both servers use the same socket path
    // (derived from corpus_path via an identical hash in both binaries), so
    // they must not run concurrently.
    auto go_out  = run_http(go_bin,  d, corpus_path);
    auto cpp_out = run_http(cpp_bin, d, corpus_path);

    if (go_out.timed_out || cpp_out.timed_out) {
        std::cerr << "infra: http server timeout\n";
        return 2;
    }

    // For HTTP mode, expect_exit holds the expected HTTP status code.
    if (d.expect_exit != go_out.exit_code || d.expect_exit != cpp_out.exit_code) {
        std::string reason = "http-status mismatch: expected " +
                             std::to_string(d.expect_exit) +
                             " got go=" + std::to_string(go_out.exit_code) +
                             " cpp=" + std::to_string(cpp_out.exit_code);
        std::cerr << reason << "\n";
        DiffResult dr;
        dr.passed = false;
        dr.reasons.push_back(reason);
        fs::path dump_dir =
            fs::path(env_or("PARITY_FAILURES", "build/parity-failures")) / d.id;
        write_dump(dump_dir, d, go_out.stdout_data, cpp_out.stdout_data,
                   nlohmann::json(nullptr), nlohmann::json(nullptr), dr);
        std::cerr << "dump: " << dump_dir << "\n";
        return 1;
    }

    DiffOptions opts;
    opts.tiers        = d.tiers;
    opts.score_abs    = d.tolerances.score_abs;
    opts.timed_max_ms = d.tolerances.timed_max_ms;
    opts.id_pattern   = d.id_pattern;

    nlohmann::json go_canon, cpp_canon;
    DiffResult dr;
    if (d.parse == ParseStyle::Json) {
        try {
            auto go_j  = nlohmann::json::parse(go_out.stdout_data);
            auto cpp_j = nlohmann::json::parse(cpp_out.stdout_data);
            CanonicalizeOptions co;
            co.ignore_paths  = d.tiers.ignore;
            co.sort_array_paths = d.tiers.sort_arrays;
            co.corpus_prefix = corpus_path;
            co.preserve_number_paths = d.tiers.ranked;
            co.preserve_number_paths.insert(co.preserve_number_paths.end(),
                                            d.tiers.timed.begin(),
                                            d.tiers.timed.end());
            go_canon  = canonicalize_json(go_j,  co);
            cpp_canon = canonicalize_json(cpp_j, co);
            dr = compare(go_canon, cpp_canon, opts);
        } catch (const std::exception& e) {
            std::cerr << "infra: http json parse failed: " << e.what()
                      << "\ngo: "  << go_out.stdout_data.substr(0, 200)
                      << "\ncpp: " << cpp_out.stdout_data.substr(0, 200) << "\n";
            return 2;
        }
    } else if (d.parse == ParseStyle::Text) {
        auto tco = text_opts_for(d, corpus_path);
        std::string a = canonicalize_text(go_out.stdout_data,  tco);
        std::string b = canonicalize_text(cpp_out.stdout_data, tco);
        dr.passed = (a == b);
        if (!dr.passed) {
            dr.reasons.push_back("text mismatch");
            dr.unified_diff = a + "\n--- vs ---\n" + b;
        }
        go_canon  = a;
        cpp_canon = b;
    } else {
        // ExitOnly — status code already checked above.
        dr.passed = true;
    }

    if (!dr.passed) {
        fs::path dump_dir =
            fs::path(env_or("PARITY_FAILURES", "build/parity-failures")) / d.id;
        write_dump(dump_dir, d, go_out.stdout_data, cpp_out.stdout_data,
                   go_canon, cpp_canon, dr);
        std::cerr << "FAIL " << d.id << " (" << dr.reasons.size() << " reasons)\n";
        for (const auto& r : dr.reasons) std::cerr << "  - " << r << "\n";
        std::cerr << "dump: " << dump_dir << "\n";
        return 1;
    }
    std::cout << "PASS " << d.id << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"lci parity runner"};
    std::string desc_path;
    app.add_option("descriptor", desc_path, "Path to .parity.json")->required();
    CLI11_PARSE(app, argc, argv);

    std::ifstream f(desc_path);
    if (!f) { std::cerr << "cannot open " << desc_path << "\n"; return 2; }
    std::stringstream ss; ss << f.rdbuf();
    Descriptor d;
    try {
        d = parse_descriptor(ss.str());
    } catch (const std::exception& e) {
        std::cerr << "descriptor parse error: " << e.what() << "\n";
        return 2;
    }

    switch (d.mode) {
        case Mode::Cli:   return run_cli_descriptor(d);
        case Mode::Mcp:   return run_mcp_descriptor(d);
        case Mode::Http:  return run_http_descriptor(d);
        case Mode::Index: return run_index_descriptor(d);
    }
    return 2;
}
