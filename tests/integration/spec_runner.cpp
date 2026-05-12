#include "integration/spec_runner.h"

#include "runner/descriptor.h"
#include "runner/modes/cli.h"
#include "runner/modes/http.h"
#include "runner/modes/index.h"
#include "runner/modes/mcp.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <unistd.h>

namespace fs = std::filesystem;

namespace lci::integration {
namespace {

using lci::parity::CapturedOutput;
using lci::parity::Descriptor;
using lci::parity::Invocation;
using lci::parity::McpFraming;
using lci::parity::Mode;
using lci::parity::ParseStyle;
using lci::parity::parse_descriptor;
using lci::parity::run_cli;
using lci::parity::run_http;
using lci::parity::run_index_export;
using lci::parity::run_mcp;

fs::path TestsSourceDir() {
    return fs::path(LCI_TESTS_SOURCE_DIR);
}

fs::path ExecutablePath() {
    std::error_code ec;
    fs::path path = fs::read_symlink("/proc/self/exe", ec);
    if (ec) {
        throw std::runtime_error("cannot resolve /proc/self/exe: " + ec.message());
    }
    return path;
}

std::string Slurp(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WriteFile(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write file: " + path.string());
    }
    output << contents;
}

std::string ResolveCorpus(const std::string& key) {
    const char* env = std::getenv("PARITY_CORPORA");
    const fs::path base = env ? fs::path(env) : (TestsSourceDir() / "parity" / "corpora");
    const fs::path synthetic = base / "synthetic" / key;
    if (fs::is_directory(synthetic)) {
        return synthetic.string();
    }
    // Spec-local corpora: a key containing a slash is interpreted as a path
    // relative to LCI_TESTS_SOURCE_DIR so suites can co-locate corpus
    // fixtures with their descriptors (e.g. `integration/cli/grep_compat/corpus`
    // for the grep_compat suite). Falls through to the legacy
    // `<base>/<key>` resolution if the relative path does not exist.
    if (key.find('/') != std::string::npos) {
        const fs::path local = TestsSourceDir() / key;
        if (fs::is_directory(local)) {
            return local.string();
        }
    }
    return (base / key).string();
}

std::string ResolveCppBinary() {
    if (const char* env = std::getenv("LCI_CPP")) {
        return std::string(env);
    }

    const fs::path exe = ExecutablePath();
    const fs::path build_dir = exe.parent_path().parent_path();
    const fs::path binary = build_dir / "src" / "lci";
    if (!fs::exists(binary)) {
        throw std::runtime_error("cannot locate lci binary at " + binary.string());
    }
    return binary.string();
}

std::string ReadDescriptor(const fs::path& path) {
    return Slurp(path);
}

spec_diff::TextCanonicalizeOptions TextOptionsFor(const Descriptor& descriptor,
                                                  const std::string& corpus_path) {
    spec_diff::TextCanonicalizeOptions options;
    options.scrub_timing = descriptor.text_normalize.scrub_timing;
    options.strip_emoji_prefix = descriptor.text_normalize.strip_emoji_prefix;
    options.strip_lines = descriptor.text_normalize.strip_lines;
    options.replace = descriptor.text_normalize.replace;
    options.sort_lines = descriptor.text_normalize.sort_lines;
    options.collapse_blank_lines = descriptor.text_normalize.collapse_blank_lines;
    if (descriptor.text_normalize.rewrite_corpus_path) {
        options.corpus_prefix = corpus_path;
    }
    return options;
}

spec_diff::SpecDescriptor::Parse InferParseMode(
    const Descriptor& descriptor,
    const SpecCase& spec_case,
    const fs::path& golden_path) {
    if (spec_case.parse_override.has_value()) {
        return *spec_case.parse_override;
    }

    switch (descriptor.parse) {
        case ParseStyle::Json:
            return spec_diff::SpecDescriptor::Parse::Json;
        case ParseStyle::Text:
            return spec_diff::SpecDescriptor::Parse::Text;
        case ParseStyle::ExitOnly:
            return golden_path.extension() == ".json"
                       ? spec_diff::SpecDescriptor::Parse::Json
                       : spec_diff::SpecDescriptor::Parse::Text;
    }

    return spec_diff::SpecDescriptor::Parse::Text;
}

spec_diff::SpecDescriptor ToSpecDescriptor(const Descriptor& descriptor,
                                           const SpecCase& spec_case,
                                           const std::string& corpus_path,
                                           const fs::path& golden_path) {
    spec_diff::SpecDescriptor spec;
    spec.parse = InferParseMode(descriptor, spec_case, golden_path);
    spec.tiers = descriptor.tiers;
    spec.text = TextOptionsFor(descriptor, corpus_path);
    spec.corpus_prefix = corpus_path;
    spec.score_abs = descriptor.tolerances.score_abs;
    spec.timed_max_ms = descriptor.tolerances.timed_max_ms;
    spec.id_pattern = descriptor.id_pattern;
    spec.json_preamble_strip = descriptor.text_normalize.strip_lines;
    return spec;
}

fs::path MakeTempArtifactPath(const fs::path& golden_path) {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("lci-spec-runner-" + std::to_string(::getpid()) + "-" +
            std::to_string(counter++) + golden_path.extension().string());
}

std::optional<fs::path> RewriteOutputArgument(Descriptor& descriptor,
                                              const fs::path& golden_path) {
    fs::path artifact_path = MakeTempArtifactPath(golden_path);
    auto& args = descriptor.invocation.args;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--output" && i + 1 < args.size()) {
            args[i + 1] = artifact_path.string();
            return artifact_path;
        }
        constexpr std::string_view prefix = "--output=";
        if (args[i].rfind(prefix.data(), 0) == 0) {
            args[i] = std::string(prefix) + artifact_path.string();
            return artifact_path;
        }
    }
    throw std::runtime_error("descriptor does not define an --output argument");
}

