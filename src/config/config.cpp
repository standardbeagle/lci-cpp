#include <lci/config.h>
#include <lci/core/portable.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lci {
namespace {

namespace fs = std::filesystem;

// -- Minimal KDL token types --------------------------------------------------

enum class TokenKind { Ident, String, Number, Bool, LBrace, RBrace, Eof, Error };

struct Token {
    TokenKind kind{};
    std::string text;
    double num_val{};
    bool bool_val{};
    int line{1};
};

// -- KDL lexer ----------------------------------------------------------------

class Lexer {
  public:
    explicit Lexer(std::string_view src) : src_(src), pos_(0) {}

    Token next() {
        skip_ws_and_comments();
        if (!pending_error_.empty()) {
            std::string error = std::move(pending_error_);
            pending_error_.clear();
            return stamp({TokenKind::Error, std::move(error), 0, false});
        }
        if (pos_ >= src_.size()) return stamp({TokenKind::Eof, {}, 0, false});

        char c = src_[pos_];

        if (c == '{') { ++pos_; return stamp({TokenKind::LBrace, "{", 0, false}); }
        if (c == '}') { ++pos_; return stamp({TokenKind::RBrace, "}", 0, false}); }

        if (c == '"') return stamp(lex_string());

        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            return stamp(lex_number_or_ident());
        }

        return stamp(lex_ident_or_bool());
    }

  private:
    std::string_view src_;
    size_t pos_;
    int line_{1};
    std::string pending_error_;

    // Stamps the token with the line where it began. Called after the lexer
    // has already advanced past the token, so re-derive the start line by
    // not counting newlines consumed inside the token body — instead the
    // line counter is advanced only in skip_ws_and_comments, which runs
    // before each token, so line_ already points at the token's start line.
    Token stamp(Token t) {
        t.line = line_;
        return t;
    }

    void skip_ws_and_comments() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == ';') {
                ++pos_;
                continue;
            }
            if (c == '\n') {
                ++pos_;
                ++line_;
                continue;
            }
            if (c == '/' && pos_ + 1 < src_.size()) {
                if (src_[pos_ + 1] == '/') {
                    pos_ += 2;
                    while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                    continue;
                }
                if (src_[pos_ + 1] == '*') {
                    const int start_line = line_;
                    pos_ += 2;
                    while (pos_ + 1 < src_.size() &&
                           !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) {
                        if (src_[pos_] == '\n') ++line_;
                        ++pos_;
                    }
                    if (pos_ + 1 < src_.size()) {
                        pos_ += 2;
                    } else {
                        pos_ = src_.size();
                        pending_error_ =
                            "unterminated block comment starting on line " +
                            std::to_string(start_line);
                    }
                    continue;
                }
            }
            break;
        }
    }

    Token lex_string() {
        const int start_line = line_;
        ++pos_;  // skip opening quote
        std::string val;
        while (pos_ < src_.size() && src_[pos_] != '"') {
            if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                ++pos_;
                switch (src_[pos_]) {
                    case 'n': val += '\n'; break;
                    case 't': val += '\t'; break;
                    case '\\': val += '\\'; break;
                    case '"': val += '"'; break;
                    default: val += src_[pos_]; break;
                }
            } else {
                if (src_[pos_] == '\n') ++line_;
                val += src_[pos_];
            }
            ++pos_;
        }
        if (pos_ >= src_.size()) {
            return {TokenKind::Error,
                    "unterminated string starting on line " +
                        std::to_string(start_line),
                    0, false};
        }
        ++pos_;  // skip closing quote
        return {TokenKind::String, std::move(val), 0, false};
    }

    Token lex_number_or_ident() {
        size_t start = pos_;
        if (src_[pos_] == '-' || src_[pos_] == '+') ++pos_;

        while (pos_ < src_.size() &&
               (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.')) {
            ++pos_;
        }

        // If followed by an identifier char, treat as ident
        if (pos_ < src_.size() && (std::isalpha(static_cast<unsigned char>(src_[pos_])) ||
                                   src_[pos_] == '_')) {
            while (pos_ < src_.size() && is_ident_char(src_[pos_])) ++pos_;
            std::string text(src_.substr(start, pos_ - start));
            return {TokenKind::Ident, std::move(text), 0, false};
        }

        std::string text(src_.substr(start, pos_ - start));
        double val = 0;
        // portable::parse_double, not std::from_chars: libc++ (macOS) leaves
        // the floating-point from_chars overload deleted.
        if (!portable::parse_double(text, val)) val = 0;
        return {TokenKind::Number, std::move(text), val, false};
    }

    Token lex_ident_or_bool() {
        size_t start = pos_;
        while (pos_ < src_.size() && is_ident_char(src_[pos_])) ++pos_;

        // No ident char consumed: an unrecognized character (e.g. the KDL-v2
        // '#' bool prefix '#true'/'#false', or a stray symbol). Emit an Error
        // token describing the offending run and ALWAYS advance past at least
        // one char so the parser cannot loop forever on the same position.
        if (pos_ == start) {
            char bad = src_[pos_];
            ++pos_;
            // Absorb a trailing ident run so '#true' is reported as one token.
            size_t run_start = pos_;
            while (pos_ < src_.size() && is_ident_char(src_[pos_])) ++pos_;
            std::string token(1, bad);
            token.append(src_.substr(run_start, pos_ - run_start));
            return {TokenKind::Error, std::move(token), 0, false};
        }

        std::string text(src_.substr(start, pos_ - start));
        if (text == "true") return {TokenKind::Bool, text, 0, true};
        if (text == "false") return {TokenKind::Bool, text, 0, false};
        return {TokenKind::Ident, std::move(text), 0, false};
    }

    static bool is_ident_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
               c == '.' || c == '/' || c == '*';
    }
};

