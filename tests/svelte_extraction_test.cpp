// Svelte component support: markup-masking + JS/TS extraction.
//
// A .svelte file is reduced to its <script> block(s) by mask_svelte_script
// (geometry-preserving: every non-script byte becomes a space, newlines
// kept), then parsed with the existing JS/TS grammar. These tests pin the
// mask's contract and verify that symbols/references extracted from the
// masked buffer carry positions valid against the ORIGINAL file.

#include <lci/parser/parser.h>
#include <lci/parser/svelte_script.h>
#include <lci/parser/unified_extractor.h>

#include <gtest/gtest.h>
#include <tree_sitter/api.h>

#include <string>
#include <string_view>

namespace lci::parser {
namespace {

constexpr std::string_view kCounterSvelte = R"(<script>
  import { writable } from 'svelte/store';

  export let title;
  export let start = 0;

  const count = writable(start);

  function increment() {
    count.update(n => n + 1);
  }

  class Tracker {
    record(n) { return n; }
  }
</script>

<h1>{title}</h1>
<button on:click={increment}>+1</button>

<style>
  h1 { color: red; }
</style>
)";

ExtractionResults extract_svelte(std::string_view src,
                                 std::string_view path,
                                 SvelteScriptInfo* info_out = nullptr) {
    std::string masked;
    auto info = mask_svelte_script(src, masked);
    if (info_out) *info_out = info;
    Language lang = info.typescript ? Language::TypeScript
                                    : Language::JavaScript;
    UniqueParser parser = make_parser(lang);
    EXPECT_TRUE(parser);
    UniqueTree tree(ts_parser_parse_string(
        parser.get(), nullptr, masked.data(),
        static_cast<uint32_t>(masked.size())));
    EXPECT_TRUE(tree);

    UnifiedExtractor ue;
    ue.init(masked, 1, info.typescript ? ".ts" : ".js", path);
    ue.extract(tree.get());
    return ue.get_results();
}

const Symbol* find_symbol(const ExtractionResults& r, std::string_view name) {
    for (const auto& s : r.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Mask contract
// ---------------------------------------------------------------------------

TEST(SvelteScriptMask, PreservesGeometryAndScriptContent) {
    std::string masked;
    auto info = mask_svelte_script(kCounterSvelte, masked);
    EXPECT_TRUE(info.has_script);
    EXPECT_FALSE(info.typescript);
    ASSERT_EQ(masked.size(), kCounterSvelte.size());
    // Newline positions identical.
    for (std::size_t i = 0; i < masked.size(); ++i) {
        if (kCounterSvelte[i] == '\n') {
            EXPECT_EQ(masked[i], '\n') << "at byte " << i;
        }
    }
    // Script content survives at the same offsets.
    auto fn = kCounterSvelte.find("function increment");
    ASSERT_NE(fn, std::string_view::npos);
    EXPECT_EQ(masked.substr(fn, 18), "function increment");
    // Markup and style are blanked.
    EXPECT_EQ(masked.find("<h1>"), std::string::npos);
    EXPECT_EQ(masked.find("color: red"), std::string::npos);
    EXPECT_EQ(masked.find("<script"), std::string::npos);
}

TEST(SvelteScriptMask, DetectsTypescriptLangAttribute) {
    std::string masked;
    auto info = mask_svelte_script(
        "<script lang=\"ts\">let x: number = 1;</script>", masked);
    EXPECT_TRUE(info.has_script);
    EXPECT_TRUE(info.typescript);
    EXPECT_NE(masked.find("let x: number = 1;"), std::string::npos);

    auto info2 = mask_svelte_script(
        "<script lang='typescript'>let y = 2;</script>", masked);
    EXPECT_TRUE(info2.typescript);
}

TEST(SvelteScriptMask, HandlesModuleAndInstanceBlocks) {
    std::string masked;
    auto info = mask_svelte_script(
        "<script context=\"module\">export const shared = 1;</script>\n"
        "<div>x</div>\n"
        "<script>const local = 2;</script>\n",
        masked);
    EXPECT_TRUE(info.has_script);
    EXPECT_NE(masked.find("export const shared = 1;"), std::string::npos);
    EXPECT_NE(masked.find("const local = 2;"), std::string::npos);
    EXPECT_EQ(masked.find("<div>"), std::string::npos);
}

TEST(SvelteScriptMask, NoScriptBlanksEverything) {
    std::string masked;
    auto info = mask_svelte_script("<h1>static</h1>\n", masked);
    EXPECT_FALSE(info.has_script);
    EXPECT_EQ(masked, "               \n");
}

TEST(SvelteScriptMask, QuotedGtDoesNotEndOpenTag) {
    std::string masked;
    auto info = mask_svelte_script(
        "<script data-x=\"a > b\">const q = 1;</script>", masked);
    EXPECT_TRUE(info.has_script);
    EXPECT_NE(masked.find("const q = 1;"), std::string::npos);
    EXPECT_EQ(masked.find("a > b"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Extraction through the masked buffer
// ---------------------------------------------------------------------------

TEST(SvelteExtraction, ScriptSymbolsAtOriginalPositions) {
    auto r = extract_svelte(kCounterSvelte, "/app/Counter.svelte");

    const Symbol* inc = find_symbol(r, "increment");
    ASSERT_NE(inc, nullptr);
    EXPECT_EQ(inc->type, SymbolType::Function);
    EXPECT_EQ(inc->line, 9);  // 1-based line in the ORIGINAL file

    const Symbol* tracker = find_symbol(r, "Tracker");
    ASSERT_NE(tracker, nullptr);
    EXPECT_EQ(tracker->type, SymbolType::Class);
    EXPECT_EQ(tracker->line, 13);

    // export let props + store const are variables/constants.
    EXPECT_NE(find_symbol(r, "title"), nullptr);
    EXPECT_NE(find_symbol(r, "start"), nullptr);
    EXPECT_NE(find_symbol(r, "count"), nullptr);
}

TEST(SvelteExtraction, ScriptReferencesAndImports) {
    auto r = extract_svelte(kCounterSvelte, "/app/Counter.svelte");
    // The store factory call is a reference.
    bool saw_writable_call = false;
    for (const auto& ref : r.references) {
        if (ref.referenced_name == "writable") saw_writable_call = true;
    }
    EXPECT_TRUE(saw_writable_call);
    // The svelte/store import is recorded.
    bool saw_import = false;
    for (const auto& imp : r.imports) {
        if (imp.path.find("svelte/store") != std::string::npos) {
            saw_import = true;
        }
    }
    EXPECT_TRUE(saw_import);
}

TEST(SvelteExtraction, TypescriptScriptUsesTsGrammar) {
    SvelteScriptInfo info;
    auto r = extract_svelte(
        "<script lang=\"ts\">\n"
        "interface Props { title: string }\n"
        "export let title: string;\n"
        "function greet(name: string): string { return name; }\n"
        "</script>\n<p>{title}</p>\n",
        "/app/Greeter.svelte", &info);
    EXPECT_TRUE(info.typescript);
    const Symbol* greet = find_symbol(r, "greet");
    ASSERT_NE(greet, nullptr);
    EXPECT_EQ(greet->type, SymbolType::Function);
    EXPECT_EQ(greet->line, 4);
    EXPECT_NE(find_symbol(r, "Props"), nullptr);
}

}  // namespace
}  // namespace lci::parser
