#include <lci/kdl.h>

#include <lci/core/portable.h>

#include <cctype>
#include <string>
#include <utility>

namespace lci {
namespace kdl {
namespace {

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

        if (c == '=') { ++pos_; return stamp({TokenKind::Equals, "=", 0, false}); }
        if (c == '{') { ++pos_; return stamp({TokenKind::LBrace, "{", 0, false}); }
        if (c == '}') { ++pos_; return stamp({TokenKind::RBrace, "}", 0, false}); }

        if (c == '"') return stamp(lex_string());

        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            return stamp(lex_number_or_ident());
        }

        return stamp(lex_ident_or_bool());
    }

    /// True when the next significant character is '=' — the one lookahead
    /// the parser needs to tell a property (`rank=0`) from a child node named
    /// `rank`. Non-consuming: whitespace/comment skipping is undone.
    bool peek_is_equals() {
        size_t save_pos = pos_;
        int save_line = line_;
        std::string save_err = pending_error_;
        skip_ws_and_comments();
        bool eq = pos_ < src_.size() && src_[pos_] == '=';
        pos_ = save_pos;
        line_ = save_line;
        pending_error_ = std::move(save_err);
        return eq;
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

// -- KDL parser ---------------------------------------------------------------

class Parser {
  public:
    explicit Parser(std::string_view src) : lex_(src) { advance(); }

    std::vector<Node> parse_document() {
        std::vector<Node> nodes;
        while (cur_.kind != TokenKind::Eof && cur_.kind != TokenKind::RBrace) {
            if (cur_.kind == TokenKind::Error) {
                set_error();
                return nodes;
            }
            if (cur_.kind == TokenKind::Ident ||
                cur_.kind == TokenKind::String) {
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

    Node parse_node() {
        Node node;
        node.name = cur_.text;
        // A quoted string in node position is a KDL quoted node name — the
        // pattern-list block form `include { "*.rs" "*.go" }` puts every
        // pattern there. It is a LEAF: consuming trailing strings as args
        // would swallow the sibling patterns (`"*.go"` would become an
        // argument of `"*.rs"` and vanish from collect_strings).
        const bool quoted_name = cur_.kind == TokenKind::String;
        advance();
        if (quoted_name) return node;

        // Collect arguments and `key=value` properties until a child block or
        // the next node name at this level. A property is an identifier the
        // lexer already consumed followed by '=', which is why the ident case
        // has to peek: `rank=0` and a bare child node `rank` differ only by
        // the token after the name.
        while (true) {
            if (cur_.kind == TokenKind::String ||
                cur_.kind == TokenKind::Number ||
                cur_.kind == TokenKind::Bool) {
                node.args.push_back(cur_);
                advance();
                continue;
            }
            if (cur_.kind == TokenKind::Ident && lex_.peek_is_equals()) {
                Property p;
                p.key = cur_.text;
                advance();  // past the key
                advance();  // past '='
                if (cur_.kind != TokenKind::String &&
                    cur_.kind != TokenKind::Number &&
                    cur_.kind != TokenKind::Bool) {
                    error_ = "line " + std::to_string(cur_.line) +
                             ": property '" + p.key +
                             "' needs a string, number, or bool value";
                    return node;
                }
                p.value = cur_;
                node.props.push_back(std::move(p));
                advance();
                continue;
            }
            break;
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
}  // namespace

std::string_view Node::first_string() const {
    for (const auto& a : args) {
        if (a.kind == TokenKind::String) return a.text;
    }
    return {};
}

std::vector<std::string> Node::string_args() const {
    std::vector<std::string> out;
    out.reserve(args.size());
    for (const auto& a : args) {
        if (a.kind == TokenKind::String) out.push_back(a.text);
    }
    return out;
}

const Token* Node::prop(std::string_view key) const {
    for (const auto& p : props) {
        if (p.key == key) return &p.value;
    }
    return nullptr;
}

int Node::int_prop(std::string_view key, int fallback) const {
    const Token* t = prop(key);
    if (t == nullptr || t->kind != TokenKind::Number) return fallback;
    return static_cast<int>(t->num_val);
}

const Node* Node::child(std::string_view name) const {
    for (const auto& c : children) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

std::vector<Node> parse(std::string_view source, std::string& error) {
    Parser parser(source);
    auto nodes = parser.parse_document();
    error = parser.error();
    return nodes;
}

}  // namespace kdl
}  // namespace lci