// -- KDL node representation --------------------------------------------------

struct KdlNode {
    std::string name;
    std::vector<Token> args;
    std::vector<KdlNode> children;
};

// -- KDL parser ---------------------------------------------------------------

class Parser {
  public:
    explicit Parser(std::string_view src) : lex_(src) { advance(); }

    std::vector<KdlNode> parse_document() {
        std::vector<KdlNode> nodes;
        while (cur_.kind != TokenKind::Eof && cur_.kind != TokenKind::RBrace) {
            if (cur_.kind == TokenKind::Error) {
                set_error();
                return nodes;
            }
            if (cur_.kind == TokenKind::Ident) {
                nodes.push_back(parse_node());
                if (!error_.empty()) return nodes;
            } else {
                advance();  // skip unexpected tokens
            }
        }
        return nodes;
    }

    // Empty on success; descriptive parse error otherwise (file, line, token).
    const std::string& error() const { return error_; }

  private:
    Lexer lex_;
    Token cur_;
    std::string error_;

    void advance() { cur_ = lex_.next(); }

    void set_error() {
        if (!error_.empty()) return;  // keep first error
        error_ = "line " + std::to_string(cur_.line) +
                 ": unrecognized token '" + cur_.text +
                 "' (note: KDL-v2 '#'-prefixed values like #true/#false are "
                 "not supported; use bare true/false)";
    }

    KdlNode parse_node() {
        KdlNode node;
        node.name = cur_.text;
        advance();

        // Collect arguments until we see { or a new node name at the same level
        while (cur_.kind == TokenKind::String || cur_.kind == TokenKind::Number ||
               cur_.kind == TokenKind::Bool) {
            node.args.push_back(cur_);
            advance();
        }

        if (cur_.kind == TokenKind::Error) {
            set_error();
            return node;
        }

        if (cur_.kind == TokenKind::LBrace) {
            const int opening_line = cur_.line;
            advance();
            node.children = parse_document();
            if (!error_.empty()) return node;
            if (cur_.kind != TokenKind::RBrace) {
                error_ = "line " + std::to_string(opening_line) +
                         ": unclosed block";
                return node;
            }
            advance();
        }

        return node;
    }
};

// -- Helpers to extract values from KDL nodes ---------------------------------

bool get_string(const KdlNode& n, std::string& out) {
    for (const auto& a : n.args) {
        if (a.kind == TokenKind::String) { out = a.text; return true; }
    }
    return false;
}

bool get_int(const KdlNode& n, int& out) {
    for (const auto& a : n.args) {
        if (a.kind == TokenKind::Number) { out = static_cast<int>(a.num_val); return true; }
    }
    return false;
}

bool get_double(const KdlNode& n, double& out) {
    for (const auto& a : n.args) {
        if (a.kind == TokenKind::Number) { out = a.num_val; return true; }
    }
    return false;
}

bool get_bool(const KdlNode& n, bool& out) {
    for (const auto& a : n.args) {
        if (a.kind == TokenKind::Bool) { out = a.bool_val; return true; }
    }
    return false;
}

// A known key whose argument has the wrong type used to be a silent no-op:
// `max_results "100"` left the default in place with no diagnostic, so the
// user's setting simply never took effect and nothing said so. These setters
// make the mismatch an error, matching the parse_size_string precedent above.

bool set_string(const KdlNode& n, std::string_view path, std::string& dst,
                std::string& error) {
    if (get_string(n, dst)) return true;
    error = std::string(path) + ": expected a quoted string argument";
    return false;
}

bool set_int(const KdlNode& n, std::string_view path, int& dst,
             std::string& error) {
    if (get_int(n, dst)) return true;
    error = std::string(path) + ": expected an integer argument";
    return false;
}

