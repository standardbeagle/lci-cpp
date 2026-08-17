#include <lci/path_classifier.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace lci {

namespace {

// Precedence when several builtin markers match one path: a test file inside
// vendor/ is vendored, a generated file under tests/ is a test fixture, etc.
// Lower rank wins.
int rank(PathAttr a) {
    switch (a) {
        case PathAttr::Vendored: return 0;
        case PathAttr::Generated: return 1;
        case PathAttr::Test: return 2;
        case PathAttr::Example: return 3;
        case PathAttr::Docs: return 4;
        case PathAttr::Production: return 5;
    }
    return 5;
}

bool better(PathAttr candidate, PathAttr current) {
    return rank(candidate) < rank(current);
}

// Simple glob: '*' matches any run (including '/'), '?' any one char.
// Iterative backtracking — no allocation, no std::regex (karpathy table).
bool glob_match(std::string_view pat, std::string_view text) {
    size_t p = 0, t = 0, star_p = std::string_view::npos, star_t = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == text[t] || pat[p] == '?')) {
            ++p; ++t;
        } else if (p < pat.size() && pat[p] == '*') {
            star_p = p++;
            star_t = t;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

bool has_suffix(std::string_view s, std::string_view suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool has_prefix(std::string_view s, std::string_view pre) {
    return s.compare(0, std::min(pre.size(), s.size()), pre) == 0 &&
           s.size() >= pre.size();
}

// Directory-segment tag. `seg` excludes the basename.
PathAttr dir_segment_attr(std::string_view seg) {
    // Vendored trees.
    if (seg == "vendor" || seg == "vendors" || seg == "node_modules" ||
        seg == "third_party" || seg == "thirdparty" ||
        seg == "bower_components" || seg == ".yarn") {
        return PathAttr::Vendored;
    }
    // Generated output dirs.
    if (seg == "generated" || seg == "__generated__") {
        return PathAttr::Generated;
    }
    // Test dirs. Deliberately NOT "testing" (real packages are named that;
    // pinned by CIEngine.BuildStructureCategorizesViaClassifyFile).
    if (seg == "test" || seg == "tests" || seg == "__tests__" ||
        seg == "testdata" || seg == "spec" || seg == "specs") {
        return PathAttr::Test;
    }
    // Example/demo dirs. A leading '_' directory is ignored by the Go
    // toolchain (chi's _examples) — non-importable, never production.
    if (seg == "example" || seg == "examples" || seg == "demo" ||
        seg == "demos" || seg == "sample" || seg == "samples" ||
        (!seg.empty() && seg.front() == '_')) {
        return PathAttr::Example;
    }
    if (seg == "doc" || seg == "docs") return PathAttr::Docs;
    return PathAttr::Production;
}

// Basename tag (extension + naming-convention patterns per ecosystem).
PathAttr basename_attr(std::string_view base) {
    // Vendored: minified / bundled assets.
    for (std::string_view suf :
         {".min.js", ".min.mjs", ".min.css", ".bundle.js"}) {
        if (has_suffix(base, suf)) return PathAttr::Vendored;
    }
    // Generated code.
    if (base.find("_generated.") != std::string_view::npos ||
        base.find(".generated.") != std::string_view::npos ||
        has_prefix(base, "zz_generated")) {
        return PathAttr::Generated;
    }
    for (std::string_view suf :
         {".pb.go", ".pb.cc", ".pb.h", "_pb2.py", "_pb2_grpc.py", ".g.dart",
          ".g.cs", ".d.ts"}) {
        if (has_suffix(base, suf)) return PathAttr::Generated;
    }
    // Tests: Go/C++/Ruby/Python suffixes.
    for (std::string_view suf :
         {"_test.go", "_test.py", "_test.cc", "_test.cpp", "_test.rb",
          "_spec.rb", "_test.exs"}) {
        if (has_suffix(base, suf)) return PathAttr::Test;
    }
    // Tests: JS/TS dotted infixes.
    for (std::string_view inf : {".test.", ".spec."}) {
        if (base.find(inf) != std::string_view::npos) return PathAttr::Test;
    }
    // Tests: PHP / Java / C# class suffixes (extension-qualified).
    for (std::string_view suf :
         {"Test.php", "Tests.php", "TestCase.php", "Test.java", "Tests.java",
          "Test.cs", "Tests.cs", "Test.kt"}) {
        if (has_suffix(base, suf)) return PathAttr::Test;
    }
    // Tests: Python prefix convention + pytest plumbing.
    if (base == "conftest.py") return PathAttr::Test;
    if (has_prefix(base, "test_") && has_suffix(base, ".py")) {
        return PathAttr::Test;
    }
    // Docs formats.
    for (std::string_view suf : {".md", ".markdown", ".rst", ".adoc"}) {
        if (has_suffix(base, suf)) return PathAttr::Docs;
    }
    return PathAttr::Production;
}

// True when a config pattern matches. Semantics (see header):
//   trailing '/'  -> directory prefix or interior segment sequence
//   contains '/'  -> glob over the whole relative path
//   otherwise     -> glob over the basename
bool rule_matches(std::string_view pattern, std::string_view rel_path,
                  std::string_view base) {
    if (pattern.empty()) return false;
    if (pattern.back() == '/') {
        std::string_view dir = pattern.substr(0, pattern.size() - 1);
        if (dir.empty()) return false;
        // Anchored prefix ("src/legacy_tests/") or any interior position
        // preceded by a '/' boundary.
        if (has_prefix(rel_path, dir) && rel_path.size() > dir.size() &&
            rel_path[dir.size()] == '/') {
            return true;
        }
        // Interior: "/<dir>/" occurring anywhere.
        std::string needle;
        needle.reserve(dir.size() + 2);
        needle.push_back('/');
        needle.append(dir);
        needle.push_back('/');
        return rel_path.find(needle) != std::string_view::npos;
    }
    if (pattern.find('/') != std::string_view::npos) {
        return glob_match(pattern, rel_path);
    }
    return glob_match(pattern, base);
}

// Content sniff: generated-file header convention within the first 512 bytes
// ("Code generated by X. DO NOT EDIT." — Go; "@generated" — JS/facebook).
bool has_generated_header(std::string_view content) {
    std::string_view head = content.substr(0, std::min<size_t>(content.size(), 512));
    return head.find("Code generated") != std::string_view::npos ||
           head.find("DO NOT EDIT") != std::string_view::npos ||
           head.find("@generated") != std::string_view::npos;
}

// Content sniff: minified asset — large payload with almost no newlines.
bool looks_minified(std::string_view content) {
    if (content.size() < 4096) return false;
    size_t sample = std::min<size_t>(content.size(), 64 * 1024);
    size_t newlines = 0;
    const char* p = content.data();
    const char* end = p + sample;
    while ((p = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)))) != nullptr) {
        ++newlines;
        ++p;
        if (p >= end) break;
    }
    // Average line length over the sample; minifiers emit >300-char lines.
    return sample / (newlines + 1) > 300;
}

}  // namespace

