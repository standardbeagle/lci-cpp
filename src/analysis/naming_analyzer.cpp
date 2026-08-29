#include <lci/analysis/naming_analyzer.h>

#include <lci/analysis/english_words.h>
#include <lci/idcodec.h>
#include <lci/reference.h>
#include <lci/semantic/name_splitter.h>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <cmath>
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
        // ubiquitous programming abbreviations (field-run FPs: walkExpr,
        // iprefix — abbreviation-shaped tokens every developer reads fluently)
        "expr", "stmt", "elem", "iter", "attr", "attrs", "prop", "props",
        "env", "src", "dst", "dest", "tmp", "vec", "fmt", "std", "impl",
        "init", "deinit", "alloc", "dealloc", "prefix", "suffix", "async",
        "sync", "mutex", "regex", "glob", "lexer", "stdin", "stdout", "stderr",
        "whitespace", "enum", "ident", "lossy", "lorem", "noop", "varargs",
        "iterator", "accessor", "getter", "setter", "callback", "tokenizer",
        // standard technical acronyms/terms (re-panel FPs: PKCE, HMAC, gzip,
        // cidr, subnet-class networking and crypto vocabulary)
        "backtick", "coord", "pseudorandom", "pkce", "hmac", "sha", "sha1", "sha256", "sha512", "md5", "crc",
        "crc32", "aes", "rsa", "ecdsa", "jwt", "oauth", "tls", "ssl", "tcp",
        "udp", "dns", "ip", "ipv4", "ipv6", "cidr", "subnet", "uuid", "ulid",
        "base64", "utf8", "ascii", "gzip", "brotli", "zstd", "grpc", "rpc",
        "csrf", "cors", "xss", "yaml", "toml", "csv", "html5", "svg", "dom",
        "css", "cli", "gui", "sdk", "os", "io", "fs", "db", "orm", "cron",
        // certification-round FPs: HTTP/domain terms and idiomatic
        // abbreviations flagged on chi/pocketbase
        "auth", "charset", "erd", "addr", "cutset", "stringified",
    };
    return w;
}

// True if `word` is recognizable vocabulary: a common programming word (or a
// derived form of one — seekable -> seek, effective -> effect+ive), or real
// English per the embedded SCOWL dictionary + Porter2 morphology
// (analysis::is_english_like_token). Real English is never a "misspelling"
// or "obscure-token" outlier — flagging fail->tail or opacity as broken
// vocabulary is the anti-signal that makes agents distrust the report (D6).
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
    return analysis::is_english_like_token(word);
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