bool set_int64(const KdlNode& n, std::string_view path, int64_t& dst,
               std::string& error) {
    int v = 0;
    if (get_int(n, v)) {
        dst = v;
        return true;
    }
    error = std::string(path) + ": expected an integer argument";
    return false;
}

bool set_double(const KdlNode& n, std::string_view path, double& dst,
                std::string& error) {
    if (get_double(n, dst)) return true;
    error = std::string(path) + ": expected a number argument";
    return false;
}

bool set_bool(const KdlNode& n, std::string_view path, bool& dst,
              std::string& error) {
    if (get_bool(n, dst)) return true;
    error = std::string(path) + ": expected true or false";
    return false;
}

// An unrecognized node name is a warning, not an error: a typo'd key is
// almost always a mistake the user wants to hear about, but rejecting the
// whole file would break forward compatibility with newer keys.
void warn_unknown(std::vector<std::string>* warnings, std::string_view scope,
                  const std::string& name) {
    if (!warnings) return;
    warnings->push_back("unknown config key '" + std::string(scope) +
                        (scope.empty() ? "" : ".") + name + "' (ignored)");
}

std::vector<std::string> collect_strings(const KdlNode& n) {
    std::vector<std::string> result;
    for (const auto& a : n.args) {
        if (a.kind == TokenKind::String) result.push_back(a.text);
    }
    if (result.empty()) {
        for (const auto& child : n.children) {
            std::string s;
            if (get_string(child, s)) {
                result.push_back(std::move(s));
            } else if (!child.name.empty()) {
                result.push_back(child.name);
            }
        }
    }
    return result;
}

// -- Parse size strings like "10MB" -------------------------------------------

// Parses "10MB" / "512KB" / "1024" into a byte count. Returns false on a
// malformed value (non-numeric, or trailing junk like "10XB") rather than
// silently coercing to 0 — a zero max_file_size disables indexing entirely,
// so a bad config line must surface, not fail-open (karpathy #6, fail-fast).
bool parse_size_string(const std::string& s, int64_t& out) {
    std::string upper;
    upper.reserve(s.size());
    for (char c : s) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    int64_t multiplier = 1;
    std::string num_str = upper;

    if (upper.ends_with("GB")) {
        multiplier = 1024LL * 1024 * 1024;
        num_str = upper.substr(0, upper.size() - 2);
    } else if (upper.ends_with("MB")) {
        multiplier = 1024LL * 1024;
        num_str = upper.substr(0, upper.size() - 2);
    } else if (upper.ends_with("KB")) {
        multiplier = 1024;
        num_str = upper.substr(0, upper.size() - 2);
    } else if (upper.ends_with("B")) {
        num_str = upper.substr(0, upper.size() - 1);
    }

    int64_t num = 0;
    const char* begin = num_str.data();
    const char* end = num_str.data() + num_str.size();
    auto [ptr, ec] = std::from_chars(begin, end, num);
    // Reject empty, non-numeric, and trailing-garbage inputs: from_chars
    // succeeds on a numeric prefix, so ptr must reach the end for the whole
    // value to be valid.
    if (ec != std::errc{} || ptr != end || num < 0) return false;
    out = num * multiplier;
    return true;
}

// -- Apply KDL nodes to Config ------------------------------------------------

bool apply_project(Config& cfg, const KdlNode& node, std::string& error,
                   std::vector<std::string>* warnings) {
    for (const auto& child : node.children) {
        if (child.name == "root") {
            if (!set_string(child, "project.root", cfg.project.root, error))
                return false;
        } else if (child.name == "name") {
            if (!set_string(child, "project.name", cfg.project.name, error))
                return false;
        } else {
            warn_unknown(warnings, "project", child.name);
        }
    }
    return true;
}

