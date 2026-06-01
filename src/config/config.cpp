#include <lci/config.h>

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
                    pos_ += 2;
                    while (pos_ + 1 < src_.size() &&
                           !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) {
                        if (src_[pos_] == '\n') ++line_;
                        ++pos_;
                    }
                    if (pos_ + 1 < src_.size()) pos_ += 2;
                    continue;
                }
            }
            break;
        }
    }

    Token lex_string() {
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
                val += src_[pos_];
            }
            ++pos_;
        }
        if (pos_ < src_.size()) ++pos_;  // skip closing quote
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
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), val);
        if (ec != std::errc{}) val = 0;
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
            advance();
            node.children = parse_document();
            if (!error_.empty()) return node;
            if (cur_.kind == TokenKind::RBrace) advance();
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

int64_t parse_size_string(const std::string& s) {
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
    auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), num);
    if (ec != std::errc{}) return 0;
    return num * multiplier;
}

// -- Apply KDL nodes to Config ------------------------------------------------

void apply_project(Config& cfg, const KdlNode& node) {
    for (const auto& child : node.children) {
        if (child.name == "root") get_string(child, cfg.project.root);
        else if (child.name == "name") get_string(child, cfg.project.name);
    }
}

void apply_index(Config& cfg, const KdlNode& node) {
    for (const auto& child : node.children) {
        if (child.name == "max_file_size") {
            std::string sz;
            if (get_string(child, sz)) {
                cfg.index.max_file_size = parse_size_string(sz);
            } else {
                int v = 0;
                if (get_int(child, v)) cfg.index.max_file_size = v;
            }
        } else if (child.name == "max_total_size_mb") {
            int v = 0;
            if (get_int(child, v)) cfg.index.max_total_size_mb = v;
        } else if (child.name == "max_file_count") {
            get_int(child, cfg.index.max_file_count);
        } else if (child.name == "follow_symlinks") {
            get_bool(child, cfg.index.follow_symlinks);
        } else if (child.name == "smart_size_control") {
            get_bool(child, cfg.index.smart_size_control);
        } else if (child.name == "priority_mode") {
            get_string(child, cfg.index.priority_mode);
        } else if (child.name == "respect_gitignore") {
            get_bool(child, cfg.index.respect_gitignore);
        } else if (child.name == "watch_mode") {
            get_bool(child, cfg.index.watch_mode);
        } else if (child.name == "watch_debounce_ms") {
            get_int(child, cfg.index.watch_debounce_ms);
        }
    }
}

void apply_performance(Config& cfg, const KdlNode& node) {
    for (const auto& child : node.children) {
        if (child.name == "max_memory_mb") {
            get_int(child, cfg.performance.max_memory_mb);
        } else if (child.name == "max_goroutines") {
            get_int(child, cfg.performance.max_goroutines);
        } else if (child.name == "debounce_ms") {
            get_int(child, cfg.performance.debounce_ms);
        } else if (child.name == "startup_delay_ms") {
            get_int(child, cfg.performance.startup_delay_ms);
        }
    }
}

void apply_ranking(SearchRankingConfig& ranking, const KdlNode& node) {
    for (const auto& child : node.children) {
        if (child.name == "enabled") get_bool(child, ranking.enabled);
        else if (child.name == "code_file_boost") get_double(child, ranking.code_file_boost);
        else if (child.name == "doc_file_penalty") get_double(child, ranking.doc_file_penalty);
        else if (child.name == "config_file_boost") get_double(child, ranking.config_file_boost);
        else if (child.name == "require_symbol") get_bool(child, ranking.require_symbol);
        else if (child.name == "non_symbol_penalty") get_double(child, ranking.non_symbol_penalty);
    }
}

