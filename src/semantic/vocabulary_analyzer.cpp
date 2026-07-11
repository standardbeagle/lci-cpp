#include <lci/semantic/semantic_scorer.h>
#include <lci/core/text.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <re2/re2.h>

namespace lci {

namespace {

const std::vector<std::string>& common_exclude_patterns() {
    static const std::vector<std::string> patterns = {
        "test", "tests", "spec", "specs",
        "mock", "mocks", "fixture", "fixtures",
        "__tests__", "__mocks__", "testdata",
        ".git", ".svn", ".hg",
        "node_modules", "vendor",
    };
    return patterns;
}

bool is_production_code(const FileSymbol& symbol, const ProjectConfig& config) {
    const auto& path = symbol.file_path;

    // Check exclude directories.
    for (const auto& dir : config.exclude_dirs) {
        if (path.find(dir) != std::string::npos) return false;
    }

    // Check common test patterns.
    for (const auto& pattern : common_exclude_patterns()) {
        if (text::ascii_contains_ci(path, pattern)) return false;
    }

    // Language-specific patterns.
    //
    // Karpathy: static RE2 instances compile ONCE on first call (function-local
    // static init is thread-safe under C++11+). PartialMatch is then linear-
    // time and allocation-free over the path StringPiece.
    if (config.language == "go") {
        static const RE2 go_test(R"(_test\.go$|test_.*\.go$|.*_test\.go$)");
        if (RE2::PartialMatch(path, go_test)) return false;
    } else if (config.language == "javascript" || config.language == "typescript") {
        static const RE2 js_test(
            R"(\.(test|spec)\.(js|ts|jsx|tsx)$|__tests?__/|test/)");
        if (RE2::PartialMatch(path, js_test)) return false;
    } else if (config.language == "python") {
        static const RE2 py_test(R"(test_.*\.py$|.*_test\.py$|tests?/)");
        if (RE2::PartialMatch(path, py_test)) return false;
    } else if (config.language == "java") {
        static const RE2 java_test(R"(.*Test\.java$|test/)");
        if (RE2::PartialMatch(path, java_test)) return false;
    } else {
        // Generic: check test markers.
        for (const auto& marker : config.test_markers) {
            if (text::ascii_contains_ci(path, marker)) return false;
        }
    }

    // Check source directories.
    if (!config.source_dirs.empty()) {
        for (const auto& src_dir : config.source_dirs) {
            if (path.find(src_dir) != std::string::npos) return true;
        }
        return false;
    }

    return true;
}

}  // anonymous namespace

std::vector<FileSymbol> filter_production_symbols(
    const std::vector<FileSymbol>& symbols,
    const ProjectConfig& config) {

    std::vector<FileSymbol> result;
    result.reserve(symbols.size());
    for (const auto& s : symbols) {
        if (is_production_code(s, config)) {
            result.push_back(s);
        }
    }
    return result;
}

}  // namespace lci