bool apply_index(Config& cfg, const KdlNode& node, std::string& error,
                 std::vector<std::string>* warnings) {
    for (const auto& child : node.children) {
        if (child.name == "max_file_size") {
            std::string sz;
            if (get_string(child, sz)) {
                if (!parse_size_string(sz, cfg.index.max_file_size)) {
                    error = "index.max_file_size: invalid size value \"" + sz + "\"";
                    return false;
                }
            } else if (!set_int64(child, "index.max_file_size",
                                  cfg.index.max_file_size, error)) {
                return false;
            }
        } else if (child.name == "max_parse_file_size") {
            std::string sz;
            if (get_string(child, sz)) {
                if (!parse_size_string(sz, cfg.index.max_parse_file_size)) {
                    error = "index.max_parse_file_size: invalid size value \"" + sz + "\"";
                    return false;
                }
            } else if (!set_int64(child, "index.max_parse_file_size",
                                  cfg.index.max_parse_file_size, error)) {
                return false;
            }
        } else if (child.name == "data_file_token_cap") {
            get_int(child, cfg.index.data_file_token_cap);
        } else if (child.name == "max_total_size_mb") {
            if (!set_int64(child, "index.max_total_size_mb",
                           cfg.index.max_total_size_mb, error))
                return false;
        } else if (child.name == "max_file_count") {
            if (!set_int(child, "index.max_file_count",
                         cfg.index.max_file_count, error))
                return false;
        } else if (child.name == "overflow_policy") {
            if (!set_string(child, "index.overflow_policy",
                            cfg.index.overflow_policy, error))
                return false;
        } else if (child.name == "follow_symlinks") {
            if (!set_bool(child, "index.follow_symlinks",
                          cfg.index.follow_symlinks, error))
                return false;
        } else if (child.name == "smart_size_control") {
            if (!set_bool(child, "index.smart_size_control",
                          cfg.index.smart_size_control, error))
                return false;
        } else if (child.name == "priority_mode") {
            if (!set_string(child, "index.priority_mode",
                            cfg.index.priority_mode, error))
                return false;
        } else if (child.name == "respect_gitignore") {
            if (!set_bool(child, "index.respect_gitignore",
                          cfg.index.respect_gitignore, error))
                return false;
        } else if (child.name == "watch_mode") {
            if (!set_bool(child, "index.watch_mode", cfg.index.watch_mode,
                          error))
                return false;
        } else if (child.name == "watch_debounce_ms") {
            if (!set_int(child, "index.watch_debounce_ms",
                         cfg.index.watch_debounce_ms, error))
                return false;
        } else {
            warn_unknown(warnings, "index", child.name);
        }
    }
    return true;
}

bool apply_performance(Config& cfg, const KdlNode& node, std::string& error,
                       std::vector<std::string>* warnings) {
    for (const auto& child : node.children) {
        if (child.name == "max_memory_mb") {
            if (!set_int(child, "performance.max_memory_mb",
                         cfg.performance.max_memory_mb, error))
                return false;
        } else if (child.name == "max_goroutines") {
            if (!set_int(child, "performance.max_goroutines",
                         cfg.performance.max_goroutines, error))
                return false;
        } else if (child.name == "debounce_ms") {
            if (!set_int(child, "performance.debounce_ms",
                         cfg.performance.debounce_ms, error))
                return false;
        } else if (child.name == "startup_delay_ms") {
            if (!set_int(child, "performance.startup_delay_ms",
                         cfg.performance.startup_delay_ms, error))
                return false;
        } else {
            warn_unknown(warnings, "performance", child.name);
        }
    }
    return true;
}

bool apply_server(Config& cfg, const KdlNode& node, std::string& error,
                  std::vector<std::string>* warnings) {
    for (const auto& child : node.children) {
        if (child.name == "max_rss_mb") {
            if (!set_int(child, "server.max_rss_mb", cfg.server.max_rss_mb,
                         error))
                return false;
        } else if (child.name == "idle_timeout_sec") {
            if (!set_int(child, "server.idle_timeout_sec",
                         cfg.server.idle_timeout_sec, error))
                return false;
        } else if (child.name == "max_instances") {
            if (!set_int(child, "server.max_instances",
                         cfg.server.max_instances, error))
                return false;
        } else {
            warn_unknown(warnings, "server", child.name);
        }
    }
    return true;
}

bool apply_ranking(SearchRankingConfig& ranking, const KdlNode& node,
                   std::string& error, std::vector<std::string>* warnings) {
    for (const auto& child : node.children) {
        if (child.name == "enabled") {
            if (!set_bool(child, "search.ranking.enabled", ranking.enabled,
                          error))
                return false;
        } else if (child.name == "code_file_boost") {
            if (!set_double(child, "search.ranking.code_file_boost",
                            ranking.code_file_boost, error))
                return false;
        } else if (child.name == "doc_file_penalty") {
            if (!set_double(child, "search.ranking.doc_file_penalty",
                            ranking.doc_file_penalty, error))
                return false;
        } else if (child.name == "config_file_boost") {
            if (!set_double(child, "search.ranking.config_file_boost",
                            ranking.config_file_boost, error))
                return false;
        } else if (child.name == "require_symbol") {
            if (!set_bool(child, "search.ranking.require_symbol",
                          ranking.require_symbol, error))
                return false;
        } else if (child.name == "non_symbol_penalty") {
            if (!set_double(child, "search.ranking.non_symbol_penalty",
                            ranking.non_symbol_penalty, error))
                return false;
        } else {
            warn_unknown(warnings, "search.ranking", child.name);
        }
    }
    return true;
}

