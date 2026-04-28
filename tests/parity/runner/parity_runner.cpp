// tests/parity/runner/parity_runner.cpp
#include "diff_engine/canonicalize.h"
#include "diff_engine/diff.h"
#include "runner/descriptor.h"
#include "runner/modes/cli.h"

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
                            {"mode", "cli"}}.dump(2);
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
            auto go_j  = nlohmann::json::parse(go_out.stdout_data);
            auto cpp_j = nlohmann::json::parse(cpp_out.stdout_data);
            CanonicalizeOptions co;
            co.ignore_paths  = d.tiers.ignore;
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
        std::string a = canonicalize_text(go_out.stdout_data);
        std::string b = canonicalize_text(cpp_out.stdout_data);
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
        case Mode::Mcp:
        case Mode::Http:
        case Mode::Index:
            std::cerr << "mode not yet implemented in this phase\n";
            return 2;
    }
    return 2;
}