void apply_search(Config& cfg, const KdlNode& node) {
    for (const auto& child : node.children) {
        if (child.name == "max_results") {
            get_int(child, cfg.search.max_results);
        } else if (child.name == "max_context_lines") {
            get_int(child, cfg.search.max_context_lines);
        } else if (child.name == "enable_fuzzy") {
            get_bool(child, cfg.search.enable_fuzzy);
        } else if (child.name == "merge_file_results") {
            get_bool(child, cfg.search.merge_file_results);
        } else if (child.name == "ensure_complete_stmt") {
            get_bool(child, cfg.search.ensure_complete_stmt);
        } else if (child.name == "include_leading_comments") {
            get_bool(child, cfg.search.include_leading_comments);
        } else if (child.name == "ranking") {
            apply_ranking(cfg.search.ranking, child);
        }
    }
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

// Base config used when a .lci.kdl file IS loaded. This intentionally
// diverges from make_default_config() (the no-file path) to match Go's
// parseKDL in internal/config/kdl_config.go: that function builds its base
// Config from a struct literal that OMITS several index/performance fields,
// so they take Go's zero value rather than the richer no-file defaults from
// config.go's Load(). Fields NOT set here are left at the C++ struct
// defaults declared in config.h — those defaults match Go's parseKDL literal
// for every field the literal does set (max_file_size, max_total_size_mb,
// max_file_count, follow_symlinks, smart_size_control, priority_mode,
// max_memory_mb, debounce_ms). Only the omitted fields below differ and must
// be reset to Go's zero value.
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

// Parses KDL content into a Config. On a malformed document, leaves `cfg`
// untouched and writes a descriptive message into `error`.
Config parse_kdl_content(const std::string& content, std::string& error) {
    Parser parser(content);
    auto nodes = parser.parse_document();
    if (!parser.error().empty()) {
        error = parser.error();
        return make_default_config();
    }

    Config cfg = make_kdl_base_config();
    cfg.include.clear();
    cfg.exclude.clear();

    for (const auto& node : nodes) {
        if (node.name == "project") apply_project(cfg, node);
        else if (node.name == "index") apply_index(cfg, node);
        else if (node.name == "performance") apply_performance(cfg, node);
        else if (node.name == "search") apply_search(cfg, node);
        else if (node.name == "include") cfg.include = collect_strings(node);
        else if (node.name == "exclude") cfg.exclude = collect_strings(node);
        else if (node.name == "propagation_config_dir") get_string(node, cfg.propagation_config_dir);
        else if (node.name == "synonyms") {
            auto result = apply_synonyms(node);
            if (!result) {
                error = result.error().to_string();
                return cfg;
            }
            cfg.synonyms = std::move(result.value());
        }
    }

    return cfg;
}

}  // namespace

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
        "**/out/**",
        "**/target/**",
        "**/bin/**",
        "**/obj/**",
        "**/ui/**",
        "**/public/**",
        "**/*.min.js",
        "**/*.min.css",
        "**/*.bundle.js",
        "**/*.chunk.js",
        "**/*.min.map",
        "**/*_test.go",
        "**/*_tests.go",
        "**/*_test.py",
        "**/*_tests.py",
        "**/test_*.py",
        "**/tests_*.py",
        "**/*.test.js",
        "**/*.test.ts",
        "**/*.test.tsx",
        "**/*.test.jsx",
        "**/*.spec.js",
        "**/*.spec.ts",
        "**/*.spec.tsx",
        "**/*.spec.jsx",
        "**/test_*",
        "**/tests_*",
        "**/__tests__/**",
        "**/test/**",
        "**/tests/**",
        "**/testdata/**",
        "**/__testdata__/**",
        "**/fixtures/**",
        "**/.test/**",
        "**/*_test.rb",
        "**/*_spec.rb",
        "**/*Test.java",
        "**/*Tests.java",
        "**/*TestCase.java",
        "**/*Test.cs",
        "**/*Tests.cs",
        "**/*Test.csproj",
        "**/tests/**",
        "**/*Test.php",
        "**/*TestCase.php",
        "**/*Test.kt",
        "**/*Tests.kt",
        "**/*TestCase.kt",
        "**/*Test.swift",
        "**/*Test.m",
        "**/*Test.h",
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

ConfigResult load_config(const std::string& project_root) {
    fs::path kdl_path = fs::path(project_root) / ".lci.kdl";

    std::error_code ec;
    if (!fs::exists(kdl_path, ec)) {
        Config cfg = make_default_config();
        cfg.project.root = project_root;
        return {std::move(cfg), {}};
    }

    std::ifstream file(kdl_path);
    if (!file) {
        return {{}, "failed to read .lci.kdl: " + kdl_path.string()};
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    std::string parse_error;
    Config cfg = parse_kdl_content(content, parse_error);
    if (!parse_error.empty()) {
        return {{}, "failed to parse " + kdl_path.string() + ": " + parse_error};
    }

    // Resolve project root path
    if (cfg.project.root.empty()) {
        cfg.project.root = project_root;
    } else if (!fs::path(cfg.project.root).is_absolute()) {
        cfg.project.root = fs::weakly_canonical(fs::path(project_root) / cfg.project.root).string();
    }

    return {std::move(cfg), {}};
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
    if (cfg.index.max_file_count <= 0) {
        return "index.max_file_count must be positive";
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
