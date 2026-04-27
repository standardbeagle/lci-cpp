#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

struct TSParser;
struct TSTree;
struct TSLanguage;

namespace lci::parser {

/// Supported programming languages for parsing.
/// Matches the Go Language type in internal/parser/parser.go.
enum class Language : uint8_t {
    Go = 0,
    Python,
    JavaScript,
    TypeScript,
    Rust,
    C,
    Cpp,
    Java,
    CSharp,
    PHP,
    Kotlin,
    Zig,
    Ruby,
};

/// Total number of supported languages.
inline constexpr int kLanguageCount = 13;

/// Returns the string name for a Language value.
constexpr std::string_view to_string(Language lang) {
    switch (lang) {
        case Language::Go: return "go";
        case Language::Python: return "python";
        case Language::JavaScript: return "javascript";
        case Language::TypeScript: return "typescript";
        case Language::Rust: return "rust";
        case Language::C: return "c";
        case Language::Cpp: return "cpp";
        case Language::Java: return "java";
        case Language::CSharp: return "csharp";
        case Language::PHP: return "php";
        case Language::Kotlin: return "kotlin";
        case Language::Zig: return "zig";
        case Language::Ruby: return "ruby";
    }
    return "unknown";
}

/// Detects the language from a file extension (including the dot).
/// Returns true and sets `out` if the extension is recognized.
bool language_from_extension(std::string_view ext, Language& out);

/// Returns the TSLanguage pointer for a given Language enum value.
/// The returned pointer has static lifetime (owned by the grammar library).
const TSLanguage* get_ts_language(Language lang);

/// Custom deleter for TSParser pointers.
struct ParserDeleter {
    void operator()(TSParser* p) const;
};

/// Custom deleter for TSTree pointers.
struct TreeDeleter {
    void operator()(TSTree* t) const;
};

/// Owning handle to a TSParser with automatic cleanup.
using UniqueParser = std::unique_ptr<TSParser, ParserDeleter>;

/// Owning handle to a TSTree with automatic cleanup.
using UniqueTree = std::unique_ptr<TSTree, TreeDeleter>;

/// Creates a new TSParser configured for the given language.
/// Returns nullptr if the language grammar could not be loaded.
UniqueParser make_parser(Language lang);

}  // namespace lci::parser
