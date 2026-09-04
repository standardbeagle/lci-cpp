// Differential fuzz: lci literal search vs an independent oracle.
//
// Every seeded-random literal pattern sampled from a bulk-indexed corpus must
// return the same (file, line) hit set from MasterIndex::search_with_options
// as from an oracle that shares NO mechanism with lci's index machinery
// (bench-harness-oracle-independence rule: a validator that reuses the
// subject's matcher inherits its blind spots).
//
// Two oracles:
//   - A naive per-file substring scanner written here (always runs).
//   - ripgrep --fixed-strings, when an `rg` binary is on PATH (cross-checks
//     the naive scanner itself; skipped silently when rg is absent since the
//     in-test oracle already enforces the contract).
//
// Patterns are drawn from real file content, so they include spaces,
// punctuation, mixed case, token fragments, and cross-token spans — the
// exact classes the postings/trigram prefilters cannot represent and must
// therefore hand to the verify scan. This test exists because a phrase with
// a space returned 0 results in production for months while the unit suite
// only ever queried single lowercase tokens.

#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/indexing/master_index.h>
#include <lci/search/search_engine.h>

#include "unique_temp.h"

#include <algorithm>
#include <array>
#include <cstdio>

#ifdef _WIN32
// MSVC spells the POSIX process-pipe pair with an underscore.
#define popen _popen
#define pclose _pclose
#endif
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace lci {
namespace {

class TempCorpus {
  public:
    TempCorpus() {
        path_ = test::unique_temp_dir("lci_rg_diff_");
        std::filesystem::create_directories(path_);
    }
    ~TempCorpus() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempCorpus(const TempCorpus&) = delete;
    TempCorpus& operator=(const TempCorpus&) = delete;

    const std::filesystem::path& path() const { return path_; }

    void write_file(const std::string& rel, const std::string& content) {
        auto full = path_ / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary);
        f << content;
        files_.emplace_back(rel, content);
    }

    const std::vector<std::pair<std::string, std::string>>& files() const {
        return files_;
    }

  private:
    std::filesystem::path path_;
    std::vector<std::pair<std::string, std::string>> files_;
};

/// (relative path, 1-based line) hit; the comparison unit for all oracles.
using HitSet = std::set<std::pair<std::string, int>>;

/// Naive oracle: plain substring scan over the corpus source of truth.
/// Deliberately built on std::string_view::find over the raw content the
/// test wrote — no lci index, tokenizer, or matcher is involved.
HitSet naive_hits(const TempCorpus& corpus, const std::string& pattern) {
    HitSet hits;
    for (const auto& [rel, content] : corpus.files()) {
        size_t pos = 0;
        while ((pos = content.find(pattern, pos)) != std::string::npos) {
            int line = 1 + static_cast<int>(
                std::count(content.begin(),
                           content.begin() + static_cast<long>(pos), '\n'));
            hits.emplace(rel, line);
            // One hit per line is enough for set comparison; skip to next line.
            size_t nl = content.find('\n', pos);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    return hits;
}

HitSet lci_hits(const MasterIndex& mi, const std::string& root,
                const std::string& pattern) {
    SearchOptions opts;
    opts.max_results = 1000;
    HitSet hits;
    for (const auto& r : mi.search_with_options(pattern, opts)) {
        std::string rel = r.path;
        if (rel.rfind(root, 0) == 0 && rel.size() > root.size()) {
            rel = rel.substr(root.size() + 1);
        }
        hits.emplace(rel, r.line);
    }
    return hits;
}

bool rg_available() {
#ifdef _WIN32
    return std::system("rg --version > NUL 2>&1") == 0;
#else
    return std::system("rg --version > /dev/null 2>&1") == 0;
#endif
}

/// ripgrep oracle: `rg --fixed-strings --line-number --no-heading`.
/// Multi-line patterns are not supported by rg's default mode, so callers
/// only route single-line patterns here.
HitSet rg_hits(const std::filesystem::path& root, const std::string& pattern) {
    std::string cmd = "cd '" + root.string() +
                      "' && rg --fixed-strings --line-number --no-heading "
                      "--with-filename -e '" +
                      pattern + "' . 2>/dev/null";
    HitSet hits;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) return hits;
    std::array<char, 4096> buf{};
    std::string out;
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) out += buf.data();
    pclose(pipe);

    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        // Format: ./rel/path:line:content
        size_t c1 = line.find(':');
        if (c1 == std::string::npos) continue;
        size_t c2 = line.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;
        std::string rel = line.substr(0, c1);
        if (rel.rfind("./", 0) == 0) rel = rel.substr(2);
        int lineno = std::atoi(line.substr(c1 + 1, c2 - c1 - 1).c_str());
        if (lineno > 0) hits.emplace(rel, lineno);
    }
    return hits;
}

