#include <lci/semantic/semantic_scorer.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_set>

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

std::string str_to_lower(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return result;
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
    std::string h = str_to_lower(haystack);
    std::string n = str_to_lower(needle);
    return h.find(n) != std::string::npos;
}

bool is_production_code(const FileSymbol& symbol, const ProjectConfig& config) {
    const auto& path = symbol.file_path;

    // Check exclude directories.
    for (const auto& dir : config.exclude_dirs) {
        if (path.find(dir) != std::string::npos) return false;
    }

    // Check common test patterns.
    for (const auto& pattern : common_exclude_patterns()) {
        if (contains_ci(path, pattern)) return false;
    }

    // Language-specific patterns.
    if (config.language == "go") {
        static const std::regex go_test("_test\\.go$|test_.*\\.go$|.*_test\\.go$");
        if (std::regex_search(path, go_test)) return false;
    } else if (config.language == "javascript" || config.language == "typescript") {
        static const std::regex js_test(
            "\\.(test|spec)\\.(js|ts|jsx|tsx)$|__tests?__/|test/");
        if (std::regex_search(path, js_test)) return false;
    } else if (config.language == "python") {
        static const std::regex py_test("test_.*\\.py$|.*_test\\.py$|tests?/");
        if (std::regex_search(path, py_test)) return false;
    } else if (config.language == "java") {
        static const std::regex java_test(".*Test\\.java$|test/");
        if (std::regex_search(path, java_test)) return false;
    } else {
        // Generic: check test markers.
        for (const auto& marker : config.test_markers) {
            if (contains_ci(path, marker)) return false;
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
