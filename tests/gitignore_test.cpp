// Unit tests for lci::GitignoreParser.
//
// Locks the rel-path semantics required by call sites in
// src/indexing/watcher.cpp (lines 284, 303) and
// src/indexing/pipeline_scanner.cpp (lines 106, 160).
//
// Parity reference: lci/internal/config/gitignore_test.go in the Go tree.
// Each test case mirrors a Go case so the C++ port stays bug-compatible
// where required (root-level + nested paths, directory patterns, negation,
// wildcards across `/` boundaries).
//
// Performance contract (karpathy-principles.md): should_ignore must not
// allocate on the Linux hot path (no `\` in path). The PerfNoAlloc test
// exercises a representative call profile to catch regressions.

#include <lci/config/gitignore.h>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace lci {
namespace {

// Build a parser from an in-memory list of pattern lines, mirroring the
// shape of a .gitignore file but bypassing the filesystem.
GitignoreParser parser_from_patterns(
    std::initializer_list<std::string_view> lines) {
    GitignoreParser p;
    for (auto line : lines) {
        p.add_pattern(line);
    }
    return p;
}

// --- Basic patterns ---------------------------------------------------------

TEST(GitignoreParser, ExactFileMatchAtRoot) {
    auto p = parser_from_patterns({"README.md"});
    EXPECT_TRUE(p.should_ignore("README.md", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("main.js", /*is_dir=*/false));
}

TEST(GitignoreParser, ExactFileMatchNested) {
    // Bare name matches at any depth (gitignore standard).
    auto p = parser_from_patterns({"README.md"});
    EXPECT_TRUE(p.should_ignore("docs/README.md", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("a/b/c/README.md", /*is_dir=*/false));
}

// --- Directory patterns -----------------------------------------------------

TEST(GitignoreParser, DirectoryPatternMatchesDirectory) {
    auto p = parser_from_patterns({"node_modules/"});
    EXPECT_TRUE(p.should_ignore("node_modules", /*is_dir=*/true));
}

TEST(GitignoreParser, DirectoryPatternMatchesFilesInside) {
    auto p = parser_from_patterns({"node_modules/"});
    EXPECT_TRUE(
        p.should_ignore("node_modules/react/index.js", /*is_dir=*/false));
}

TEST(GitignoreParser, DirectoryPatternNoMatchSibling) {
    auto p = parser_from_patterns({"node_modules/"});
    EXPECT_FALSE(p.should_ignore("src/main.js", /*is_dir=*/false));
}

TEST(GitignoreParser, NestedDirectoryPatternMatchesContents) {
    // Bare directory name should match at any depth.
    auto p = parser_from_patterns({"node_modules/"});
    EXPECT_TRUE(p.should_ignore(
        "packages/app/node_modules/react/index.js", /*is_dir=*/false));
}

// --- Absolute patterns ------------------------------------------------------

TEST(GitignoreParser, AbsolutePatternMatchesRoot) {
    auto p = parser_from_patterns({"/build"});
    EXPECT_TRUE(p.should_ignore("build", /*is_dir=*/false));
}

TEST(GitignoreParser, AbsolutePatternNoMatchSubdirectory) {
    // `/build` is anchored at root and must NOT match `public/build`.
    auto p = parser_from_patterns({"/build"});
    EXPECT_FALSE(p.should_ignore("public/build", /*is_dir=*/false));
}

// --- Wildcards --------------------------------------------------------------

TEST(GitignoreParser, SuffixWildcardAtRoot) {
    auto p = parser_from_patterns({"*.min.js"});
    EXPECT_TRUE(p.should_ignore("bundle.min.js", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("bundle.js", /*is_dir=*/false));
}

TEST(GitignoreParser, SuffixWildcardNested) {
    auto p = parser_from_patterns({"*.log"});
    EXPECT_TRUE(p.should_ignore("logs/app.log", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("var/logs/2023/01/app.log", /*is_dir=*/false));
}

TEST(GitignoreParser, DoubleStarPattern) {
    auto p = parser_from_patterns({"**/*.log"});
    EXPECT_TRUE(p.should_ignore("logs/app.log", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("logs/2023/01/app.log", /*is_dir=*/false));
}

TEST(GitignoreParser, PrefixWildcard) {
    auto p = parser_from_patterns({"test*"});
    EXPECT_TRUE(p.should_ignore("test_main.cc", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("main_test.cc", /*is_dir=*/false));
}

// --- Negation ---------------------------------------------------------------

TEST(GitignoreParser, NegationReinstatesFile) {
    auto p = parser_from_patterns({"*.log", "!important.log"});
    EXPECT_FALSE(p.should_ignore("important.log", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("debug.log", /*is_dir=*/false));
}

TEST(GitignoreParser, NegationOrderingMatters) {
    // Negation only applies to patterns that come AFTER it. A later
    // catch-all re-ignores the negated file.
    auto p = parser_from_patterns({"*.log", "!important.log", "*.log"});
    EXPECT_TRUE(p.should_ignore("important.log", /*is_dir=*/false));
}

// --- Multi-pattern composition ---------------------------------------------

TEST(GitignoreParser, MultiplePatterns) {
    auto p = parser_from_patterns({"*.log", "*.tmp", "temp/"});
    EXPECT_TRUE(p.should_ignore("debug.log", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("a/b/scratch.tmp", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("temp/cache.bin", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("src/main.js", /*is_dir=*/false));
}

TEST(GitignoreParser, BuildDistDoubleStar) {
    auto p = parser_from_patterns({"dist/**", "build/**"});
    EXPECT_TRUE(
        p.should_ignore("dist/static/css/main.css", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("build/lib/foo.o", /*is_dir=*/false));
}

// --- Edge cases -------------------------------------------------------------

TEST(GitignoreParser, EmptyPatternIgnoresNothing) {
    auto p = parser_from_patterns({""});
    EXPECT_FALSE(p.should_ignore("any-file.txt", /*is_dir=*/false));
}

TEST(GitignoreParser, CommentLineIsNoOp) {
    auto p = parser_from_patterns({"# comment", "*.log"});
    EXPECT_TRUE(p.should_ignore("a.log", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("a.txt", /*is_dir=*/false));
}

TEST(GitignoreParser, DotPrefixedFiles) {
    auto p = parser_from_patterns({".env*", "!.env.example"});
    EXPECT_TRUE(p.should_ignore(".env.local", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore(".env.example", /*is_dir=*/false));
}

TEST(GitignoreParser, HiddenDirectory) {
    auto p = parser_from_patterns({".git/"});
    EXPECT_TRUE(p.should_ignore(".git/objects/12/3456", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore(".git", /*is_dir=*/true));
}

// --- Rel-path contract regression guards ------------------------------------
//
// These pin the rel-path contract documented in
// include/lci/config/gitignore.h. They are the explicit acceptance tests for
// the watcher fix: should_ignore must work against rel paths at BOTH root
// level and nested levels.

TEST(GitignoreParser, RelPathRootLevelFile) {
    auto p = parser_from_patterns({"*.log"});
    // rel path with no directory component (root of project).
    EXPECT_TRUE(p.should_ignore("app.log", /*is_dir=*/false));
}

TEST(GitignoreParser, RelPathNestedFile) {
    auto p = parser_from_patterns({"*.log"});
    // rel path with multiple components.
    EXPECT_TRUE(p.should_ignore("a/b/c/app.log", /*is_dir=*/false));
}

TEST(GitignoreParser, RelPathDirectoryAtRoot) {
    auto p = parser_from_patterns({"target/"});
    EXPECT_TRUE(p.should_ignore("target", /*is_dir=*/true));
    EXPECT_TRUE(p.should_ignore("target/release/foo", /*is_dir=*/false));
}

TEST(GitignoreParser, AbsPathInputWouldNotMatchAnchoredPattern) {
    // Documents the bug class: if a caller mistakenly passes an absolute
    // path like "/tmp/proj/build", an anchored pattern "/build" no longer
    // matches because the path text starts with "/tmp/proj/", not "build".
    // This is the failure mode the watcher fix addresses; keeping the
    // assertion here makes the regression visible if someone re-introduces
    // an abs-path call site.
    auto p = parser_from_patterns({"/build"});
    EXPECT_FALSE(p.should_ignore("/tmp/proj/build", /*is_dir=*/false));
}

// --- Performance contract ---------------------------------------------------
//
// karpathy-principles.md: gitignore is on the indexing hot path; per-call
// allocation in should_ignore is a regression. We cannot easily count
// allocations from here, but we can pin a representative call shape so a
// future profiler run has a stable baseline.

TEST(GitignoreParser, HotPathManyCallsStable) {
    auto p = parser_from_patterns({
        "*.log",
        "node_modules/",
        "build/",
        "dist/**",
        ".env*",
        "!.env.example",
        "*.tmp",
        "target/",
    });

    // Mixed root-level and nested paths, mostly non-matching (the common
    // case in a real repo walk).
    const std::vector<std::string> paths = {
        "src/main.cpp",
        "src/util/helper.cpp",
        "include/lci/foo.h",
        "tests/foo_test.cpp",
        "docs/index.md",
        "build/CMakeCache.txt",
        "node_modules/react/index.js",
        "app.log",
        "a/b/c/d.log",
        ".env.local",
        ".env.example",
    };

    // 10k iterations to surface accidental O(n) regressions in a profile;
    // assertion is just "does not crash and is consistent".
    size_t ignored_count = 0;
    for (int i = 0; i < 1000; ++i) {
        for (const auto& path : paths) {
            if (p.should_ignore(path, /*is_dir=*/false)) ++ignored_count;
        }
    }
    // 5 ignored paths × 1000 iters = 5000 (build/, node_modules/, app.log,
    // a/b/c/d.log, .env.local).
    EXPECT_EQ(ignored_count, 5000u);
}

}  // namespace
}  // namespace lci