std::string describe(const HitSet& hits) {
    std::string s;
    for (const auto& [rel, line] : hits) {
        s += rel + ":" + std::to_string(line) + " ";
    }
    return s.empty() ? "(none)" : s;
}

/// Builds the corpus: real-ish code with phrases, mixed case, long tokens,
/// substring-sharing identifiers, and a postings-PARTIAL residue file so the
/// production candidate-set shape (partial files self-nominating) is present.
void build_corpus(TempCorpus& corpus, Config& cfg) {
    corpus.write_file("server.go",
        "package main\n"
        "// The Index server exits after this long with no requests\n"
        "type PageWindow struct{ start, end int }\n"
        "func handle_gadget(w PageWindow) { repagination(w) }\n"
        "func repagination(w PageWindow) {}\n");
    corpus.write_file("util.go",
        "package main\n"
        "// shared page windows walk the master index search path\n"
        "var indexServerReady = false\n"
        "func normalize_context_params(x int) int { return x }\n");
    corpus.write_file("sub/dir/notes.go",
        "package dir\n"
        "// mixed Case Words, punctuation: a->b, x[0], \"quoted text\"\n"
        "const veryLongIdentifierThatKeepsGoingWellPastSixtyFourBytes"
        "AndThenSomeMoreCharacters = 1\n");
    cfg.index.data_file_token_cap = 25;
    std::string residue = "package residue\n// ";
    for (int i = 0; i < cfg.index.data_file_token_cap * 4 + 50; ++i) {
        residue += "tokres" + std::to_string(i) + " ";
    }
    residue += "\n";
    corpus.write_file("residue.go", residue);
}

/// Samples a printable single-line pattern from a random file's content.
std::string sample_pattern(std::mt19937& rng, const TempCorpus& corpus) {
    const auto& files = corpus.files();
    std::uniform_int_distribution<size_t> pick_file(0, files.size() - 1);
    const std::string& content = files[pick_file(rng)].second;
    if (content.size() < 4) return {};
    std::uniform_int_distribution<size_t> pick_pos(0, content.size() - 4);
    std::uniform_int_distribution<size_t> pick_len(3, 24);
    size_t pos = pick_pos(rng);
    size_t len = std::min(pick_len(rng), content.size() - pos);
    std::string p = content.substr(pos, len);
    // Single-line patterns only (rg parity); also drop shell-quoting hazards
    // for the rg leg — the naive oracle still sees every pattern.
    if (p.find('\n') != std::string::npos) {
        p = p.substr(0, p.find('\n'));
    }
    if (p.size() < 3) return {};
    return p;
}

TEST(SearchRgDifferentialTest, RandomLiteralPatternsMatchIndependentOracles) {
    TempCorpus corpus;
    Config cfg = make_default_config();
    build_corpus(corpus, cfg);
    cfg.project.root = corpus.path().string();

    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(corpus.path().string()));
    ASSERT_GE(mi.postings_index().partial_file_count(), 1)
        << "corpus must contain a postings-PARTIAL residue file";

    const bool have_rg = rg_available();
    std::mt19937 rng(20260815);  // Deterministic (karpathy rule 4).

    int checked = 0;
    for (int iter = 0; iter < 400 && checked < 200; ++iter) {
        std::string pattern = sample_pattern(rng, corpus);
        if (pattern.empty()) continue;
        ++checked;

        HitSet expected = naive_hits(corpus, pattern);
        HitSet actual = lci_hits(mi, corpus.path().generic_string(), pattern);
        EXPECT_EQ(expected, actual)
            << "pattern [" << pattern << "]\n  naive: " << describe(expected)
            << "\n  lci:   " << describe(actual);

        if (have_rg && pattern.find('\'') == std::string::npos) {
            HitSet rg = rg_hits(corpus.path(), pattern);
            EXPECT_EQ(rg, expected)
                << "oracle disagreement (naive vs rg) for pattern ["
                << pattern << "]\n  naive: " << describe(expected)
                << "\n  rg:    " << describe(rg);
        }
    }
    ASSERT_GE(checked, 100) << "pattern sampler starved";
}

