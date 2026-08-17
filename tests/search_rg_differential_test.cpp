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

#include "unique_temp.h"

#include <algorithm>
#include <array>
#include <cstdio>
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
    return std::system("rg --version > /dev/null 2>&1") == 0;
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
        HitSet actual = lci_hits(mi, corpus.path().string(), pattern);
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
        HitSet actual = lci_hits(mi, corpus.path().string(), pattern);
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
            const std::string& root = cfg.project.root;
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

}  // namespace
}  // namespace lci
