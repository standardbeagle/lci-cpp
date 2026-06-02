#include <lci/analysis/naming_analyzer.h>

#include <lci/idcodec.h>
#include <lci/reference.h>
#include <lci/semantic/name_splitter.h>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <filesystem>

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
    };
    return w;
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

    struct Cand {
        const EnhancedSymbol* sym;
        std::string base_path;
        std::vector<std::string> tokens;
    };
    std::vector<Cand> cands;

    for (const auto& file : files) {
        std::string bp = basename_of(file.path);
        for (const auto* sym : file.symbols) {
            if (!sym || !is_function_like(sym->symbol.type)) continue;
            auto tokens = splitter.split(sym->symbol.name);
            if (tokens.empty()) continue;
            absl::flat_hash_set<std::string> uniq(tokens.begin(), tokens.end());
            for (const auto& t : uniq) token_freq[t]++;

            // Record synonym-group usage for the leading verb, keyed by the
            // group's primary (most recognizable) term.
            const std::string& verb = tokens.front();
            auto primary = synonyms.primary_of(verb);
            if (!primary.empty()) {
                group_usage[std::string(primary)][verb]++;
            }
            cands.push_back({sym, std::move(bp), std::move(tokens)});
        }
    }

    // Pass 2: classify outliers.
    for (const auto& c : cands) {
        const std::string& verb = c.tokens.front();
        int fan_in = static_cast<int>(c.sym->incoming_refs.size());
        if (fan_in < 2) continue;  // only important, search-worthy symbols

        bool verb_known = !synonyms.synonyms_of(verb).empty() ||
                          is_common_word(verb);
        // Corpus rarity of the verb: a non-standard word that nonetheless
        // appears across many symbols (e.g. "Settings", "Logger") is normal
        // domain vocabulary, not jargon — only flag rare unknown verbs.
        auto vf = token_freq.find(verb);
        int verb_freq = vf != token_freq.end() ? vf->second : 0;
        std::string odd_term, reason;
        std::vector<std::string> suggested;

        if (!verb_known && is_alpha_word(verb, 3) && verb_freq <= 2) {
            odd_term = verb;
            reason = "unknown-verb";
        } else {
            // Look for a corpus-rare, non-standard, non-common obscure token.
            for (const auto& t : c.tokens) {
                if (!is_alpha_word(t, 4)) continue;
                if (is_common_word(t)) continue;
                if (!synonyms.synonyms_of(t).empty()) continue;
                auto it = token_freq.find(t);
                if (it != token_freq.end() && it->second <= 2) {
                    odd_term = t;
                    reason = "obscure-token";
                    break;
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
        // Suggest common synonyms when the odd term maps to a group (e.g. a
        // recognised-but-rare alias used in this codebase).
        auto syn = synonyms.synonyms_of(odd_term);
        for (const auto& s : syn) o.suggested.push_back(s);
        std::sort(o.suggested.begin(), o.suggested.end());
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