bool apply_search(Config& cfg, const KdlNode& node, std::string& error,
                  std::vector<std::string>* warnings) {
    for (const auto& child : node.children) {
        if (child.name == "max_results") {
            if (!set_int(child, "search.max_results", cfg.search.max_results,
                         error))
                return false;
        } else if (child.name == "max_context_lines") {
            if (!set_int(child, "search.max_context_lines",
                         cfg.search.max_context_lines, error))
                return false;
        } else if (child.name == "enable_fuzzy") {
            if (!set_bool(child, "search.enable_fuzzy",
                          cfg.search.enable_fuzzy, error))
                return false;
        } else if (child.name == "merge_file_results") {
            if (!set_bool(child, "search.merge_file_results",
                          cfg.search.merge_file_results, error))
                return false;
        } else if (child.name == "ensure_complete_stmt") {
            if (!set_bool(child, "search.ensure_complete_stmt",
                          cfg.search.ensure_complete_stmt, error))
                return false;
        } else if (child.name == "include_leading_comments") {
            if (!set_bool(child, "search.include_leading_comments",
                          cfg.search.include_leading_comments, error))
                return false;
        } else if (child.name == "ranking") {
            if (!apply_ranking(cfg.search.ranking, child, error, warnings))
                return false;
        } else {
            warn_unknown(warnings, "search", child.name);
        }
    }
    return true;
}

// Folds a `synonyms` KDL block into a frozen SynonymTable. Children are
// `group <words…>`, `clear <word>`, or a leading `clear-all`. Returns the
// table or an lci::Error on validation failure (fail-fast, Karpathy rule 6).
Result<SynonymTable> apply_synonyms(const KdlNode& node) {
    std::vector<SynonymOp> ops;
    ops.reserve(node.children.size());
    for (const auto& child : node.children) {
        if (child.name == "clear-all") {
            ops.push_back({SynonymOp::Kind::ClearAll, {}});
        } else if (child.name == "clear") {
            ops.push_back({SynonymOp::Kind::Clear, collect_strings(child)});
        } else if (child.name == "group") {
            ops.push_back({SynonymOp::Kind::Group, collect_strings(child)});
        } else {
            return make_config_error(
                "synonyms", child.name,
                "unknown synonyms child '" + child.name +
                    "' (expected group, clear, or clear-all)");
        }
    }
    return SynonymTable::build_from_ops(ops);
}

// Expands a leading `~/` (or a bare `~`) against $HOME. Without this,
// `root "~/proj"` produced the literal path <project_root>/~/proj — a
// directory that does not exist, so the index silently covered nothing.
std::string expand_tilde(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    if (path.size() > 1 && path[1] != '/' && path[1] != '\\') {
        return path;  // "~user/..." is not something we resolve.
    }
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (home == nullptr || *home == '\0') home = std::getenv("USERPROFILE");
#endif
    if (home == nullptr || *home == '\0') return path;
    if (path.size() == 1) return std::string(home);
    return (fs::path(home) / path.substr(2)).string();
}

// Base config used when a .lci.kdl file IS loaded. This intentionally
// diverges from make_default_config() (the no-file path) to match Go's
// parseKDL in internal/config/kdl_config.go: that function builds its base
// Config from a struct literal that OMITS several index/performance fields,
// so they take Go's zero value rather than the richer no-file defaults from
// config.go's Load(). Fields NOT set here are left at the C++ struct
// defaults declared in config.h — those defaults match Go's parseKDL literal
// for every field the literal does set (max_file_size, max_total_size_mb,
// follow_symlinks, smart_size_control, priority_mode, max_memory_mb,
// debounce_ms). Only the omitted fields below differ and must be reset to
// Go's zero value. Deliberate divergence: max_file_count default is 50000
// (Go pinned 10000) — the budget became enforced in 2026-08 and 10k files
// would truncate ordinary large repos; the Go oracle is retired.
Config make_kdl_base_config() {
    Config cfg = make_default_config();

    // index: Go's parseKDL literal omits these two -> Go zero values.
    // (respect_gitignore is NOT zero-valued by Go's parseKDL — confirmed
    // empirically against the Go binary on a KDL that omits the field;
    // it stays true. Leaving the struct default in place here.)
    cfg.index.watch_mode = false;
    cfg.index.watch_debounce_ms = 0;

    // performance: Go's parseKDL literal sets MaxGoroutines:4 explicitly and
    // omits ParallelFileWorkers / IndexingTimeoutSec / StartupDelayMs.
    cfg.performance.max_goroutines = 4;
    cfg.performance.parallel_file_workers = 0;
    cfg.performance.indexing_timeout_sec = 0;
    cfg.performance.startup_delay_ms = 0;

    return cfg;
}

