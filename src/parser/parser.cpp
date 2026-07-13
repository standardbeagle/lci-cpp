#include <lci/parser/parser.h>

#include <lci/language_map.h>

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
    // Extension identity comes from the centralized table (language_map.h);
    // this only maps the canonical LangId onto the parser's grammar enum,
    // returning false for languages with no linked tree-sitter grammar.
    // Note: Cython .pyx/.pxd map to Python -- tree-sitter-python parses their
    // bodies best-effort and the unified extractor recovers cpdef/cdef
    // signatures the grammar cannot. C/C++ headers use the C++ grammar
    // superset (the table classifies .h/.hpp/.hxx/.hh as Cpp).
    switch (language_info(ext).language) {
        case LangId::Go: out = Language::Go; return true;
        case LangId::Python: out = Language::Python; return true;
        case LangId::JavaScript: out = Language::JavaScript; return true;
        case LangId::TypeScript: out = Language::TypeScript; return true;
        case LangId::Rust: out = Language::Rust; return true;
        case LangId::C: out = Language::C; return true;
        case LangId::Cpp: out = Language::Cpp; return true;
        case LangId::Java: out = Language::Java; return true;
        case LangId::CSharp: out = Language::CSharp; return true;
        case LangId::PHP: out = Language::PHP; return true;
        case LangId::Kotlin: out = Language::Kotlin; return true;
        case LangId::Zig: out = Language::Zig; return true;
        case LangId::Ruby: out = Language::Ruby; return true;
        case LangId::Swift:   // no tree-sitter-swift grammar linked
        case LangId::Scala:   // no tree-sitter-scala grammar linked
        case LangId::Unknown:
            return false;
    }
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
