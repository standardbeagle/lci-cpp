#pragma once

// Minimal KDL reader — the single parser behind every KDL document lci reads:
// a project's `.lci.kdl`, the user-level defaults file, and the attribute
// ruleset shipped inside the binary. It lived inside config.cpp until the
// classifier needed the same grammar; a second copy would have drifted the
// moment either side gained a token.
//
// Supported subset: nodes with string/number/bool arguments, `key=value`
// properties, child blocks, `//` and `/* */` comments, `;` separators.
// Deliberately NOT supported: KDL-v2 `#true`/`#false` (the lexer reports them
// as an error naming the offending token), type annotations, slashdash.

#include <string>
#include <string_view>
#include <vector>

namespace lci {
namespace kdl {

enum class TokenKind {
    Ident,
    String,
    Number,
    Bool,
    Equals,
    LBrace,
    RBrace,
    Eof,
    Error,
};

struct Token {
    TokenKind kind{};
    std::string text;
    double num_val{};
    bool bool_val{};
    int line{1};
};

/// One `key=value` property on a node. Value keeps its token kind so a
/// consumer can tell `rank=0` from `rank="0"`.
struct Property {
    std::string key;
    Token value;
};

struct Node {
    std::string name;
    std::vector<Token> args;
    std::vector<Property> props;
    std::vector<Node> children;

    /// First string argument, or empty when the node has none.
    std::string_view first_string() const;
    /// Every string argument, in order.
    std::vector<std::string> string_args() const;
    /// Property lookup. Returns nullptr when absent.
    const Token* prop(std::string_view key) const;
    /// Integer property with a fallback for absent/non-numeric values.
    int int_prop(std::string_view key, int fallback) const;
    /// Child node by name, or nullptr.
    const Node* child(std::string_view name) const;
};

/// Historical spelling used across the config reader.
using KdlNode = Node;

/// Parses a whole document. On a malformed document the returned nodes are
/// whatever parsed before the failure and `error` describes the first
/// problem (line number + offending token); on success `error` is cleared.
std::vector<Node> parse(std::string_view source, std::string& error);

}  // namespace kdl
}  // namespace lci