// Applies parsed KDL nodes onto an existing Config. Fields absent from the
// document keep whatever `cfg` already holds — this is the shared overlay
// primitive for both the project file and the user-level defaults file.
bool apply_kdl_nodes(Config& cfg, const std::vector<KdlNode>& nodes,
                     std::string& error,
                     std::vector<std::string>* warnings) {
    for (const auto& node : nodes) {
        if (node.name == "project") {
            if (!apply_project(cfg, node, error, warnings)) return false;
        }
        else if (node.name == "index") {
            if (!apply_index(cfg, node, error, warnings)) return false;
        }
        else if (node.name == "performance") {
            if (!apply_performance(cfg, node, error, warnings)) return false;
        }
        else if (node.name == "server") {
            if (!apply_server(cfg, node, error, warnings)) return false;
        }
        else if (node.name == "search") {
            if (!apply_search(cfg, node, error, warnings)) return false;
        }
        else if (node.name == "include") cfg.include = collect_strings(node);
        else if (node.name == "exclude") cfg.exclude = collect_strings(node);
        else if (node.name == "propagation_config_dir") {
            if (!set_string(node, "propagation_config_dir",
                            cfg.propagation_config_dir, error))
                return false;
        }
        else if (node.name == "attributes") {
            // attributes { test "src/legacy_tests/"; vendored "*.iife.js" }
            // Child node name = attribute tag, string args = patterns.
            // Unknown tags fail fast — a typo silently dropping a rule would
            // leave vendored/test code polluting every analysis section.
            for (const auto& child : node.children) {
                PathAttr attr{};
                if (!PathClassifier::parse(child.name, attr)) {
                    error = "attributes: unknown tag '" + child.name +
                            "' (valid: production, test, example, vendored, "
                            "generated, docs)";
                    return false;
                }
                bool any = false;
                for (const auto& a : child.args) {
                    if (a.kind == TokenKind::String && !a.text.empty()) {
                        cfg.attributes.push_back({attr, a.text});
                        any = true;
                    }
                }
                if (!any) {
                    error = "attributes: tag '" + child.name +
                            "' has no pattern strings";
                    return false;
                }
            }
        }
        else if (node.name == "synonyms") {
            auto result = apply_synonyms(node);
            if (!result) {
                error = result.error().to_string();
                return false;
            }
            cfg.synonyms = std::move(result.value());
        }
        else {
            warn_unknown(warnings, "", node.name);
        }
    }
    return true;
}

// Path of the user-level defaults file: $XDG_CONFIG_HOME/lci/config.kdl,
// falling back to ~/.config/lci/config.kdl. Empty when no home is known.
fs::path user_config_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME");
        xdg != nullptr && *xdg != '\0') {
        return fs::path(xdg) / "lci" / "config.kdl";
    }
    if (const char* home = std::getenv("HOME");
        home != nullptr && *home != '\0') {
        return fs::path(home) / ".config" / "lci" / "config.kdl";
    }
    return {};
}

// Overlays the user-level defaults file onto `cfg` when it exists. A
// malformed user file is an error (fail fast — silently ignoring it would
// leave the user believing their defaults apply).
bool overlay_user_config(Config& cfg, std::string& error,
                         std::vector<std::string>* warnings = nullptr) {
    fs::path path = user_config_path();
    if (path.empty()) return true;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return true;

    std::ifstream file(path);
    if (!file) {
        error = "failed to read user config: " + path.string();
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    // Parser holds a string_view — the content must outlive it.
    const std::string content = ss.str();

    Parser parser(content);
    auto nodes = parser.parse_document();
    if (!parser.error().empty()) {
        error = "failed to parse " + path.string() + ": " + parser.error();
        return false;
    }
    if (!apply_kdl_nodes(cfg, nodes, error, warnings)) {
        error = path.string() + ": " + error;
        return false;
    }
    return true;
}

}  // namespace

// Parses KDL content into a Config. On a malformed document, leaves `cfg`
// untouched and writes a descriptive message into `error`. External linkage
// (declared in lci/config.h): the KDL parse path takes untrusted bytes from
// cloned repos' .lci.kdl files, and fuzz_config_kdl drives it directly.
Config parse_kdl_content(const std::string& content, std::string& error,
                         std::vector<std::string>* warnings) {
    Parser parser(content);
    auto nodes = parser.parse_document();
    if (!parser.error().empty()) {
        error = parser.error();
        return make_default_config();
    }

    Config cfg = make_kdl_base_config();
    // User-level defaults sit between the KDL base and the project file:
    // scalar fields (index budgets, performance caps) survive unless the
    // project file overrides them. include/exclude are cleared below per
    // the long-standing project-file contract, so user include/exclude
    // apply only in the no-project-file path.
    std::string user_err;
    if (!overlay_user_config(cfg, user_err, warnings)) {
        error = user_err;
        return cfg;
    }
    cfg.include.clear();
    cfg.exclude.clear();

    if (!apply_kdl_nodes(cfg, nodes, error, warnings)) return cfg;

    return cfg;
}