NamingReport NamingAnalyzer::analyze(
    const std::vector<FileSymbolData>& files, const SynonymTable& synonyms,
    std::string_view project_root,
    const std::function<std::string_view(FileID)>& content_of) const {
    (void)project_root;
    NameSplitter splitter;
    NamingReport report;

    // Pass 1: corpus token frequency = number of distinct symbols whose name
    // contains the token.
    absl::flat_hash_map<std::string, int> token_freq;
    // Definition-site count per full symbol name (ambiguity signal).
    absl::flat_hash_map<std::string, int> name_defs;
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
        std::string ext;  ///< language key for the repo-level style tally
        std::vector<std::string> tokens;
        NameStyle style;
        const StyleTally* file_tally;  ///< convention tally of the owning file
    };
    std::vector<Cand> cands;

    // Keyed by full file path; stable node pointers (node_hash_map semantics
    // via unique_ptr) so Cand can hold a pointer across insertions.
    absl::flat_hash_map<std::string, std::unique_ptr<StyleTally>> file_styles;
    // Repo-level style per language (extension): the fallback for files that
    // are themselves mixed. Guzzle's StreamHandler hides 11 snake_case
    // methods beside ~10 camelCase ones — no 2x minority within the file, so
    // the file-local gate never fires, while the repo is overwhelmingly
    // camelCase (2026-08-26 re-panel miss). Per-language keying matters: a
    // polyglot repo legitimately mixes Go camelCase with Python snake_case.
    absl::flat_hash_map<std::string, StyleTally> ext_styles;

    for (const auto& file : files) {
        std::string bp = basename_of(file.path);
        std::string ext;
        if (auto dot = file.path.rfind('.'); dot != std::string::npos)
            ext = file.path.substr(dot);
        auto& tally_ptr = file_styles[file.path];
        if (!tally_ptr) tally_ptr = std::make_unique<StyleTally>();
        StyleTally& tally = *tally_ptr;
        for (const auto* sym : file.symbols) {
            if (!sym || !is_function_like(sym->symbol.type)) continue;
            if (sym->symbol.test_scaffold) continue;
            // Language-mandated dunder names (__construct, __call, __init__)
            // are neither convention breaks nor vocabulary: never candidates
            // and never counted in a file's style tally (they'd read as
            // snake_case and skew it). 14/15 guzzle "outliers" were magic
            // methods before this gate (2026-08-26 re-panel).
            if (sym->symbol.name.rfind("__", 0) == 0) continue;
            auto tokens = splitter.split(sym->symbol.name);
            if (tokens.empty()) continue;
            absl::flat_hash_set<std::string> uniq(tokens.begin(), tokens.end());
            for (const auto& t : uniq) token_freq[t]++;

            NameStyle style = classify_style(sym->symbol.name);
            if (style == NameStyle::Snake) tally.snake++;
            if (style == NameStyle::Camel) tally.camel++;
            auto& et = ext_styles[ext];
            if (style == NameStyle::Snake) et.snake++;
            if (style == NameStyle::Camel) et.camel++;

            // Record synonym-group usage for the leading verb, keyed by the
            // group's primary (most recognizable) term.
            const std::string& verb = tokens.front();
            auto primary = synonyms.primary_of(verb);
            if (!primary.empty()) {
                group_usage[std::string(primary)][verb]++;
            }
            name_defs[sym->symbol.name]++;
            cands.push_back({sym, bp, ext, std::move(tokens), style, &tally});
        }
    }

    // Name information: corpus-relative selectivity, no English judgment.
    // Token selectivity = fraction of candidates whose name contains it;
    // bits add across a name (selectivities multiply — a name is the
    // intersection of its tokens' candidate sets). A name is vague when the
    // whole name still expects >= kAmbiguousNameDefs matches. The obscurity
    // axis (dictionary legibility of tokens) is tallied in the same pass.
    {
        NameInformation ni;
        const double n_total = cands.empty()
                                   ? 1.0
                                   : static_cast<double>(cands.size());
        absl::flat_hash_map<std::string, int> nonword_freq;
        // name -> (bits, expected_matches, definitions); identical names
        // score identically, so aggregate before ranking.
        struct NameAgg {
            double bits{};
            double expected{};
            int defs{};
        };
        absl::flat_hash_map<std::string, NameAgg> by_name;
        std::vector<double> all_bits;
        all_bits.reserve(cands.size());

        for (const auto& c : cands) {
            // Constructors/destructors are language forms, not naming
            // choices: their tokens necessarily repeat the type name, so
            // they'd flood the vague list without being actionable.
            const std::string& nm = c.sym->symbol.name;
            if (!nm.empty() && nm[0] == '~') continue;
            if (auto sep = nm.rfind("::"); sep != std::string::npos) {
                std::string_view last(nm.c_str() + sep + 2);
                if (nm.compare(0, sep, last) == 0 ||
                    (!last.empty() && last[0] == '~'))
                    continue;
            }
            double bits = 0.0;
            int scored = 0;
            bool has_nonword = false;
            absl::flat_hash_set<std::string_view> seen;
            for (const auto& t : c.tokens) {
                if (!is_alpha_word(t, 2)) continue;
                if (!seen.insert(t).second) continue;
                ni.total_tokens++;
                auto it = token_freq.find(t);
                int freq = it != token_freq.end() ? it->second : 1;
                bits += -std::log2(static_cast<double>(freq) / n_total);
                scored++;
                // Obscurity: unguessable token (fails every dictionary and
                // the corpus-vocabulary bar).
                bool word = is_common_english(t) ||
                            !synonyms.synonyms_of(t).empty() ||
                            freq >= ci_thresholds::kVaguenessCorpusWord;
                if (!word) {
                    ni.nonword_tokens++;
                    nonword_freq[t]++;
                    has_nonword = true;
                }
            }
            (void)has_nonword;
            ni.total_symbols++;
            if (scored == 0) continue;
            all_bits.push_back(bits);
            auto& agg = by_name[c.sym->symbol.name];
            agg.bits = bits;
            agg.expected = n_total * std::exp2(-bits);
            agg.defs++;
        }

        if (!all_bits.empty()) {
            std::sort(all_bits.begin(), all_bits.end());
            ni.median_bits = all_bits[all_bits.size() / 2];
        }

        std::vector<VagueName> vague;
        for (const auto& [name, agg] : by_name) {
            if (agg.expected >=
                static_cast<double>(ci_thresholds::kAmbiguousNameDefs)) {
                vague.push_back(
                    VagueName{name, agg.bits, agg.expected, agg.defs});
            }
        }
        std::sort(vague.begin(), vague.end(),
                  [](const VagueName& a, const VagueName& b) {
                      if (a.expected_matches != b.expected_matches)
                          return a.expected_matches > b.expected_matches;
                      return a.name < b.name;
                  });
        if (vague.size() > size_t{ci_thresholds::kMaxVagueNames})
            vague.resize(ci_thresholds::kMaxVagueNames);
        ni.vague_names = std::move(vague);

        std::vector<std::pair<std::string, int>> top(nonword_freq.begin(),
                                                     nonword_freq.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        if (top.size() > size_t{ci_thresholds::kMaxVagueTokens})
            top.resize(ci_thresholds::kMaxVagueTokens);
        ni.top_nonwords = std::move(top);
        report.information = std::move(ni);
    }

    // Ambiguous names: the same name defined at kAmbiguousNameDefs+ sites.
    // A search on such a name returns every site, so it no longer
    // identifies a symbol (findability, not style).
    {
        std::vector<AmbiguousName> amb;
        for (const auto& [name, n] : name_defs) {
            if (n >= ci_thresholds::kAmbiguousNameDefs)
                amb.push_back(AmbiguousName{name, n});
        }
        std::sort(amb.begin(), amb.end(),
                  [](const AmbiguousName& a, const AmbiguousName& b) {
                      if (a.definition_count != b.definition_count)
                          return a.definition_count > b.definition_count;
                      return a.name < b.name;
                  });
        if (amb.size() > size_t{ci_thresholds::kMaxAmbiguousNames})
            amb.resize(ci_thresholds::kMaxAmbiguousNames);
        report.ambiguous_names = std::move(amb);
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
        bool low_importance = fan_in < 2 && !c.sym->is_exported;

        std::string odd_term, reason;
        std::vector<std::string> suggested;

        // 1) Misspelling: a rare, unknown token within typo distance of the
        // corpus's own frequent vocabulary or a common word. A misspelled
        // NAME is worth flagging at any referenced importance (idnToAsci had
        // fan-in 1 and hid behind the importance gate for three audit
        // rounds); wholly unreferenced private symbols still skip.
        if (low_importance && fan_in < 1) continue;
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

        if (low_importance && reason.empty()) continue;
        // 2) Convention mismatch: minority naming style within its file
        // (e.g. snake_case add_* methods in a camelCase codebase). A file
        // that is itself heavily mixed defeats the local gate, so fall back
        // to the repo-wide tally FOR THE SAME LANGUAGE with a stricter
        // margin (3x, >=20 samples) — repo-level claims need more evidence.
        if (reason.empty() &&
            (c.style == NameStyle::Snake || c.style == NameStyle::Camel)) {
            const StyleTally& tally = *c.file_tally;
            int total = tally.snake + tally.camel;
            int mine = c.style == NameStyle::Snake ? tally.snake : tally.camel;
            int other = total - mine;
            bool flag = total >= 6 && mine * 2 <= other;
            if (!flag) {
                auto it = ext_styles.find(c.ext);
                if (it != ext_styles.end()) {
                    int rt = it->second.snake + it->second.camel;
                    int rm = c.style == NameStyle::Snake ? it->second.snake
                                                         : it->second.camel;
                    flag = rt >= 20 && rm * 3 <= rt - rm;
                }
            }
            if (flag) {
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
                // Corpus-rare, non-standard, non-common obscure token. A
                // token the author spelled ALL-CAPS in the symbol name
                // (hmacSHA256, parsePKCE) is a deliberate acronym, not
                // obscure jargon.
                auto spelled_as_acronym = [&](const std::string& t) {
                    if (t.size() < 2) return false;
                    std::string upper;
                    upper.reserve(t.size());
                    for (char ch : t)
                        upper += static_cast<char>(std::toupper(
                            static_cast<unsigned char>(ch)));
                    return c.sym->symbol.name.find(upper) !=
                           std::string::npos;
                };
                for (const auto& t : c.tokens) {
                    if (!is_alpha_word(t, 4)) continue;
                    if (is_common_english(t)) continue;
                    if (!synonyms.synonyms_of(t).empty()) continue;
                    if (spelled_as_acronym(t)) continue;
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

    // Synonym splits (cluster-vocabulary coherence): names that canonicalize
    // to the same concept — every token mapped to its synonym-group primary —
    // but use different token sequences (fetchUser vs loadUser). One search
    // misses the sibling spellings. Same-sequence names differing only in
    // casing belong to the convention axis; identical spellings at many
    // sites belong to the ambiguity axis; both stay out by keying members on
    // the token sequence.
    {
        struct SpellingAgg {
            int fan_in{};        // summed refs across sites with this spelling
            int best_fan_in{-1};
            std::string name;    // raw spelling at the highest-fan-in site
            std::string location;
        };
        absl::flat_hash_map<std::string,
                            absl::flat_hash_map<std::string, SpellingAgg>>
            by_canonical;
        std::string canon, seq;
        for (const auto& c : cands) {
            const std::string& nm = c.sym->symbol.name;
            if (!nm.empty() && nm[0] == '~') continue;  // language forms
            // Bare single-token verbs (add/push, get/load, put/store on
            // different containers) are deliberate per-structure vocabulary,
            // not a split — aliases_in_use already teaches those. First
            // self-run: all 3 single-token groups were noise, both
            // multi-token groups were real (get_file/load_file,
            // load_snapshot/read_snapshot).
            if (c.tokens.size() < 2) continue;
            canon.clear();
            seq.clear();
            for (const auto& t : c.tokens) {
                auto p = synonyms.primary_of(t);
                if (!canon.empty()) {
                    canon += '_';
                    seq += '_';
                }
                canon += p.empty() ? std::string_view(t) : p;
                seq += t;
            }
            auto& agg = by_canonical[canon][seq];
            int fan_in = static_cast<int>(c.sym->incoming_ref_count);
            agg.fan_in += fan_in;
            if (fan_in > agg.best_fan_in) {
                agg.best_fan_in = fan_in;
                agg.name = nm;
                agg.location =
                    c.base_path + ":" + std::to_string(c.sym->symbol.line);
            }
        }
        std::vector<SynonymSplit> splits;
        for (auto& [canonical, spellings] : by_canonical) {
            if (spellings.size() < 2) continue;
            SynonymSplit sp;
            sp.canonical = canonical;
            for (auto& [s, agg] : spellings) {
                sp.total_fan_in += agg.fan_in;
                sp.members.push_back(SynonymSplitMember{
                    std::move(agg.name), std::move(agg.location), agg.fan_in});
            }
            std::sort(sp.members.begin(), sp.members.end(),
                      [](const SynonymSplitMember& a,
                         const SynonymSplitMember& b) {
                          if (a.fan_in != b.fan_in) return a.fan_in > b.fan_in;
                          return a.name < b.name;
                      });
            splits.push_back(std::move(sp));
        }
        std::sort(splits.begin(), splits.end(),
                  [](const SynonymSplit& a, const SynonymSplit& b) {
                      if (a.total_fan_in != b.total_fan_in)
                          return a.total_fan_in > b.total_fan_in;
                      return a.canonical < b.canonical;
                  });
        if (splits.size() > size_t{ci_thresholds::kMaxSynonymSplits})
            splits.resize(ci_thresholds::kMaxSynonymSplits);
        report.synonym_splits = std::move(splits);
    }

    // Name-vs-source fidelity: a variable initialized from a call should
    // share vocabulary with its source. Textual line scan against the
    // indexed content — report-time only, Variable symbols only.
    if (content_of) {
        struct Cand {
            FidelityMismatch m;
        };
        std::vector<FidelityMismatch> mism;
        for (const auto& file : files) {
            if (file.path.find("/test") != std::string::npos ||
                file.path.find("_test.") != std::string::npos)
                continue;
            std::string_view content;
            bool have_content = false;
            const char* line_start = nullptr;
            // Lazily split content per file only when a Variable shows up.
            std::vector<std::string_view> lines;
            for (const auto* sym : file.symbols) {
                if (sym == nullptr) continue;
                if (sym->symbol.type != SymbolType::Variable) continue;
                if (sym->symbol.test_scaffold) continue;
                const std::string& vname = sym->symbol.name;
                if (vname.size() < 2) continue;
                if (!have_content) {
                    content = content_of(sym->symbol.file_id);
                    have_content = true;
                    size_t start = 0;
                    while (start <= content.size()) {
                        size_t nl = content.find('\n', start);
                        if (nl == std::string_view::npos) {
                            lines.push_back(content.substr(start));
                            break;
                        }
                        lines.push_back(content.substr(start, nl - start));
                        start = nl + 1;
                    }
                }
                int ln = sym->symbol.line;
                if (ln < 1 || static_cast<size_t>(ln) > lines.size()) continue;
                auto callee = initializer_callee(lines[ln - 1],
                                                 sym->symbol.column);
                if (callee.empty() || callee.size() < 4) continue;
                report.fidelity.checked++;

                // Only PLACEHOLDER names are defects here. A name that
                // diverges from its source usually names the ROLE, which is
                // good naming ("size = f.tellg()", "path = read_symlink()").
                // The defect class is a placeholder that threw away an
                // informative source name ("tmp = load_config()").
                static const absl::flat_hash_set<std::string> kPlaceholders =
                    {"tmp",  "temp",  "val",   "value", "ret",   "res",
                     "result", "results", "data", "obj",  "object", "item",
                     "elem", "foo",   "bar",   "baz",  "var",   "ptr",
                     "arr",  "aux",   "misc",  "thing", "stuff", "out"};
                {
                    std::string lower;
                    lower.reserve(vname.size());
                    for (char c : vname)
                        lower.push_back(static_cast<char>(
                            std::tolower(static_cast<unsigned char>(c))));
                    if (!kPlaceholders.contains(lower)) continue;
                }

                NameSplitter fsplit;
                auto vtoks = fsplit.split(vname);
                auto stoks = fsplit.split(callee);
                if (vtoks.empty() || stoks.empty()) continue;
                // The source must have an informative token to offer:
                // real English, >= 4 chars, and not a generic derivation
                // word — "static_cast"/"substr"/"require" teach nothing.
                static const absl::flat_hash_set<std::string> kGenericSrc = {
                    "cast",  "static", "dynamic", "const", "reinterpret",
                    "require", "make", "create",  "get",   "read", "load",
                    "init",  "alloc", "clone",   "copy",  "move", "swap",
                    "begin", "end",   "front",   "back",  "data", "size",
                    "substr", "sync", "async",   "call",  "func", "wrap",
                    // Bare container/value constructors teach nothing.
                    "array", "object", "json",    "list",  "dict", "vector",
                    "string", "value"};
                bool src_informative = false;
                for (const auto& t : stoks) {
                    if (t.size() >= 4 && !kGenericSrc.contains(t) &&
                        is_common_english(t)) {
                        src_informative = true;
                        break;
                    }
                }
                if (!src_informative) continue;
                auto matches = [&](const std::string& v,
                                   const std::string& t) {
                    if (v == t) return true;
                    // Abbreviation: v a subsequence of t sharing the first
                    // char (cfg ~ config, idx ~ index).
                    if (v.size() >= 2 && v.size() < t.size() &&
                        v[0] == t[0]) {
                        size_t vi = 0;
                        for (char c : t) {
                            if (vi < v.size() && v[vi] == c) ++vi;
                        }
                        if (vi == v.size()) return true;
                    }
                    auto pv = synonyms.primary_of(v);
                    if (!pv.empty() && pv == synonyms.primary_of(t))
                        return true;
                    return false;
                };
                bool any = false;
                for (const auto& v : vtoks) {
                    for (const auto& t : stoks) {
                        if (matches(v, t)) { any = true; break; }
                    }
                    if (any) break;
                }
                if (any) continue;
                report.fidelity.mismatched++;
                FidelityMismatch fm;
                fm.var_name = vname;
                fm.source_name = std::string(callee);
                fm.location = basename_of(file.path) + ":" +
                              std::to_string(ln);
                fm.use_count =
                    static_cast<int>(sym->incoming_ref_count);
                mism.push_back(std::move(fm));
            }
        }
        std::sort(mism.begin(), mism.end(),
                  [](const FidelityMismatch& a, const FidelityMismatch& b) {
                      if (a.use_count != b.use_count)
                          return a.use_count > b.use_count;
                      if (a.location != b.location)
                          return a.location < b.location;
                      return a.var_name < b.var_name;
                  });
        if (mism.size() > size_t{10}) mism.resize(10);
        report.fidelity.mismatches = std::move(mism);
    }

    return report;
}


// -- name-vs-source fidelity --------------------------------------------------

std::string_view NamingAnalyzer::initializer_callee(std::string_view line,
                                                    int name_col) {
    size_t pos = name_col > 0 ? static_cast<size_t>(name_col - 1) : 0;
    if (pos >= line.size()) return {};
    // Find the assignment introducing the initializer: '=' (or Go ':='),
    // but not '==', '<=', '>=', '!='.
    size_t eq = std::string_view::npos;
    for (size_t i = pos; i + 1 < line.size(); ++i) {
        char c = line[i];
        if (c == '=') {
            if (line[i + 1] == '=') { ++i; continue; }
            if (i > 0 && (line[i - 1] == '<' || line[i - 1] == '>' ||
                          line[i - 1] == '!' || line[i - 1] == '=')) {
                continue;
            }
            eq = i;
            break;
        }
        if (c == ';' || c == ')') return {};
    }
    if (eq == std::string_view::npos) return {};
    size_t i = eq + 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    // Identifier chain: segments joined by '.', "->", "::".
    size_t chain_start = i;
    size_t seg_start = i;
    auto ident_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    while (i < line.size()) {
        if (ident_char(line[i])) { ++i; continue; }
        if (line[i] == '.') { seg_start = ++i; continue; }
        if (line[i] == ':' && i + 1 < line.size() && line[i + 1] == ':') {
            i += 2; seg_start = i; continue;
        }
        if (line[i] == '-' && i + 1 < line.size() && line[i + 1] == '>') {
            i += 2; seg_start = i; continue;
        }
        break;
    }
    if (i == chain_start || seg_start >= i) return {};
    size_t seg_end = i;
    // Template arguments between the callee and its call parens.
    if (i < line.size() && line[i] == '<') {
        int depth = 1;
        ++i;
        while (i < line.size() && depth > 0) {
            if (line[i] == '<') ++depth;
            else if (line[i] == '>') --depth;
            ++i;
        }
    }
    if (i >= line.size() || line[i] != '(') return {};
    return line.substr(seg_start, seg_end - seg_start);
}

}  // namespace lci
