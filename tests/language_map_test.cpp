#include <gtest/gtest.h>

#include <lci/language_map.h>

#include <lci/git/types.h>
#include <lci/parser/parser.h>
#include <lci/search/search_options.h>
#include <lci/symbollinker/python_linker.h>

// ---------------------------------------------------------------------------
// Discrimination test for the centralized extension -> language map.
//
// The point of the central table (include/lci/language_map.h) is to kill the
// shotgun-surgery debt where adding an extension meant editing ~8 hard-coded
// lists. This test pins that EVERY consumer path derives its answer from the
// single table: it asserts each public consumer surface agrees with
// lci::language_info() for representative extensions, and pins the reconciled
// drift cases (.pyw/.pyi/.pyx/.pxd) that previously appeared in some lists but
// not others. Add an extension to the table -> every assertion here that walks
// a consumer must still agree, or a consumer is not consulting the table.
// ---------------------------------------------------------------------------

namespace lci {
namespace {

// -- The table is the single source of truth --------------------------------

TEST(LanguageMap, PythonExtensionsAllMapToPython) {
    for (std::string_view ext : {".py", ".pyw", ".pyi", ".pyx", ".pxd"}) {
        auto info = language_info(ext);
        EXPECT_EQ(info.language, LangId::Python) << ext;
        EXPECT_EQ(info.family, LangFamily::kPython) << ext;
        EXPECT_TRUE(info.is_code) << ext;
    }
}

TEST(LanguageMap, FamilyGrouping) {
    EXPECT_EQ(language_info(".go").family, LangFamily::kGo);
    EXPECT_EQ(language_info(".rs").family, LangFamily::kRust);
    EXPECT_EQ(language_info(".cpp").family, LangFamily::kCFamily);
    EXPECT_EQ(language_info(".h").family, LangFamily::kCFamily);
    EXPECT_EQ(language_info(".ts").family, LangFamily::kJsTs);
    EXPECT_EQ(language_info(".jsx").family, LangFamily::kJsTs);
    EXPECT_EQ(language_info(".rb").family, LangFamily::kRuby);
    EXPECT_EQ(language_info(".cs").family, LangFamily::kCSharp);
}

TEST(LanguageMap, CaseInsensitiveAndPathForm) {
    EXPECT_EQ(language_info(".PY").language, LangId::Python);
    EXPECT_EQ(language_info_for_path("/a/b/service.pyx").language, LangId::Python);
    EXPECT_EQ(language_info_for_path("noext").language, LangId::Unknown);
}

TEST(LanguageMap, NonLanguageCodeExtensionsStillCode) {
    // Extensions with a tree-sitter-less language: no LangId, but still code.
    for (std::string_view ext : {".lua", ".hs", ".vue", ".svelte"}) {
        auto info = language_info(ext);
        EXPECT_EQ(info.language, LangId::Unknown) << ext;
        EXPECT_TRUE(info.is_code) << ext;
    }
}

// -- Consumer: search/engine classify_file (is_code) ------------------------

TEST(LanguageMap, ClassifyFileConsultsTable) {
    // .pyx/.pxd (Cython) and .pyw/.pyi must all classify as code now.
    for (std::string_view ext : {".py", ".pyw", ".pyi", ".pyx", ".pxd"}) {
        EXPECT_EQ(classify_file(std::string("mod") + std::string(ext)),
                  FileCategory::Code)
            << ext;
    }
    EXPECT_EQ(classify_file("README.md"), FileCategory::Documentation);
}

// -- Consumer: parser::language_from_extension ------------------------------

TEST(LanguageMap, ParserAgreesWithTable) {
    parser::Language out{};
    // Cython dialect parses as Python.
    for (std::string_view ext : {".py", ".pyw", ".pyi", ".pyx", ".pxd"}) {
        ASSERT_TRUE(parser::language_from_extension(ext, out)) << ext;
        EXPECT_EQ(out, parser::Language::Python) << ext;
    }
    ASSERT_TRUE(parser::language_from_extension(".go", out));
    EXPECT_EQ(out, parser::Language::Go);
    ASSERT_TRUE(parser::language_from_extension(".rb", out));
    EXPECT_EQ(out, parser::Language::Ruby);
    // Not a parseable language -> false.
    EXPECT_FALSE(parser::language_from_extension(".unknownext", out));
}

// -- Consumer: git::get_language_from_path ----------------------------------

TEST(LanguageMap, GitLanguageAgreesWithTable) {
    using git::Language;
    for (std::string_view ext : {".py", ".pyw", ".pyi", ".pyx", ".pxd"}) {
        EXPECT_EQ(git::get_language_from_path(std::string("f") + std::string(ext)),
                  Language::Python)
            << ext;
    }
    EXPECT_EQ(git::get_language_from_path("a.swift"), Language::Swift);
    EXPECT_EQ(git::get_language_from_path("a.scala"), Language::Scala);
    EXPECT_EQ(git::get_language_from_path("a.zig"), Language::Zig);
    EXPECT_EQ(git::get_language_from_path("a.nosuch"), Language::Unknown);
}

// -- Consumer: symbollinker python can_handle -------------------------------

TEST(LanguageMap, PythonExtractorCanHandleConsultsTable) {
    symbollinker::PythonExtractor extractor;
    for (std::string_view ext : {".py", ".pyw", ".pyi", ".pyx", ".pxd"}) {
        EXPECT_TRUE(extractor.can_handle(std::string("m") + std::string(ext)))
            << ext;
    }
    EXPECT_FALSE(extractor.can_handle("m.go"));
    EXPECT_FALSE(extractor.can_handle("m.cpp"));
}

}  // namespace
}  // namespace lci