// -- Public API ---------------------------------------------------------------

Config make_default_config() {
    Config cfg;

    std::error_code ec;
    auto cwd = fs::current_path(ec);
    cfg.project.root = ec ? "." : cwd.string();

    cfg.exclude = {
        "**/.git/**",
        "**/.*/**",
        "**/node_modules/**",
        "**/vendor/**",
        "**/bower_components/**",
        "**/jspm_packages/**",
        "**/dist/**",
        "**/build/**",
        // Hyphenated build-dir conventions: cmake-build-<cfg> (CLion),
        // build-<variant> (this repo's build-errlookup indexed 1.2 GB of
        // its own _deps sources before this line existed).
        "**/build-*/**",
        "**/cmake-build-*/**",
        "**/out/**",
        "**/target/**",
        "**/bin/**",
        "**/obj/**",
        // Generated artifacts checked into source trees. `compiled/` is the
        // vendored-bundle convention (next.js ships 100+ MB of minified JS
        // under src/compiled/ — symbol extraction on one such bundle cost
        // 4.4 GB RSS). Lockfiles are machine-written dependency pins, not
        // source; the multi-MB ones (pnpm-lock.yaml) are pure index weight.
        // Dot-dirs (.next, .turbo, .cache, .venv) are covered by "**/.*/**".
        "**/compiled/**",
        "**/coverage/**",
        "**/__generated__/**",
        "**/*.generated.*",
        "**/*.map",
        "**/*.min.map",
        "**/package-lock.json",
        "**/pnpm-lock.yaml",
        "**/yarn.lock",
        "**/bun.lockb",
        "**/composer.lock",
        "**/Cargo.lock",
        "**/poetry.lock",
        "**/uv.lock",
        "**/Pipfile.lock",
        "**/Gemfile.lock",
        "**/go.sum",
        // Test files and test/fixture directories are INDEXED. They are
        // first-party code: grep finds them, so search must too (parity
        // mandate — a default-invisible test corpus produced false-empty
        // results for every *_test.go identifier). Query-time filtering
        // stays available via search flags=nt (SearchOptions.exclude_tests)
        // and scoring already downranks FileCategory::Test (0.8x boost).
        // ui/ and public/ are likewise first-party source in web projects
        // and must not be blanket-excluded; generated bundles inside them
        // are still dropped by the *.min.js / *.bundle.js / dist rules.
        "**/*.min.js",
        "**/*.min.css",
        "**/*.bundle.js",
        "**/*.chunk.js",
        "**/*.avif",
        "**/*.webp",
        "**/*.wasm",
        "**/*.woff",
        "**/*.woff2",
        "**/*.ttf",
        "**/*.eot",
        "**/*.otf",
        "**/*.mp4",
        "**/*.avi",
        "**/*.mov",
        "**/*.wmv",
        "**/*.flv",
        "**/*.mkv",
        "**/*.webm",
        "**/*.m4v",
        "**/*.mpg",
        "**/*.mpeg",
        "**/*.3gp",
        "**/*.ogv",
        "**/*.mp3",
        "**/*.wav",
        "**/*.flac",
        "**/*.aac",
        "**/*.ogg",
        "**/*.wma",
        "**/*.m4a",
        "**/*.aiff",
        "**/*.ape",
        "**/*.doc",
        "**/*.docx",
        "**/*.docm",
        "**/*.xls",
        "**/*.xlsx",
        "**/*.xlsm",
        "**/*.xlsb",
        "**/*.xlt",
        "**/*.xltx",
        "**/*.xltm",
        "**/*.xlam",
        "**/*.ppt",
        "**/*.pptx",
        "**/*.pptm",
        "**/*.pps",
        "**/*.ppsx",
        "**/*.ppsm",
        "**/*.pot",
        "**/*.potx",
        "**/*.potm",
        "**/*.odt",
        "**/*.ods",
        "**/*.odp",
        "**/*.rtf",
        "**/*.pages",
        "**/*.numbers",
        "**/*.key",
        "**/*.swp",
        "**/*.swo",
        "**/*~",
        "**/__pycache__/**",
        "**/*.pyc",
        "**/Thumbs.db",
        "**/desktop.ini",
        "**/logs/**",
        "**/*.log",
    };

    return cfg;
}

