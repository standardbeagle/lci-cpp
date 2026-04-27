#include <lci/parser/parser.h>

#include <tree_sitter/api.h>

// Each grammar exposes a C function returning its TSLanguage pointer.
extern "C" {
const TSLanguage* tree_sitter_go();
const TSLanguage* tree_sitter_python();
const TSLanguage* tree_sitter_javascript();
const TSLanguage* tree_sitter_typescript();
const TSLanguage* tree_sitter_tsx();
const TSLanguage* tree_sitter_rust();
const TSLanguage* tree_sitter_c();
const TSLanguage* tree_sitter_cpp();
const TSLanguage* tree_sitter_java();
const TSLanguage* tree_sitter_c_sharp();
const TSLanguage* tree_sitter_php();
const TSLanguage* tree_sitter_kotlin();
const TSLanguage* tree_sitter_zig();
const TSLanguage* tree_sitter_ruby();
}

namespace lci::parser {

bool language_from_extension(std::string_view ext, Language& out) {
    if (ext == ".go") { out = Language::Go; return true; }
    if (ext == ".py") { out = Language::Python; return true; }
    if (ext == ".js" || ext == ".jsx") { out = Language::JavaScript; return true; }
    if (ext == ".ts" || ext == ".tsx") { out = Language::TypeScript; return true; }
    if (ext == ".rs") { out = Language::Rust; return true; }
    if (ext == ".c") { out = Language::C; return true; }
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
        ext == ".h" || ext == ".hpp") { out = Language::Cpp; return true; }
    if (ext == ".java") { out = Language::Java; return true; }
    if (ext == ".cs") { out = Language::CSharp; return true; }
    if (ext == ".php" || ext == ".phtml") { out = Language::PHP; return true; }
    if (ext == ".kt" || ext == ".kts") { out = Language::Kotlin; return true; }
    if (ext == ".zig") { out = Language::Zig; return true; }
    if (ext == ".rb") { out = Language::Ruby; return true; }
    return false;
}

const TSLanguage* get_ts_language(Language lang) {
    switch (lang) {
        case Language::Go: return tree_sitter_go();
        case Language::Python: return tree_sitter_python();
        case Language::JavaScript: return tree_sitter_javascript();
        case Language::TypeScript: return tree_sitter_typescript();
        case Language::Rust: return tree_sitter_rust();
        case Language::C: return tree_sitter_c();
        case Language::Cpp: return tree_sitter_cpp();
        case Language::Java: return tree_sitter_java();
        case Language::CSharp: return tree_sitter_c_sharp();
        case Language::PHP: return tree_sitter_php();
        case Language::Kotlin: return tree_sitter_kotlin();
        case Language::Zig: return tree_sitter_zig();
        case Language::Ruby: return tree_sitter_ruby();
    }
    return nullptr;
}

void ParserDeleter::operator()(TSParser* p) const {
    if (p) ts_parser_delete(p);
}

void TreeDeleter::operator()(TSTree* t) const {
    if (t) ts_tree_delete(t);
}

UniqueParser make_parser(Language lang) {
    const TSLanguage* ts_lang = get_ts_language(lang);
    if (!ts_lang) return nullptr;

    TSParser* parser = ts_parser_new();
    if (!parser) return nullptr;

    if (!ts_parser_set_language(parser, ts_lang)) {
        ts_parser_delete(parser);
        return nullptr;
    }

    return UniqueParser(parser);
}

}  // namespace lci::parser
