#include <lci/analysis/naming_analyzer.h>

#include <lci/idcodec.h>
#include <lci/reference.h>
#include <lci/semantic/name_splitter.h>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>

namespace lci {

namespace {

// Common programming / English words that are inherently searchable and must
// never be flagged as obscure jargon. Kept deliberately broad: the cost of a
// false "obscure" flag (agent distrusts the report) is higher than missing one
// genuine outlier. Synonym-group members are recognised separately.
const absl::flat_hash_set<std::string>& common_words() {
    static const absl::flat_hash_set<std::string> w = {
        // verbs
        "get", "set", "add", "new", "init", "run", "make", "build", "handle",
        "process", "parse", "read", "write", "load", "save", "find", "update",
        "delete", "create", "remove", "list", "map", "filter", "send", "recv",
        "receive", "open", "close", "start", "stop", "check", "test", "main",
        "setup", "format", "print", "log", "emit", "call", "exec", "apply",
        "visit", "walk", "scan", "copy", "move", "sort", "merge", "split",
        "join", "wrap", "unwrap", "encode", "decode", "hash", "lock", "unlock",
        "push", "pop", "peek", "clear", "reset", "flush", "sync", "wait",
        "poll", "bind", "listen", "accept", "serve", "route", "render",
        "resolve", "reject", "validate", "verify", "compare", "clone", "fetch",
        "store", "query", "search", "lookup", "count", "register", "connect",
        "enable", "disable", "convert", "transform", "extract", "compute",
        "calculate", "generate", "execute", "dispatch", "notify", "subscribe",
        "publish", "consume", "encrypt", "decrypt", "compress", "marshal",
        // printf-family format verbs (common across C/Go/etc.)
        "printf", "sprintf", "fprintf", "logf", "errorf", "fatalf", "debugf",
        "warnf", "infof", "panicf", "scanf", "sscanf", "println", "printef",
        // nouns
        "data", "value", "name", "id", "type", "key", "item", "result",
        "error", "err", "config", "cfg", "client", "server", "request", "req",
        "response", "resp", "res", "file", "path", "dir", "line", "token",
        "node", "tree", "array", "slice", "buffer", "buf", "string", "str",
        "byte", "int", "bool", "context", "ctx", "opt", "option", "options",
        "args", "arg", "param", "params", "info", "state", "status", "index",
        "idx", "size", "len", "length", "offset", "start", "end", "min", "max",
        "sum", "total", "user", "session", "db", "sql", "url", "uri", "http",
        "json", "xml", "html", "api", "util", "utils", "helper", "impl",
        "base", "core", "common", "internal", "field", "record", "model",
        "view", "controller", "service", "manager", "factory", "builder",
        "handler", "worker", "pool", "queue", "stack", "cache", "store",
        "header", "body", "payload", "message", "event", "signal", "channel",
        "stream", "reader", "writer", "scanner", "parser", "encoder",
        "decoder", "iterator", "entry", "element", "object", "instance",
        "class", "func", "method", "module", "package", "symbol", "block",
        "chunk", "page", "row", "column", "table", "schema", "field", "value",
        // very common predicate / positional / misc words that lead names
        "has", "is", "are", "can", "should", "will", "was", "next", "prev",
        "previous", "first", "last", "current", "cur", "parent", "child",
        "root", "left", "right", "top", "bottom", "head", "tail", "empty",
        "contains", "exists", "exist", "equal", "equals", "valid", "ok",
        "done", "ready", "active", "default", "all", "any", "none", "post",
        "put", "patch", "head", "trace", "settings", "logger", "trigger",
        "collection", "pointer", "ensure", "starts", "ends", "with", "without",
        "to", "from", "as", "into", "ref", "ptr", "len", "cap", "num", "obj",
        // common English programming words previously mis-flagged as jargon
        // or needed as misspelling correction targets (D6)
        "use", "mount", "group", "tee", "suppress", "separator", "missing",
        "miss", "found", "not", "discard", "seek", "effect", "escape", "rune",
        "nullify",
    };
    return w;
}

// True if `word` is a common word or a common derived form of one — strips a
// frequent English suffix and re-checks the stem (seekable -> seek,
// effective -> effect+ive, movable -> move). Guards obscure-token against
// flagging ordinary derived English (D6 anti-signal).
bool is_common_english(const std::string& word) {
    const auto& cw = common_words();
    if (cw.contains(word)) return true;
    static constexpr std::string_view suffixes[] = {
        "able", "ible", "ive", "ing", "ed", "er", "s",
    };
    for (auto suf : suffixes) {
        if (word.size() < suf.size() + 3) continue;
        if (std::string_view(word).substr(word.size() - suf.size()) != suf)
            continue;
        std::string stem = word.substr(0, word.size() - suf.size());
        if (cw.contains(stem) || cw.contains(stem + "e")) return true;
    }
    return false;
}

// Bounded Levenshtein distance; returns limit+1 as soon as the distance
// provably exceeds `limit`. Words here are short (< 32 chars).
int edit_distance_capped(std::string_view a, std::string_view b, int limit) {
    int la = static_cast<int>(a.size()), lb = static_cast<int>(b.size());
    if (std::abs(la - lb) > limit) return limit + 1;
    // Single reused row; sizes are tiny so stack buffer suffices.
    int prev[64], curr[64];
    for (int j = 0; j <= lb; ++j) prev[j] = j;
    for (int i = 1; i <= la; ++i) {
        curr[0] = i;
        int row_min = curr[0];
        for (int j = 1; j <= lb; ++j) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
            row_min = std::min(row_min, curr[j]);
        }
        if (row_min > limit) return limit + 1;
        std::copy(curr, curr + lb + 1, prev);
    }
    return prev[lb];
}

// Max edit distance treated as a plausible typo for a word of this length.
int misspell_limit(size_t len) { return len >= 8 ? 2 : 1; }

// Naming-convention style of a raw (unsplit) symbol name.
enum class NameStyle { Snake, Camel, Other };

NameStyle classify_style(std::string_view name) {
    bool has_underscore = false, has_lower = false, transition = false;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c == '_' && i > 0 && i + 1 < name.size()) has_underscore = true;
        if (c >= 'a' && c <= 'z') has_lower = true;
        if (i > 0 && c >= 'A' && c <= 'Z' && name[i - 1] >= 'a' &&
            name[i - 1] <= 'z') {
            transition = true;
        }
    }
    if (has_underscore && has_lower && !transition) return NameStyle::Snake;
    if (transition && !has_underscore) return NameStyle::Camel;
    return NameStyle::Other;
}