namespace {

// Shared body of load_config / load_config_file. `must_exist` distinguishes
// the implicit <root>/.lci.kdl (absent means "use defaults") from a file the
// user named on the command line (absent means the flag did nothing, which
// must be an error rather than a silent fallback).
ConfigResult load_config_from(const fs::path& kdl_path,
                              const std::string& project_root,
                              bool must_exist) {
    std::vector<std::string> warnings;

    std::error_code ec;
    if (!fs::exists(kdl_path, ec)) {
        if (must_exist) {
            return {{}, "config file not found: " + kdl_path.string(), {}};
        }
        Config cfg = make_default_config();
        // No project file: user-level defaults overlay the full rich
        // defaults, include/exclude included.
        std::string user_err;
        if (!overlay_user_config(cfg, user_err, &warnings)) {
            return {{}, user_err, {}};
        }
        cfg.project.root = project_root;
        if (auto verr = validate_config(cfg); !verr.empty()) {
            return {{}, verr, std::move(warnings)};
        }
        return {std::move(cfg), {}, std::move(warnings)};
    }

    std::ifstream file(kdl_path);
    if (!file) {
        return {{}, "failed to read config: " + kdl_path.string(), {}};
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    std::string parse_error;
    Config cfg = parse_kdl_content(content, parse_error, &warnings);
    if (!parse_error.empty()) {
        return {{},
                "failed to parse " + kdl_path.string() + ": " + parse_error,
                std::move(warnings)};
    }

    // Resolve project root path
    if (cfg.project.root.empty()) {
        cfg.project.root = project_root;
    } else {
        cfg.project.root = expand_tilde(cfg.project.root);
        if (!fs::path(cfg.project.root).is_absolute()) {
            cfg.project.root =
                fs::weakly_canonical(fs::path(project_root) / cfg.project.root)
                    .string();
        }
    }

    // Validate here, not only in `lci config show`. Every other command used
    // the config unvalidated, so an out-of-range value (or a 0 meaning
    // "auto") reached the indexer as-is and misbehaved far from its cause.
    if (auto verr = validate_config(cfg); !verr.empty()) {
        return {{}, kdl_path.string() + ": " + verr, std::move(warnings)};
    }

    return {std::move(cfg), {}, std::move(warnings)};
}

}  // namespace

ConfigResult load_config(const std::string& project_root) {
    return load_config_from(fs::path(project_root) / ".lci.kdl", project_root,
                            /*must_exist=*/false);
}

ConfigResult load_config_file(const std::string& config_path,
                              const std::string& project_root) {
    return load_config_from(fs::path(config_path), project_root,
                            /*must_exist=*/true);
}

std::string validate_config(Config& cfg) {
    if (cfg.project.root.empty()) {
        return "project root cannot be empty";
    }

    if (cfg.index.max_file_size <= 0) {
        return "index.max_file_size must be positive";
    }
    if (cfg.index.max_file_size > 100 * 1024 * 1024) {
        return "index.max_file_size should not exceed 100MB";
    }
    if (cfg.index.max_total_size_mb <= 0) {
        return "index.max_total_size_mb must be positive";
    }
    if (cfg.index.overflow_policy != "reduced" &&
        cfg.index.overflow_policy != "reject") {
        return "index.overflow_policy must be \"reduced\" or \"reject\"";
    }
    if (cfg.index.max_file_count <= 0) {
        return "index.max_file_count must be positive";
    }
    if (cfg.index.data_file_token_cap < 0) {
        return "index.data_file_token_cap cannot be negative (0 disables)";
    }

    if (cfg.performance.max_memory_mb < 100) {
        return "performance.max_memory_mb must be at least 100";
    }
    if (cfg.performance.max_goroutines < 0) {
        return "performance.max_goroutines cannot be negative";
    }
    if (cfg.performance.parallel_file_workers < 0) {
        return "performance.parallel_file_workers cannot be negative";
    }

    if (cfg.server.idle_timeout_sec < 0) {
        return "server.idle_timeout_sec cannot be negative";
    }
    if (cfg.server.max_rss_mb < 0) {
        return "server.max_rss_mb cannot be negative (0 disables)";
    }
    if (cfg.server.max_instances < 0) {
        return "server.max_instances cannot be negative";
    }

    if (cfg.search.max_context_lines < 0) {
        return "search.max_context_lines cannot be negative";
    }
    if (cfg.search.max_results < 0) {
        return "search.max_results cannot be negative";
    }

    // Apply smart defaults
    int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (hw_threads < 1) hw_threads = 1;

    if (cfg.performance.max_goroutines == 0) {
        cfg.performance.max_goroutines = hw_threads;
    }
    if (cfg.performance.parallel_file_workers == 0) {
        cfg.performance.parallel_file_workers = hw_threads;
    }
    if (cfg.performance.max_memory_mb == 0) {
        cfg.performance.max_memory_mb = 1024;
    }
    if (cfg.search.max_context_lines == 0) {
        cfg.search.max_context_lines = 50;
    }
    if (cfg.index.priority_mode.empty()) {
        cfg.index.priority_mode = "recent";
    }

    return {};
}

}  // namespace lci