PathClassifier::PathClassifier(std::vector<PathAttrRule> config_rules)
    : config_rules_(std::move(config_rules)) {}

PathAttr PathClassifier::classify(std::string_view rel_path) const {
    auto slash = rel_path.rfind('/');
    std::string_view base = slash == std::string_view::npos
                                ? rel_path
                                : rel_path.substr(slash + 1);

    // Config rules first, declaration order, first match wins — including
    // `production`, which un-tags builtin matches.
    for (const auto& rule : config_rules_) {
        if (rule_matches(rule.pattern, rel_path, base)) return rule.attr;
    }

    // Builtins: strongest attribute across all directory segments + basename.
    PathAttr best = PathAttr::Production;
    size_t start = 0;
    while (start < rel_path.size()) {
        auto end = rel_path.find('/', start);
        if (end == std::string_view::npos) break;  // basename handled below
        std::string_view seg = rel_path.substr(start, end - start);
        PathAttr a = dir_segment_attr(seg);
        if (better(a, best)) best = a;
        start = end + 1;
    }
    PathAttr b = basename_attr(base);
    if (better(b, best)) best = b;
    return best;
}

PathAttr PathClassifier::classify(std::string_view rel_path,
                                  std::string_view content) const {
    auto slash = rel_path.rfind('/');
    std::string_view base = slash == std::string_view::npos
                                ? rel_path
                                : rel_path.substr(slash + 1);
    // An explicit config match (any tag) is authoritative over content.
    for (const auto& rule : config_rules_) {
        if (rule_matches(rule.pattern, rel_path, base)) return rule.attr;
    }
    PathAttr attr = classify(rel_path);
    if (attr != PathAttr::Production || content.empty()) return attr;
    if (has_generated_header(content)) return PathAttr::Generated;
    if (looks_minified(content)) return PathAttr::Vendored;
    return PathAttr::Production;
}

std::string_view PathClassifier::name(PathAttr attr) {
    switch (attr) {
        case PathAttr::Production: return "production";
        case PathAttr::Test: return "test";
        case PathAttr::Example: return "example";
        case PathAttr::Vendored: return "vendored";
        case PathAttr::Generated: return "generated";
        case PathAttr::Docs: return "docs";
    }
    return "production";
}

bool PathClassifier::parse(std::string_view n, PathAttr& out) {
    if (n == "production" || n == "code") { out = PathAttr::Production; return true; }
    if (n == "test") { out = PathAttr::Test; return true; }
    if (n == "example") { out = PathAttr::Example; return true; }
    if (n == "vendored") { out = PathAttr::Vendored; return true; }
    if (n == "generated") { out = PathAttr::Generated; return true; }
    if (n == "docs") { out = PathAttr::Docs; return true; }
    return false;
}

}  // namespace lci