CapturedOutput RunCppSide(const std::string& cpp_binary,
                          const std::string& corpus_path,
                          const Descriptor& descriptor) {
    switch (descriptor.mode) {
        case Mode::Cli:
            return run_cli(cpp_binary, descriptor.invocation, corpus_path);
        case Mode::Http:
            return run_http(cpp_binary, descriptor, corpus_path);
        case Mode::Mcp:
            return run_mcp(cpp_binary, descriptor, corpus_path,
                           McpFraming::ContentLength);
        case Mode::Index:
            return run_index_export(cpp_binary, descriptor, corpus_path);
    }
    throw std::runtime_error("unsupported descriptor mode");
}

std::string LoadActualRaw(const CapturedOutput& capture,
                          const std::optional<fs::path>& artifact_path,
                          const SpecCase& spec_case) {
    if (spec_case.actual_source == SpecCase::ActualSource::Stdout) {
        return capture.stdout_data;
    }
    if (!artifact_path.has_value()) {
        throw std::runtime_error("artifact path was not prepared");
    }
    return Slurp(*artifact_path);
}

std::string SerializeCanonical(const spec_diff::CanonicalValue& value) {
    if (std::holds_alternative<nlohmann::json>(value)) {
        return std::get<nlohmann::json>(value).dump(2) + "\n";
    }
    return std::get<std::string>(value);
}

}  // namespace