/// Directed corner patterns that history proved the unit suite never sends.
TEST(SearchRgDifferentialTest, DirectedCornerPatternsMatchNaiveOracle) {
    TempCorpus corpus;
    Config cfg = make_default_config();
    build_corpus(corpus, cfg);
    cfg.project.root = corpus.path().string();

    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(corpus.path().string()));

    const std::vector<std::string> patterns = {
        "Index server",              // phrase with space
        "page windows walk",         // three-word phrase
        "PageWindow",                // mixed-case token, case-sensitive
        "pagination",                // substring of repagination
        "handle_g",                  // prefix of an identifier
        "ndexServerReady",           // suffix fragment
        "a->b, x[0]",                // punctuation-only token boundary
        "PastSixtyFourBytes",        // interior of a >64-byte identifier
        "server exits after this long with no requests",  // long phrase
    };
    for (const auto& pattern : patterns) {
        HitSet expected = naive_hits(corpus, pattern);
        ASSERT_FALSE(expected.empty())
            << "corpus must contain directed pattern [" << pattern << "]";
        HitSet actual = lci_hits(mi, corpus.path().generic_string(), pattern);
        EXPECT_EQ(expected, actual)
            << "pattern [" << pattern << "]\n  naive: " << describe(expected)
            << "\n  lci:   " << describe(actual);
    }
}

/// Case-insensitive naive oracle: byte-fold both sides.
HitSet naive_hits_ci(const TempCorpus& corpus, const std::string& pattern) {
    auto fold = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    HitSet hits;
    const std::string fp = fold(pattern);
    for (const auto& [rel, content] : corpus.files()) {
        const std::string fc = fold(content);
        size_t pos = 0;
        while ((pos = fc.find(fp, pos)) != std::string::npos) {
            int line = 1 + static_cast<int>(
                std::count(fc.begin(), fc.begin() + static_cast<long>(pos),
                           '\n'));
            hits.emplace(rel, line);
            size_t nl = fc.find('\n', pos);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    return hits;
}

TEST(SearchRgDifferentialTest, CaseInsensitivePatternsMatchNaiveOracle) {
    // Guards the case-FOLDED bloom certification: a ci query must never be
    // certified away when only case differs, and truly-absent ci patterns
    // must return empty.
    TempCorpus corpus;
    Config cfg = make_default_config();
    build_corpus(corpus, cfg);
    cfg.project.root = corpus.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(corpus.path().string()));

    const std::vector<std::string> patterns = {
        "pagewindow",      // matches only via case-fold
        "INDEX SERVER",    // folded phrase
        "Handle_Gadget",   // mixed-case fold of an identifier
        "rEpAgInAtIoN",    // aggressive fold
        "zqx absent vbn",  // truly absent
    };
    for (const auto& pattern : patterns) {
        SearchOptions opts;
        opts.max_results = 1000;
        opts.case_insensitive = true;
        HitSet actual;
        for (const auto& r : mi.search_with_options(pattern, opts)) {
            std::string rel = r.path;
            // Result paths are generic; match the strip prefix's spelling.
            const std::string root = corpus.path().generic_string();
            if (rel.rfind(root, 0) == 0 && rel.size() > root.size()) {
                rel = rel.substr(root.size() + 1);
            }
            actual.emplace(rel, r.line);
        }
        HitSet expected = naive_hits_ci(corpus, pattern);
        EXPECT_EQ(expected, actual)
            << "ci pattern [" << pattern << "]\n  naive: "
            << describe(expected) << "\n  lci:   " << describe(actual);
    }
}

// -- invert-match / exclude-comments differential ------------------------------
//
// These drive SearchEngine, not MasterIndex::search_with_options, because the
// MCP `search` handler uses SearchEngine whenever one exists
// (handlers_search.cpp:578) and that is the path `flags=iv` / `flags=nc`
// travel. See the divergence note on the exclude-comments test below.

/// Splits content into lines the way rg counts them: '\n' terminated, and a
/// trailing newline does NOT create a final empty line.
std::vector<std::string> corpus_lines(const std::string& content) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            lines.push_back(content.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < content.size()) lines.push_back(content.substr(start));
    return lines;
}

