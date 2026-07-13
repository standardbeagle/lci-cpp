#pragma once

#include <cstdint>
#include <string_view>

// ---------------------------------------------------------------------------
// Centralized file-extension -> language classification.
//
// One authoritative table consulted by every classification site (search
// engine is_code, reference_tracker export/family gating, import_resolver,
// git naming conventions, parser grammar selection, symbollinker can_handle,
// mcp language summary). Adding an extension edits ONLY this table instead of
// the ~8 hard-coded lists that used to drift (.pyw/.pyi/.pyx/.pxd appeared in
// some, not others).
//
// LAYERING: this header lives at the foundation level (top-level lci namespace,
// depends on nothing but <cstdint>/<string_view>) so every layer -- core,
// parser, git, search, symbollinker, mcp -- may include it. It deliberately
// returns a foundation-owned canonical LangId / LangFamily rather than the
// parser or git Language enums: returning those would force a foundation ->
// parser / foundation -> git dependency edge (core must not depend on parser).
// Consumers that need their own narrower enum map from LangId locally.
// ---------------------------------------------------------------------------

namespace lci {

/// Canonical language identity, superset of every language any consumer
/// classifies. Foundation-owned so no layer needs the parser/git enums to
/// read the table. Extensions with no tree-sitter grammar and no naming
/// convention (e.g. .lua) resolve to Unknown but may still be `is_code`.
enum class LangId : uint8_t {
    Unknown = 0,
    Go,
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
    Swift,
    Scala,
};

/// Coarse language family for cross-language link gating. Families, not exact
/// languages: C/C++ headers and JS/TS interop legitimately share symbols
/// within a family, but a Python call must never resolve into a C++ file.
/// Mirrors the enumerators ReferenceTracker::LangFamily used before this
/// table existed (which now aliases this type).
enum class LangFamily : uint8_t {
    kUnknown = 0,
    kPython,
    kCFamily,
    kJsTs,
    kGo,
    kJava,
    kCSharp,
    kRust,
    kPhp,
    kKotlin,
    kRuby,
    kZig,
};

/// The three facts every consumer needs from an extension.
struct LanguageInfo {
    LangId language;
    LangFamily family;
    bool is_code;
};

/// Lowercase display name for a LangId (matches the Go text output tokens:
/// "go", "python", "cpp", ...). Returns "unknown" for LangId::Unknown.
constexpr std::string_view to_string(LangId id) {
    switch (id) {
        case LangId::Go: return "go";
        case LangId::Python: return "python";
        case LangId::JavaScript: return "javascript";
        case LangId::TypeScript: return "typescript";
        case LangId::Rust: return "rust";
        case LangId::C: return "c";
        case LangId::Cpp: return "cpp";
        case LangId::Java: return "java";
        case LangId::CSharp: return "csharp";
        case LangId::PHP: return "php";
        case LangId::Kotlin: return "kotlin";
        case LangId::Zig: return "zig";
        case LangId::Ruby: return "ruby";
        case LangId::Swift: return "swift";
        case LangId::Scala: return "scala";
        case LangId::Unknown: return "unknown";
    }
    return "unknown";
}

namespace detail {

struct LangMapEntry {
    std::string_view ext;  // lowercase, includes leading dot
    LangId language;
    LangFamily family;
    bool is_code;
};

// Single source of truth. Reconciled from the eight previous hard-coded lists;
// where they drifted the most inclusive/correct classification wins (e.g. all
// five Python extensions everywhere). `.h` resolves to Cpp so header parsing
// uses the C++ grammar superset, matching the parser, the language summary, and
// the Go reference (internal/parser/parser.go groups .c/.h/.hpp as "cpp"). The
// git naming-convention site previously called .h "C"; it now applies the more
// permissive C++ convention to ambiguous headers -- a deliberate reconciliation
// of the pre-existing drift. CUDA/.cu is C-family for link gating but has no
// canonical LangId. Header-only, so the constexpr table has static storage and
// the lookup is inlined at each call site.
inline constexpr LangMapEntry kLangMap[] = {
    {".go", LangId::Go, LangFamily::kGo, true},

    {".py", LangId::Python, LangFamily::kPython, true},
    {".pyw", LangId::Python, LangFamily::kPython, true},
    {".pyi", LangId::Python, LangFamily::kPython, true},
    {".pyx", LangId::Python, LangFamily::kPython, true},
    {".pxd", LangId::Python, LangFamily::kPython, true},

    {".js", LangId::JavaScript, LangFamily::kJsTs, true},
    {".jsx", LangId::JavaScript, LangFamily::kJsTs, true},
    {".mjs", LangId::JavaScript, LangFamily::kJsTs, true},
    {".cjs", LangId::JavaScript, LangFamily::kJsTs, true},

    {".ts", LangId::TypeScript, LangFamily::kJsTs, true},
    {".tsx", LangId::TypeScript, LangFamily::kJsTs, true},
    {".mts", LangId::TypeScript, LangFamily::kJsTs, true},
    {".cts", LangId::TypeScript, LangFamily::kJsTs, true},

    {".rs", LangId::Rust, LangFamily::kRust, true},

    {".c", LangId::C, LangFamily::kCFamily, true},
    {".cpp", LangId::Cpp, LangFamily::kCFamily, true},
    {".cc", LangId::Cpp, LangFamily::kCFamily, true},
    {".cxx", LangId::Cpp, LangFamily::kCFamily, true},
    {".h", LangId::Cpp, LangFamily::kCFamily, true},
    {".hpp", LangId::Cpp, LangFamily::kCFamily, true},
    {".hh", LangId::Cpp, LangFamily::kCFamily, true},
    {".hxx", LangId::Cpp, LangFamily::kCFamily, true},
    {".h++", LangId::Cpp, LangFamily::kCFamily, true},
    {".cu", LangId::Unknown, LangFamily::kCFamily, true},

    {".java", LangId::Java, LangFamily::kJava, true},
    {".cs", LangId::CSharp, LangFamily::kCSharp, true},

    {".php", LangId::PHP, LangFamily::kPhp, true},
    {".phtml", LangId::PHP, LangFamily::kPhp, true},

    {".kt", LangId::Kotlin, LangFamily::kKotlin, true},
    {".kts", LangId::Kotlin, LangFamily::kKotlin, true},

    {".zig", LangId::Zig, LangFamily::kZig, true},
    {".rb", LangId::Ruby, LangFamily::kRuby, true},

    {".swift", LangId::Swift, LangFamily::kUnknown, true},
    {".scala", LangId::Scala, LangFamily::kUnknown, true},
    {".sc", LangId::Scala, LangFamily::kUnknown, true},

    // Code files with no linked grammar and no naming convention: `is_code`
    // only, so the search engine still counts them as source.
    {".lua", LangId::Unknown, LangFamily::kUnknown, true},
    {".pl", LangId::Unknown, LangFamily::kUnknown, true},
    {".pm", LangId::Unknown, LangFamily::kUnknown, true},
    {".r", LangId::Unknown, LangFamily::kUnknown, true},
    {".jl", LangId::Unknown, LangFamily::kUnknown, true},
    {".ex", LangId::Unknown, LangFamily::kUnknown, true},
    {".exs", LangId::Unknown, LangFamily::kUnknown, true},
    {".erl", LangId::Unknown, LangFamily::kUnknown, true},
    {".hrl", LangId::Unknown, LangFamily::kUnknown, true},
    {".hs", LangId::Unknown, LangFamily::kUnknown, true},
    {".clj", LangId::Unknown, LangFamily::kUnknown, true},
    {".cljs", LangId::Unknown, LangFamily::kUnknown, true},
    {".elm", LangId::Unknown, LangFamily::kUnknown, true},
    {".vue", LangId::Unknown, LangFamily::kUnknown, true},
    {".svelte", LangId::Unknown, LangFamily::kUnknown, true},
    {".nim", LangId::Unknown, LangFamily::kUnknown, true},
    {".v", LangId::Unknown, LangFamily::kUnknown, true},
    {".d", LangId::Unknown, LangFamily::kUnknown, true},
    {".m", LangId::Unknown, LangFamily::kUnknown, true},
    {".mm", LangId::Unknown, LangFamily::kUnknown, true},
};

}  // namespace detail

/// Classifies an extension (including the leading dot, case-insensitive).
/// Unknown extensions return {Unknown, kUnknown, false}.
inline LanguageInfo language_info(std::string_view ext) {
    for (const auto& e : detail::kLangMap) {
        if (ext.size() != e.ext.size()) continue;
        bool match = true;
        for (std::size_t i = 0; i < ext.size(); ++i) {
            char c = ext[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != e.ext[i]) {
                match = false;
                break;
            }
        }
        if (match) return {e.language, e.family, e.is_code};
    }
    return {LangId::Unknown, LangFamily::kUnknown, false};
}

/// Classifies a path by the substring after its last '.' (the extension).
inline LanguageInfo language_info_for_path(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return {LangId::Unknown, LangFamily::kUnknown, false};
    }
    return language_info(path.substr(dot));
}

}  // namespace lci