void ExpectSpecMatches(const SpecCase& spec_case) {
    const fs::path descriptor_path =
        TestsSourceDir() / spec_case.descriptor_rel_path;
    const fs::path golden_path =
        TestsSourceDir() / spec_case.golden_rel_path;

    SCOPED_TRACE("descriptor: " + descriptor_path.string());
    SCOPED_TRACE("golden: " + golden_path.string());

    Descriptor descriptor;
    try {
        descriptor = parse_descriptor(ReadDescriptor(descriptor_path));
    } catch (const std::exception& error) {
        FAIL() << "Failed to parse descriptor: " << error.what();
    }

    std::string corpus_path;
    std::string cpp_binary;
    try {
        corpus_path = ResolveCorpus(descriptor.corpus);
        cpp_binary = ResolveCppBinary();
    } catch (const std::exception& error) {
        FAIL() << "Missing runtime configuration: " << error.what();
    }

    Descriptor exec_descriptor = descriptor;
    std::optional<fs::path> artifact_path;
    try {
        if (spec_case.actual_source == SpecCase::ActualSource::OutputFile) {
            artifact_path = RewriteOutputArgument(exec_descriptor, golden_path);
        }
    } catch (const std::exception& error) {
        FAIL() << "Failed to prepare artifact capture: " << error.what();
    }

    CapturedOutput capture;
    try {
        capture = RunCppSide(cpp_binary, corpus_path, exec_descriptor);
    } catch (const std::exception& error) {
        FAIL() << "Failed to execute descriptor: " << error.what();
    }

    ASSERT_FALSE(capture.timed_out) << "Descriptor timed out";
    if (descriptor.mode != Mode::Mcp) {
        ASSERT_EQ(capture.exit_code, descriptor.expect_exit)
            << "stderr:\n" << capture.stderr_data << "\nstdout:\n" << capture.stdout_data;
    }

    std::string actual_raw;
    try {
        actual_raw = LoadActualRaw(capture, artifact_path, spec_case);
    } catch (const std::exception& error) {
        FAIL() << "Failed to load actual output: " << error.what();
    }

    const auto spec_descriptor =
        ToSpecDescriptor(descriptor, spec_case, corpus_path, golden_path);

    if (std::getenv("LCI_UPDATE_GOLDENS") != nullptr) {
        try {
            WriteFile(golden_path,
                      SerializeCanonical(spec_diff::canonicalize(actual_raw,
                                                                 spec_descriptor)));
        } catch (const std::exception& error) {
            FAIL() << "Failed to update golden: " << error.what();
        }
    }

    spec_diff::DiffResult diff;
    try {
        diff = spec_diff::assert_matches(actual_raw, spec_descriptor,
                                         golden_path.string());
    } catch (const std::exception& error) {
        FAIL() << "Failed to compare against golden: " << error.what();
    }

    EXPECT_TRUE(diff.passed)
        << "Reasons:\n"
        << (diff.reasons.empty() ? std::string("(none)")
                                 : [&diff] {
                                       std::ostringstream out;
                                       for (const auto& reason : diff.reasons) {
                                           out << "- " << reason << '\n';
                                       }
                                       return out.str();
                                   }())
        << "\nUnified diff:\n"
        << diff.unified_diff
        << "\nstderr:\n"
        << capture.stderr_data;

    if (artifact_path.has_value()) {
        std::error_code ec;
        fs::remove(*artifact_path, ec);
    }
}

namespace {

// Resolve a "golden" field value from a spec.json. Accepts:
//   * absolute paths (used as-is)
//   * paths starting with "tests/" or "integration/" — resolved against
//     the tests/ source directory
//   * any other relative path — resolved first against the spec file's
//     directory, falling back to tests/integration/<value>.
//
// Returns a path relative to the tests/ source directory (the form
// SpecCase.golden_rel_path expects). Throws if the value is empty.
std::string ResolveGoldenPath(const fs::path& spec_path,
                              const std::string& golden_value) {
    if (golden_value.empty()) {
        throw std::runtime_error("spec missing required field: golden");
    }

    const fs::path tests_dir = TestsSourceDir();
    fs::path resolved;
    fs::path candidate(golden_value);

    if (candidate.is_absolute()) {
        resolved = candidate;
    } else {
        // Try sibling-of-spec first.
        const fs::path sibling = spec_path.parent_path() / candidate;
        if (fs::exists(sibling) || fs::exists(sibling.parent_path())) {
            resolved = sibling;
        } else if (golden_value.rfind("tests/", 0) == 0) {
            // Path expressed from repo root: strip "tests/" and resolve under
            // tests_dir.
            resolved = tests_dir / fs::path(golden_value.substr(6));
        } else {
            // Fallback: treat as relative to tests/integration/.
            resolved = tests_dir / "integration" / candidate;
        }
    }

    // Convert back to a path relative to tests_dir (SpecCase.golden_rel_path
    // is always interpreted as TestsSourceDir() / golden_rel_path).
    std::error_code ec;
    fs::path rel = fs::relative(resolved, tests_dir, ec);
    if (ec || rel.empty() || *rel.begin() == "..") {
        // Could not be expressed under tests_dir; fall through to absolute.
        return resolved.lexically_normal().string();
    }
    return rel.lexically_normal().string();
}

SpecCase::ActualSource ParseActualSource(const std::string& value) {
    if (value == "stdout") return SpecCase::ActualSource::Stdout;
    if (value == "output_file") return SpecCase::ActualSource::OutputFile;
    throw std::runtime_error(
        "spec field 'actual_source' must be 'stdout' or 'output_file' (got '"
        + value + "')");
}

std::optional<spec_diff::SpecDescriptor::Parse> ParseParseOverride(
    const std::string& value) {
    if (value == "json") return spec_diff::SpecDescriptor::Parse::Json;
    if (value == "text") return spec_diff::SpecDescriptor::Parse::Text;
    throw std::runtime_error(
        "spec field 'parse_override' must be 'json' or 'text' (got '" + value
        + "')");
}

}  // namespace

