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

#include "unique_temp.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace lci {
namespace {

namespace fs = std::filesystem;

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

// --- Directory-pattern component boundary -----------------------------------
//
// Discrimination pair for the substring-match defect: `build/` must exclude
// files under `build/` and must NOT exclude files under a directory that
// merely ends with "build".

TEST(GitignoreParser, DirectoryPatternRequiresComponentBoundary) {
    auto p = parser_from_patterns({"build/"});
    EXPECT_TRUE(p.should_ignore("build/x.go", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("pkg/build/x.go", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("prebuild/x.go", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("rebuild/x.go", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("pkg/prebuild/x.go", /*is_dir=*/false));
}

// --- Character classes ------------------------------------------------------

TEST(GitignoreParser, CharacterClassMatchesMembers) {
    auto p = parser_from_patterns({"*.p[yc]"});
    EXPECT_TRUE(p.should_ignore("mod.py", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("mod.pc", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("mod.px", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("mod.p", /*is_dir=*/false));
}

TEST(GitignoreParser, CharacterClassRange) {
    auto p = parser_from_patterns({"log[0-9].txt"});
    EXPECT_TRUE(p.should_ignore("log0.txt", /*is_dir=*/false));
    EXPECT_TRUE(p.should_ignore("log7.txt", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("loga.txt", /*is_dir=*/false));
}

TEST(GitignoreParser, CharacterClassNegated) {
    auto p = parser_from_patterns({"tmp[!0-9]"});
    EXPECT_TRUE(p.should_ignore("tmpa", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("tmp1", /*is_dir=*/false));
}

TEST(GitignoreParser, UnterminatedCharacterClassIsLiteral) {
    auto p = parser_from_patterns({"a[bc*"});
    EXPECT_TRUE(p.should_ignore("a[bcd", /*is_dir=*/false));
    EXPECT_FALSE(p.should_ignore("ab", /*is_dir=*/false));
}

// --- Parity with real git ----------------------------------------------------
//
// The expected answers below are NOT hand-written: each row runs real
// `git check-ignore` over a temp fixture repo (git init, write .gitignore +
// the probed file, run git) and compares the parser's verdict against
// git's. The matcher under test shares no code with the oracle
// (bench-harness-oracle-independence).

namespace {

bool git_available() {
    return std::system("git --version > /dev/null 2>&1") == 0;
}

// Runs `git check-ignore` inside `repo` for `path`. Returns 1 = git ignores
// it, 0 = git does not, -1 = git itself failed.
int git_check_ignore(const fs::path& repo, const std::string& path) {
    std::string cmd = "git -C \"" + repo.string() +
                      "\" check-ignore -q -- \"" + path + "\"";
    int rc = std::system(cmd.c_str());
    if (rc == -1) return -1;
#ifdef _WIN32
    return rc;  // system() returns the exit code directly
#else
    if (WIFEXITED(rc)) {
        int code = WEXITSTATUS(rc);
        if (code == 0) return 1;
        if (code == 1) return 0;
    }
    return -1;
#endif
}

struct GitignoreOracleRow {
    std::string gitignore_line;  // single line written to .gitignore
    std::string path;            // repo-relative path to probe
    bool is_dir;
};

void run_oracle_rows(const std::vector<GitignoreOracleRow>& rows) {
    // Group rows by .gitignore line so each fixture carries exactly one
    // pattern, matching how the parser fixture is built.
    std::map<std::string, std::vector<GitignoreOracleRow>> by_line;
    for (const auto& r : rows) by_line[r.gitignore_line].push_back(r);

    for (const auto& [line, probes] : by_line) {
        auto dir = lci::test::unique_temp_dir("lci_gitignore_oracle_");
        fs::create_directories(dir);
        {
            std::ofstream f(dir / ".gitignore");
            f << line << "\n";
        }
        ASSERT_EQ(std::system(("git -C \"" + dir.string() +
                               "\" init -q")
                                  .c_str()),
                  0);
        for (const auto& probe : probes) {
            fs::path p = dir / probe.path;
            if (probe.is_dir) {
                fs::create_directories(p);
            } else {
                fs::create_directories(p.parent_path());
                std::ofstream(p) << "x";
            }
            int git_verdict = git_check_ignore(dir, probe.path);
            ASSERT_NE(git_verdict, -1)
                << "git check-ignore failed for " << probe.path;
            GitignoreParser parser;
            ASSERT_TRUE(parser.load_gitignore(dir.string()));
            EXPECT_EQ(parser.should_ignore(probe.path, probe.is_dir),
                      git_verdict == 1)
                << "pattern '" << line << "' vs path '" << probe.path
                << "' (git says " << (git_verdict == 1) << ")";
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

}  // namespace

TEST(GitignoreGitParity, MatchesRealGitCheckIgnore) {
    if (!git_available()) GTEST_SKIP() << "git not on PATH";
    run_oracle_rows({
        // Interior slash anchors the pattern to the .gitignore directory:
        // doc/*.txt must NOT swallow x/doc/a.txt.
        {"doc/*.txt", "doc/a.txt", false},
        {"doc/*.txt", "x/doc/a.txt", false},
        // A leading-slash directory pattern is anchored: /build/ must not
        // match src/build/x.c.
        {"/build/", "build/x.c", false},
        {"/build/", "src/build/x.c", false},
        // A wildcard directory pattern must match files inside matching
        // directories at any depth.
        {"*.egg-info/", "foo.egg-info/bar.py", false},
        {"*.egg-info/", "pkg/foo.egg-info/bar.py", false},
        {"*.egg-info/", "bar.py", false},
        // git keeps leading whitespace: the pattern names a file whose name
        // begins with a space.
        {" leading.txt", " leading.txt", false},
        {" leading.txt", "leading.txt", false},
        // Backslash escapes a leading '#': pattern matches file "#literal".
        {"\\#literal", "#literal", false},
        {"\\#literal", "x/#literal", false},
    });
}

// --- Nested .gitignore files ---------------------------------------------------

TEST(GitignoreParser, NestedGitignoreAppliesToItsSubtree) {
    auto dir = lci::test::unique_temp_dir("lci_gitignore_nested_");
    fs::create_directories(dir / "sub/x");
    fs::create_directories(dir / "sub/y");
    {
        std::ofstream f(dir / ".gitignore");
        f << "*.tmp\n";
    }
    {
        std::ofstream f(dir / "sub/x/.gitignore");
        f << "*.log\n";
    }
    GitignoreParser p;
    ASSERT_TRUE(p.load_gitignore(dir.string()));

    // Root pattern applies everywhere.
    EXPECT_TRUE(p.should_ignore("a.tmp", false));
    EXPECT_TRUE(p.should_ignore("sub/y/a.tmp", false));
    // Nested pattern applies only inside sub/x.
    EXPECT_TRUE(p.should_ignore("sub/x/a.log", false));
    EXPECT_TRUE(p.should_ignore("sub/x/deep/a.log", false));
    EXPECT_FALSE(p.should_ignore("sub/y/a.log", false));
    EXPECT_FALSE(p.should_ignore("a.log", false));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Nested .gitignore rules override the parent's (git precedence: deeper
// files win), so a negation in the subtree re-includes what the root
// ignored.
TEST(GitignoreParser, NestedGitignoreOverridesParent) {
    auto dir = lci::test::unique_temp_dir("lci_gitignore_nested_");
    fs::create_directories(dir / "sub");
    {
        std::ofstream f(dir / ".gitignore");
        f << "*.log\n";
    }
    {
        std::ofstream f(dir / "sub/.gitignore");
        f << "!keep.log\n";
    }
    GitignoreParser p;
    ASSERT_TRUE(p.load_gitignore(dir.string()));

    EXPECT_TRUE(p.should_ignore("a.log", false));
    EXPECT_TRUE(p.should_ignore("sub/debug.log", false));
    EXPECT_FALSE(p.should_ignore("sub/keep.log", false));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// --- Shared glob entry point ------------------------------------------------

TEST(GlobMatch, SharedDialect) {
    EXPECT_TRUE(glob_match("*.md", "readme.md"));
    EXPECT_FALSE(glob_match("*.md", "docs/readme.md"));
    EXPECT_TRUE(glob_match("**/readme.md", "docs/a/readme.md"));
    EXPECT_TRUE(glob_match("src/?.go", "src/a.go"));
    EXPECT_TRUE(glob_match("*.[ch]", "main.c"));
    EXPECT_FALSE(glob_match("*.[ch]", "main.o"));
}

}  // namespace
}  // namespace lci