/// Independent oracle for `rg -v`: every line that does NOT contain the
/// pattern, across EVERY file -- including files with no match at all.
/// Built on std::string::find over the content the test wrote; shares no
/// mechanism with lci's matcher (bench-harness-oracle-independence rule 1).
HitSet naive_invert_hits(const TempCorpus& corpus, const std::string& pattern) {
    HitSet hits;
    for (const auto& [rel, content] : corpus.files()) {
        auto lines = corpus_lines(content);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find(pattern) == std::string::npos) {
                hits.emplace(rel, static_cast<int>(i) + 1);
            }
        }
    }
    return hits;
}

/// Independent oracle for `flags=nc`: matching lines, minus lines that are
/// nothing but a comment. Spelled out here rather than calling the production
/// line_is_comment_only, so the checker cannot inherit that predicate's blind
/// spots (bench-harness-oracle-independence rule 1).
HitSet naive_no_comment_hits(const TempCorpus& corpus,
                             const std::string& pattern) {
    HitSet hits;
    for (const auto& [rel, content] : corpus.files()) {
        auto lines = corpus_lines(content);
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& raw = lines[i];
            if (raw.find(pattern) == std::string::npos) continue;

            size_t b = raw.find_first_not_of(" \t\r");
            if (b == std::string::npos) continue;
            size_t e = raw.find_last_not_of(" \t\r");
            std::string t = raw.substr(b, e - b + 1);

            bool comment_only = t.rfind("//", 0) == 0 || t[0] == '#' ||
                                t.rfind("/*", 0) == 0 ||
                                t.find("*/") != std::string::npos;
            if (comment_only) continue;
            hits.emplace(rel, static_cast<int>(i) + 1);
        }
    }
    return hits;
}

/// ripgrep invert oracle: `rg -v --fixed-strings`.
HitSet rg_invert_hits(const std::filesystem::path& root,
                      const std::string& pattern) {
    std::string cmd = "cd '" + root.string() +
                      "' && rg --fixed-strings --invert-match --line-number "
                      "--no-heading --with-filename -e '" +
                      pattern + "' . 2>/dev/null";
    HitSet hits;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) return hits;
    std::array<char, 4096> buf{};
    std::string out;
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) out += buf.data();
    pclose(pipe);

    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        size_t c1 = line.find(':');
        if (c1 == std::string::npos) continue;
        size_t c2 = line.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;
        std::string rel = line.substr(0, c1);
        if (rel.rfind("./", 0) == 0) rel = rel.substr(2);
        int lineno = std::atoi(line.substr(c1 + 1, c2 - c1 - 1).c_str());
        if (lineno > 0) hits.emplace(rel, lineno);
    }
    return hits;
}

/// Runs a SearchEngine query and reduces it to the (rel path, line) hit set.
HitSet engine_hits(const SearchEngine& engine, const std::string& root,
                   const std::string& pattern, const SearchOptions& base) {
    SearchOptions opts = base;
    opts.max_results = 5000;
    HitSet hits;
    for (const auto& r : engine.search(pattern, opts)) {
        std::string rel = r.path;
        if (rel.rfind(root, 0) == 0 && rel.size() > root.size()) {
            rel = rel.substr(root.size() + 1);
        }
        hits.emplace(rel, r.line);
    }
    return hits;
}