SpecCase LoadIntegrationSpec(const fs::path& spec_path) {
    if (!fs::exists(spec_path)) {
        throw std::runtime_error("spec file does not exist: "
                                 + spec_path.string());
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(Slurp(spec_path));
    } catch (const std::exception& error) {
        throw std::runtime_error("failed to parse spec " + spec_path.string()
                                 + ": " + error.what());
    }

    if (!j.is_object()) {
        throw std::runtime_error("spec must be a JSON object: "
                                 + spec_path.string());
    }

    if (!j.contains("golden") || !j.at("golden").is_string()) {
        throw std::runtime_error(
            "spec missing required string field 'golden': "
            + spec_path.string());
    }

    SpecCase spec_case;

    const fs::path tests_dir = TestsSourceDir();
    std::error_code ec;
    fs::path desc_rel = fs::relative(spec_path, tests_dir, ec);
    if (ec || desc_rel.empty()) {
        throw std::runtime_error(
            "spec path is not under LCI_TESTS_SOURCE_DIR: "
            + spec_path.string());
    }
    spec_case.descriptor_rel_path = desc_rel.lexically_normal().string();

    spec_case.golden_rel_path = ResolveGoldenPath(
        spec_path, j.at("golden").get<std::string>());

    if (j.contains("actual_source")) {
        if (!j.at("actual_source").is_string()) {
            throw std::runtime_error(
                "spec field 'actual_source' must be a string: "
                + spec_path.string());
        }
        spec_case.actual_source =
            ParseActualSource(j.at("actual_source").get<std::string>());
    }

    if (j.contains("parse_override")) {
        if (!j.at("parse_override").is_string()) {
            throw std::runtime_error(
                "spec field 'parse_override' must be a string: "
                + spec_path.string());
        }
        spec_case.parse_override =
            ParseParseOverride(j.at("parse_override").get<std::string>());
    }

    return spec_case;
}

std::vector<SpecCase> DiscoverIntegrationSpecs(const fs::path& root_dir) {
    if (!fs::is_directory(root_dir)) {
        throw std::runtime_error("integration spec root is not a directory: "
                                 + root_dir.string());
    }

    std::vector<fs::path> spec_paths;
    for (auto& entry : fs::recursive_directory_iterator(root_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        constexpr std::string_view suffix = ".spec.json";
        if (name.size() <= suffix.size()) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix)
            != 0) {
            continue;
        }
        spec_paths.push_back(entry.path());
    }

    std::sort(spec_paths.begin(), spec_paths.end());

    std::vector<SpecCase> cases;
    cases.reserve(spec_paths.size());
    for (const auto& path : spec_paths) {
        cases.push_back(LoadIntegrationSpec(path));
    }
    return cases;
}

std::vector<SpecCase> DiscoverIntegrationSpecsFromTestsDir(
    const std::string& subdir) {
    fs::path root = TestsSourceDir() / "integration" / subdir;
    return DiscoverIntegrationSpecs(root);
}

}  // namespace lci::integration