std::string basename_of(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

bool is_alpha_word(std::string_view s, size_t min_len) {
    if (s.size() < min_len) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    }
    return true;
}

bool is_function_like(SymbolType t) {
    return t == SymbolType::Function || t == SymbolType::Method;
}

}  // namespace

bool NamingAnalyzer::is_common_word(std::string_view word) {
    return common_words().contains(word);
}

NamingReport NamingAnalyzer::analyze(const std::vector<FileSymbolData>& files,
                                     const SynonymTable& synonyms,
                                     std::string_view project_root) const {
    (void)project_root;
    NameSplitter splitter;
    NamingReport report;

    // Pass 1: corpus token frequency = number of distinct symbols whose name
    // contains the token.
    absl::flat_hash_map<std::string, int> token_freq;
    // Per-synonym-group member usage: canonical group key -> (member -> count).
    absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, int>>
        group_usage;

    // Per-file naming-convention tally (snake_case vs camelCase methods).
    struct StyleTally {
        int snake{};
        int camel{};
    };

    struct Cand {
        const EnhancedSymbol* sym;
        std::string base_path;
        std::vector<std::string> tokens;
        NameStyle style;
        const StyleTally* file_tally;  ///< convention tally of the owning file
    };
    std::vector<Cand> cands;

    // Keyed by full file path; stable node pointers (node_hash_map semantics
    // via unique_ptr) so Cand can hold a pointer across insertions.
    absl::flat_hash_map<std::string, std::unique_ptr<StyleTally>> file_styles;

    for (const auto& file : files) {
        std::string bp = basename_of(file.path);
        auto& tally_ptr = file_styles[file.path];
        if (!tally_ptr) tally_ptr = std::make_unique<StyleTally>();
        StyleTally& tally = *tally_ptr;
        for (const auto* sym : file.symbols) {
            if (!sym || !is_function_like(sym->symbol.type)) continue;
            auto tokens = splitter.split(sym->symbol.name);
            if (tokens.empty()) continue;
            absl::flat_hash_set<std::string> uniq(tokens.begin(), tokens.end());
            for (const auto& t : uniq) token_freq[t]++;

            NameStyle style = classify_style(sym->symbol.name);
            if (style == NameStyle::Snake) tally.snake++;
            if (style == NameStyle::Camel) tally.camel++;

            // Record synonym-group usage for the leading verb, keyed by the
            // group's primary (most recognizable) term.
            const std::string& verb = tokens.front();
            auto primary = synonyms.primary_of(verb);
            if (!primary.empty()) {
                group_usage[std::string(primary)][verb]++;
            }
            cands.push_back({sym, bp, std::move(tokens), style, &tally});
        }
    }

    // Misspelling dictionary: corpus-frequent tokens (>= 3 distinct symbols)
    // plus the common-word list — the vocabulary a rare lone token can be a
    // typo OF. Bucketed by length so the distance scan stays tight.
    absl::flat_hash_map<size_t, std::vector<std::string_view>> dict_by_len;
    auto add_dict_word = [&](std::string_view w) {
        if (w.size() >= 4) dict_by_len[w.size()].push_back(w);
    };
    for (const auto& [t, n] : token_freq) {
        if (n >= 3 && is_alpha_word(t, 4)) add_dict_word(t);
    }
    for (const auto& w : common_words()) add_dict_word(w);
    // Hash-map iteration order is nondeterministic; sort buckets so the
    // chosen correction is stable across runs/machines (karpathy rule 4).
    for (auto& [len, words] : dict_by_len) {
        std::sort(words.begin(), words.end());
        words.erase(std::unique(words.begin(), words.end()), words.end());
    }

    // Best correction for a rare unknown token, empty if none within the
    // typo distance limit for its length.
    auto find_correction = [&](const std::string& t) -> std::string_view {
        int limit = misspell_limit(t.size());
        std::string_view best;
        int best_d = limit + 1;
        for (int len = static_cast<int>(t.size()) - limit;
             len <= static_cast<int>(t.size()) + limit; ++len) {
            auto it = dict_by_len.find(static_cast<size_t>(len));
            if (it == dict_by_len.end()) continue;
            for (auto w : it->second) {  // sorted: first hit at a given d wins
                if (w == t) continue;
                int d = edit_distance_capped(t, w, limit);
                if (d < best_d) {
                    best_d = d;
                    best = w;
                }
            }
        }
        return best_d <= limit ? best : std::string_view{};
    };

    // High fan-in means the codebase itself teaches the term (a chi user
    // learns Use/Mount immediately) — such verbs are the library's own
    // vocabulary, never "unknown".
    constexpr int kLibraryVerbFanIn = 8;

    // Pass 2: classify outliers. Reason priority: misspelling >
    // convention-mismatch > unknown-verb > obscure-token.
    for (const auto& c : cands) {
        const std::string& verb = c.tokens.front();
        int fan_in = static_cast<int>(c.sym->incoming_ref_count);
        // Important = referenced, or part of the exported API surface
        // (a zero-fan-in exported misspelling like SupressNotFound is exactly
        // what an agent will fail to search for).
        if (fan_in < 2 && !c.sym->is_exported) continue;

        std::string odd_term, reason;
        std::vector<std::string> suggested;

        // 1) Misspelling: a rare, unknown token within typo distance of the
        // corpus's own frequent vocabulary or a common word.
        for (const auto& t : c.tokens) {
            if (!is_alpha_word(t, 4)) continue;
            if (is_common_english(t)) continue;
            if (!synonyms.synonyms_of(t).empty()) continue;
            auto it = token_freq.find(t);
            if (it != token_freq.end() && it->second > 2) continue;
            auto fix = find_correction(t);
            if (!fix.empty()) {
                odd_term = t;
                reason = "misspelling";
                suggested.emplace_back(fix);
                break;
            }
        }

        // 2) Convention mismatch: minority naming style within its file
        // (e.g. snake_case add_* methods in a camelCase codebase).
        if (reason.empty() &&
            (c.style == NameStyle::Snake || c.style == NameStyle::Camel)) {
            const StyleTally& tally = *c.file_tally;
            int total = tally.snake + tally.camel;
            int mine = c.style == NameStyle::Snake ? tally.snake : tally.camel;
            int other = total - mine;
            if (total >= 6 && mine * 2 <= other) {
                odd_term =
                    c.style == NameStyle::Snake ? "snake_case" : "camelCase";
                reason = "convention-mismatch";
            }
        }

        if (reason.empty()) {
            bool verb_known = !synonyms.synonyms_of(verb).empty() ||
                              is_common_english(verb);
            // Corpus rarity of the verb: a non-standard word that nonetheless
            // appears across many symbols (e.g. "Settings", "Logger") is
            // normal domain vocabulary, not jargon — only flag rare unknown
            // verbs on symbols below library-vocabulary fan-in.
            auto vf = token_freq.find(verb);
            int verb_freq = vf != token_freq.end() ? vf->second : 0;

            if (!verb_known && is_alpha_word(verb, 3) && verb_freq <= 2 &&
                fan_in < kLibraryVerbFanIn) {
                odd_term = verb;
                reason = "unknown-verb";
            } else {
                // Corpus-rare, non-standard, non-common obscure token.
                for (const auto& t : c.tokens) {
                    if (!is_alpha_word(t, 4)) continue;
                    if (is_common_english(t)) continue;
                    if (!synonyms.synonyms_of(t).empty()) continue;
                    auto it = token_freq.find(t);
                    if (it != token_freq.end() && it->second <= 2) {
                        odd_term = t;
                        reason = "obscure-token";
                        break;
                    }
                }
            }
        }
        if (reason.empty()) continue;

        VocabularyOutlier o;
        o.object_id = encode_symbol_id(c.sym->id);
        o.name = c.sym->symbol.name;
        o.location = c.base_path + ":" + std::to_string(c.sym->symbol.line);
        o.fan_in = fan_in;
        o.odd_term = odd_term;
        o.reason = reason;
        // Misspellings carry their correction; otherwise suggest common
        // synonyms when the odd term maps to a group (e.g. a
        // recognised-but-rare alias used in this codebase).
        if (!suggested.empty()) {
            o.suggested = std::move(suggested);
        } else {
            auto syn = synonyms.synonyms_of(odd_term);
            for (const auto& s : syn) o.suggested.push_back(s);
            std::sort(o.suggested.begin(), o.suggested.end());
        }
        report.outliers.push_back(std::move(o));
    }

    std::sort(report.outliers.begin(), report.outliers.end(),
              [](const VocabularyOutlier& a, const VocabularyOutlier& b) {
                  if (a.fan_in != b.fan_in) return a.fan_in > b.fan_in;
                  return a.name < b.name;
              });
    if (report.outliers.size() > 15) report.outliers.resize(15);

    // aliases_in_use: only groups where the codebase uses a NON-primary
    // spelling (e.g. "explode" for split). If every occurrence is already the
    // primary/most-recognizable term, there is nothing for an agent to learn,
    // so it is skipped — this is the whole point (surface odd vocabulary, not
    // confirm obvious vocabulary).
    for (auto& [canonical, members] : group_usage) {
        bool has_non_primary = false;
        AliasUsage au;
        au.canonical = canonical;
        for (auto& [m, n] : members) {
            au.terms.emplace_back(m, n);
            if (m != canonical) has_non_primary = true;
        }
        if (!has_non_primary) continue;
        std::sort(au.terms.begin(), au.terms.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second > b.second;
                      return a.first < b.first;
                  });
        report.aliases_in_use.push_back(std::move(au));
    }
    std::sort(report.aliases_in_use.begin(), report.aliases_in_use.end(),
              [](const AliasUsage& a, const AliasUsage& b) {
                  return a.canonical < b.canonical;
              });
    if (report.aliases_in_use.size() > 12) report.aliases_in_use.resize(12);

    return report;
}

}  // namespace lci