/// A corpus with comment-only lines, trailing comments, and -- critically --
/// a file containing NO occurrence of the pattern, which `rg -v` still reports
/// in full.
void build_flag_corpus(TempCorpus& corpus) {
    corpus.write_file("alpha.go",
        "package main\n"                       // 1 no match
        "// Widget is the comment form\n"      // 2 comment-only, matches
        "type Widget struct{}\n"               // 3 code, matches
        "func run() { use(Widget{}) } // Widget again\n"  // 4 code + trailing
        "var unrelated = 1\n");                // 5 no match
    corpus.write_file("beta.py",
        "import os\n"                          // 1 no match
        "# Widget helper\n"                    // 2 comment-only, matches
        "class Widget:\n"                      // 3 code, matches
        "    pass\n");                         // 4 no match
    corpus.write_file("gamma.go",
        "package gamma\n"                      // 1 no match
        "func nothing() {}\n"                  // 2 no match
        "var plain = 2\n");                    // 3 no match  (NO pattern here)
    corpus.write_file("delta.c",
        "/* Widget block opener */\n"          // 1 comment-only, matches
        "int Widget = 3;\n"                    // 2 code, matches
        "int tail = 4; /* Widget */\n");       // 3 code + block comment
}

TEST(SearchRgDifferentialTest, InvertMatchEqualsInvertOracle) {
    TempCorpus corpus;
    build_flag_corpus(corpus);

    Config cfg = make_default_config();
    cfg.project.root = corpus.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(corpus.path().string()));
    SearchEngine engine(mi);

    const std::string pattern = "Widget";
    SearchOptions opts;
    opts.invert_match = true;
    opts.case_insensitive = false;

    auto got = engine_hits(engine, cfg.project.root, pattern, opts);
    auto want = naive_invert_hits(corpus, pattern);

    EXPECT_EQ(want, got)
        << "\n  want: " << describe(want) << "\n  got:  " << describe(got);

    // The decisive case: gamma.go contains no occurrence at all, so every one
    // of its lines belongs in rg -v output. A candidate-set-scoped invert
    // drops the file entirely.
    bool saw_gamma = false;
    for (const auto& [rel, line] : got) {
        (void)line;
        if (rel == "gamma.go") saw_gamma = true;
    }
    EXPECT_TRUE(saw_gamma) << "a file with no match must still contribute";

    if (rg_available()) {
        auto rg = rg_invert_hits(corpus.path(), pattern);
        EXPECT_EQ(rg, got)
            << "\n  rg:  " << describe(rg) << "\n  got: " << describe(got);
    } else {
        GTEST_LOG_(INFO)
            << "rg not reachable via system(); naive invert oracle only";
    }
}

TEST(SearchRgDifferentialTest, ExcludeCommentsDropsCommentOnlyLines) {
    TempCorpus corpus;
    build_flag_corpus(corpus);

    Config cfg = make_default_config();
    cfg.project.root = corpus.path().string();
    MasterIndex mi(cfg);
    ASSERT_TRUE(mi.index_directory(corpus.path().string()));
    SearchEngine engine(mi);

    const std::string pattern = "Widget";

    SearchOptions plain;
    plain.case_insensitive = false;
    auto all_hits = engine_hits(engine, cfg.project.root, pattern, plain);
    ASSERT_EQ(naive_hits(corpus, pattern), all_hits)
        << "control: unfiltered search must match the plain oracle first";

    SearchOptions nc;
    nc.case_insensitive = false;
    nc.exclude_comments = true;
    auto got = engine_hits(engine, cfg.project.root, pattern, nc);
    auto want = naive_no_comment_hits(corpus, pattern);

    EXPECT_EQ(want, got)
        << "\n  want: " << describe(want) << "\n  got:  " << describe(got);

    // Every comment-only line the plain search returned must be gone, and
    // every code line must survive -- including one with a trailing comment.
    EXPECT_TRUE(got.count({"alpha.go", 2}) == 0) << "// comment-only kept";
    EXPECT_TRUE(got.count({"beta.py", 2}) == 0) << "# comment-only kept";
    EXPECT_TRUE(got.count({"delta.c", 1}) == 0) << "/* comment-only kept";
    EXPECT_TRUE(got.count({"alpha.go", 3}) == 1) << "code line dropped";
    EXPECT_TRUE(got.count({"alpha.go", 4}) == 1)
        << "code line with a TRAILING comment must be kept";
    EXPECT_TRUE(got.count({"beta.py", 3}) == 1) << "code line dropped";
}

}  // namespace
}  // namespace lci
