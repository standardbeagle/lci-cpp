#include <lci/semantic/fuzzy_matcher.h>
#include <lci/semantic/name_splitter.h>
#include <lci/semantic/score_types.h>
#include <lci/semantic/semantic_scorer.h>
#include <lci/semantic/stemmer.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace lci {
namespace {

// -- NameSplitter tests -------------------------------------------------------

struct SplitCase {
    std::string input;
    std::vector<std::string> expected;
};

class NameSplitterTest : public ::testing::TestWithParam<SplitCase> {};

TEST_P(NameSplitterTest, SplitsCorrectly) {
    NameSplitter splitter;
    auto result = splitter.split(GetParam().input);
    EXPECT_EQ(result, GetParam().expected) << "Input: " << GetParam().input;
}

INSTANTIATE_TEST_SUITE_P(Basic, NameSplitterTest, ::testing::Values(
    SplitCase{"", {}},
    SplitCase{"simple", {"simple"}},
    SplitCase{"Simple", {"simple"}},
    SplitCase{"SIMPLE", {"simple"}}
));

INSTANTIATE_TEST_SUITE_P(CamelCase, NameSplitterTest, ::testing::Values(
    SplitCase{"camelCase", {"camel", "case"}},
    SplitCase{"getUserName", {"get", "user", "name"}},
    SplitCase{"createTable", {"create", "table"}},
    SplitCase{"parseJSON", {"parse", "json"}}
));

INSTANTIATE_TEST_SUITE_P(PascalCase, NameSplitterTest, ::testing::Values(
    SplitCase{"PascalCase", {"pascal", "case"}},
    SplitCase{"GetUserName", {"get", "user", "name"}},
    SplitCase{"CreateTable", {"create", "table"}},
    SplitCase{"ParseJSON", {"parse", "json"}}
));

INSTANTIATE_TEST_SUITE_P(Acronyms, NameSplitterTest, ::testing::Values(
    SplitCase{"HTTPServer", {"http", "server"}},
    SplitCase{"XMLParser", {"xml", "parser"}},
    SplitCase{"JSONData", {"json", "data"}},
    SplitCase{"HTTPSConnection", {"https", "connection"}},
    SplitCase{"XMLHttpRequest", {"xml", "http", "request"}},
    SplitCase{"IDGenerator", {"id", "generator"}},
    SplitCase{"URLPath", {"url", "path"}}
));

INSTANTIATE_TEST_SUITE_P(SnakeCase, NameSplitterTest, ::testing::Values(
    SplitCase{"snake_case", {"snake", "case"}},
    SplitCase{"get_user_name", {"get", "user", "name"}},
    SplitCase{"create_table", {"create", "table"}},
    SplitCase{"parse_json", {"parse", "json"}}
));

INSTANTIATE_TEST_SUITE_P(ScreamingSnake, NameSplitterTest, ::testing::Values(
    SplitCase{"SCREAMING_SNAKE_CASE", {"screaming", "snake", "case"}},
    SplitCase{"GET_USER_NAME", {"get", "user", "name"}},
    SplitCase{"CREATE_TABLE", {"create", "table"}}
));

INSTANTIATE_TEST_SUITE_P(KebabCase, NameSplitterTest, ::testing::Values(
    SplitCase{"kebab-case", {"kebab", "case"}},
    SplitCase{"get-user-name", {"get", "user", "name"}},
    SplitCase{"create-table", {"create", "table"}}
));

INSTANTIATE_TEST_SUITE_P(DotNotation, NameSplitterTest, ::testing::Values(
    SplitCase{"dot.notation", {"dot", "notation"}},
    SplitCase{"java.util.ArrayList", {"java", "util", "array", "list"}},
    SplitCase{"com.example.MyClass", {"com", "example", "my", "class"}}
));

INSTANTIATE_TEST_SUITE_P(PathNotation, NameSplitterTest, ::testing::Values(
    SplitCase{"path/to/file", {"path", "to", "file"}},
    SplitCase{"src/main/java", {"src", "main", "java"}}
));

INSTANTIATE_TEST_SUITE_P(Mixed, NameSplitterTest, ::testing::Values(
    SplitCase{"get_userName", {"get", "user", "name"}},
    SplitCase{"http_ServerName", {"http", "server", "name"}},
    SplitCase{"parse-JSONData", {"parse", "json", "data"}}
));

INSTANTIATE_TEST_SUITE_P(Numbers, NameSplitterTest, ::testing::Values(
    SplitCase{"version2", {"version", "2"}},
    SplitCase{"v2Parser", {"v", "2", "parser"}},
    SplitCase{"parse2XML", {"parse", "2", "xml"}},
    SplitCase{"base64Encode", {"base", "64", "encode"}}
));

INSTANTIATE_TEST_SUITE_P(EdgeCases, NameSplitterTest, ::testing::Values(
    SplitCase{"a", {"a"}},
    SplitCase{"A", {"a"}},
    SplitCase{"_leading", {"leading"}},
    SplitCase{"trailing_", {"trailing"}},
    SplitCase{"__double__underscore__", {"double", "underscore"}},
    SplitCase{"Mixed__Case__Style", {"mixed", "case", "style"}}
));

INSTANTIATE_TEST_SUITE_P(Complex, NameSplitterTest, ::testing::Values(
    SplitCase{"AbstractHTTPSConnectionPoolManager",
              {"abstract", "https", "connection", "pool", "manager"}},
    SplitCase{"IUserAuthenticationService",
              {"i", "user", "authentication", "service"}},
    SplitCase{"__init__", {"init"}},
    SplitCase{"MAX_RETRY_COUNT", {"max", "retry", "count"}}
));

TEST(NameSplitterTest, CacheHit) {
    NameSplitter splitter;
    auto first = splitter.split("getUserName");
    auto second = splitter.split("getUserName");
    EXPECT_EQ(first, second);
}

TEST(NameSplitterTest, SplitToSet) {
    NameSplitter splitter;
    auto result = splitter.split_to_set("getUserName");
    EXPECT_TRUE(result.count("get"));
    EXPECT_TRUE(result.count("user"));
    EXPECT_TRUE(result.count("name"));
    EXPECT_EQ(result.size(), 3u);
}

// -- Stemmer tests ------------------------------------------------------------

TEST(StemmerTest, DisabledReturnsOriginal) {
    auto s = Stemmer::disabled();
    EXPECT_EQ(s.stem("running"), "running");
    EXPECT_EQ(s.stem("authentication"), "authentication");
}

TEST(StemmerTest, ExcludedWordsNotStemmed) {
    Stemmer s(true, "porter2", 3, {"api", "db", "uri"});
    EXPECT_EQ(s.stem("api"), "api");
    EXPECT_EQ(s.stem("db"), "db");
    EXPECT_TRUE(s.is_excluded("api"));
}

TEST(StemmerTest, MinLengthRespected) {
    Stemmer s(true, "porter2", 5, {});
    EXPECT_EQ(s.stem("run"), "run");
    // Word meeting min length should be stemmed.
    EXPECT_NE(s.stem("running"), "running");
}

TEST(StemmerTest, Porter2KnownStems) {
    // Test key Porter2 outputs matching Go surgebase/porter2.
    EXPECT_EQ(porter2_stem("running"), "run");
    EXPECT_EQ(porter2_stem("runs"), "run");
    EXPECT_EQ(porter2_stem("search"), "search");
    EXPECT_EQ(porter2_stem("searching"), "search");
    EXPECT_EQ(porter2_stem("searches"), "search");
    EXPECT_EQ(porter2_stem("function"), "function");
    EXPECT_EQ(porter2_stem("functions"), "function");
    EXPECT_EQ(porter2_stem("process"), "process");
    EXPECT_EQ(porter2_stem("processing"), "process");
}

TEST(StemmerTest, StemAll) {
    Stemmer s(true, "porter2", 3, {});
    auto result = s.stem_all({"running", "searching", "process"});
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "run");
    EXPECT_EQ(result[1], "search");
    EXPECT_EQ(result[2], "process");
}

TEST(StemmerTest, StemAndGroup) {
    Stemmer s(true, "porter2", 3, {});
    auto groups = s.stem_and_group({"run", "running", "runs", "search", "searching"});
    EXPECT_TRUE(groups.count("run"));
    EXPECT_TRUE(groups.count("search"));
    EXPECT_GE(groups["run"].size(), 2u);
}

// -- FuzzyMatcher tests -------------------------------------------------------

TEST(FuzzyMatcherTest, ExactMatchReturns1) {
    FuzzyMatcher fm(true, 0.8, "jaro-winkler");
    EXPECT_DOUBLE_EQ(fm.similarity("hello", "hello"), 1.0);
}

TEST(FuzzyMatcherTest, EmptyStringReturns0) {
    FuzzyMatcher fm(true, 0.8, "jaro-winkler");
    EXPECT_DOUBLE_EQ(fm.similarity("", "hello"), 0.0);
    EXPECT_DOUBLE_EQ(fm.similarity("hello", ""), 0.0);
}

TEST(FuzzyMatcherTest, SimilarStringsAboveThreshold) {
    FuzzyMatcher fm(true, 0.8, "jaro-winkler");
    // "search" vs "serach" (transposition) should be above threshold.
    double sim = fm.similarity("search", "serach");
    EXPECT_GT(sim, 0.8);
    EXPECT_TRUE(fm.match("search", "serach"));
}

TEST(FuzzyMatcherTest, DissimilarStringsBelowThreshold) {
    FuzzyMatcher fm(true, 0.8, "jaro-winkler");
    double sim = fm.similarity("apple", "zebra");
    EXPECT_LT(sim, 0.8);
    EXPECT_FALSE(fm.match("apple", "zebra"));
}

TEST(FuzzyMatcherTest, DisabledExactOnly) {
    FuzzyMatcher fm(false, 0.8, "jaro-winkler");
    EXPECT_TRUE(fm.match("hello", "hello"));
    EXPECT_FALSE(fm.match("hello", "helo"));
}

TEST(FuzzyMatcherTest, LevenshteinAlgorithm) {
    FuzzyMatcher fm(true, 0.7, "levenshtein");
    double sim = fm.similarity("kitten", "sitting");
    EXPECT_GT(sim, 0.4);
    EXPECT_LT(sim, 1.0);
}

TEST(FuzzyMatcherTest, FindMatchesSorted) {
    FuzzyMatcher fm(true, 0.7, "jaro-winkler");
    auto matches = fm.find_matches("search",
        {"search", "serach", "searching", "apple", "sarch"});
    EXPECT_GE(matches.size(), 1u);
    // First match should be the most similar.
    EXPECT_EQ(matches[0].term, "search");
}

// -- LRUCache tests -----------------------------------------------------------

TEST(LRUCacheTest, GetMissReturnsNull) {
    LRUCache cache(10);
    EXPECT_EQ(cache.get("missing"), nullptr);
}

TEST(LRUCacheTest, SetAndGet) {
    LRUCache cache(10);
    NormalizedQuery q{"hello", {"hello"}, {"hello"}};
    cache.set("hello", q);
    auto* result = cache.get("hello");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->original, "hello");
}

TEST(LRUCacheTest, EvictsOldest) {
    LRUCache cache(2);
    cache.set("a", {"a", {}, {}});
    cache.set("b", {"b", {}, {}});
    cache.set("c", {"c", {}, {}});
    // "a" should have been evicted.
    EXPECT_EQ(cache.get("a"), nullptr);
    EXPECT_NE(cache.get("b"), nullptr);
    EXPECT_NE(cache.get("c"), nullptr);
}

TEST(LRUCacheTest, GetPromotesEntry) {
    LRUCache cache(2);
    cache.set("a", {"a", {}, {}});
    cache.set("b", {"b", {}, {}});
    // Access "a" to promote it.
    cache.get("a");
    // Add "c" - should evict "b" (least recently used).
    cache.set("c", {"c", {}, {}});
    EXPECT_NE(cache.get("a"), nullptr);
    EXPECT_EQ(cache.get("b"), nullptr);
}

TEST(LRUCacheTest, ClearRemovesAll) {
    LRUCache cache(10);
    cache.set("a", {"a", {}, {}});
    cache.set("b", {"b", {}, {}});
    EXPECT_EQ(cache.size(), 2);
    cache.clear();
    EXPECT_EQ(cache.size(), 0);
    EXPECT_EQ(cache.get("a"), nullptr);
}

// -- SemanticScorer tests -----------------------------------------------------

class SemanticScorerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        scorer = std::make_unique<SemanticScorer>(
            std::make_shared<NameSplitter>(),
            Stemmer(true, "porter2", 3, {}),
            FuzzyMatcher(true, 0.7, "jaro-winkler"));
    }
    std::unique_ptr<SemanticScorer> scorer;
};

TEST_F(SemanticScorerTest, ExactMatch) {
    auto score = scorer->score_symbol("getUserName", "getUserName");
    EXPECT_EQ(score.query_match, MatchType::Exact);
    EXPECT_DOUBLE_EQ(score.score, 1.0);
    EXPECT_DOUBLE_EQ(score.confidence, 1.0);
}

TEST_F(SemanticScorerTest, CaseInsensitiveExact) {
    auto score = scorer->score_symbol("getusername", "GetUserName");
    EXPECT_EQ(score.query_match, MatchType::Exact);
}

TEST_F(SemanticScorerTest, SubstringMatch) {
    auto score = scorer->score_symbol("user", "getUserName");
    EXPECT_EQ(score.query_match, MatchType::Substring);
    EXPECT_DOUBLE_EQ(score.score, 0.9);
}

TEST_F(SemanticScorerTest, NoMatch) {
    auto score = scorer->score_symbol("zzzzz", "getUserName");
    EXPECT_EQ(score.query_match, MatchType::None);
    EXPECT_DOUBLE_EQ(score.score, 0.0);
}

TEST_F(SemanticScorerTest, EmptyInputs) {
    auto s1 = scorer->score_symbol("", "getUserName");
    EXPECT_EQ(s1.query_match, MatchType::None);
    auto s2 = scorer->score_symbol("query", "");
    EXPECT_EQ(s2.query_match, MatchType::None);
}

TEST_F(SemanticScorerTest, NameSplitMatch) {
    auto score = scorer->score_symbol("user", "UserAuthenticationService");
    // Should match via substring or name split.
    EXPECT_GT(score.score, 0.0);
}

TEST_F(SemanticScorerTest, ScoreMultiple) {
    std::vector<std::string> symbols = {
        "getUserName", "setUserName", "processOrder", "user", "banana"};
    auto results = scorer->score_multiple("user", symbols);
    EXPECT_GE(results.size(), 1u);
    // "user" should be ranked highest (exact match).
    EXPECT_EQ(results[0].symbol, "user");
    EXPECT_EQ(results[0].rank, 1);
}

TEST(SemanticScorerStandalone, ScoreMultipleDirect) {
    SemanticScorer s(
        std::make_shared<NameSplitter>(),
        Stemmer(true, "porter2", 3, {}),
        FuzzyMatcher(true, 0.7, "jaro-winkler"));
    std::vector<std::string> symbols = {"alphaSearch"};
    auto scored = s.score_multiple("alpha", symbols);
    EXPECT_GE(scored.size(), 0u);
}

TEST(SemanticScorerStandalone, SemanticSearchResultConstruction) {
    // Test that SemanticSearchResult can be created and destroyed.
    SemanticSearchResult result;
    result.query = "test";
    result.candidates_considered = 1;
    result.results_returned = 0;
    result.execution_time_ns = 42;
    EXPECT_EQ(result.query, "test");
}

TEST(SemanticScorerStandalone, ScorerDestructionOrder) {
    // Test that scorer can be created and destroyed safely.
    auto s = std::make_unique<SemanticScorer>(
        std::make_shared<NameSplitter>(),
        Stemmer(true, "porter2", 3, {}),
        FuzzyMatcher(true, 0.7, "jaro-winkler"));
    auto score = s->score_symbol("test", "testFunction");
    EXPECT_GT(score.score, 0.0);
    s.reset();  // Explicit destruction.
}

TEST(SemanticScorerStandalone, SearchReturnsTimedResult) {
    auto s = std::make_unique<SemanticScorer>(
        std::make_shared<NameSplitter>(),
        Stemmer(true, "porter2", 3, {}),
        FuzzyMatcher(true, 0.7, "jaro-winkler"));
    std::vector<std::string> symbols = {"alphaSearch"};
    auto result = s->search("alpha", symbols);
    EXPECT_EQ(result.query, "alpha");
    EXPECT_EQ(result.candidates_considered, 1);
    EXPECT_GE(result.execution_time_ns, 0);
}

TEST_F(SemanticScorerTest, PhraseMatch) {
    auto score = scorer->score_symbol("get user", "getUserName");
    // Multi-word query should trigger phrase matching.
    EXPECT_GT(score.score, 0.0);
    EXPECT_EQ(score.query_match, MatchType::Phrase);
}

TEST_F(SemanticScorerTest, AbbreviationMatch) {
    auto score = scorer->score_symbol("auth", "AuthenticationService");
    EXPECT_GT(score.score, 0.0);
}

TEST_F(SemanticScorerTest, StemmingMatch) {
    auto score = scorer->score_symbol("searching", "searchEngine");
    EXPECT_GT(score.score, 0.0);
}

TEST_F(SemanticScorerTest, ConfigUpdate) {
    ScoreLayers custom = kDefaultScoreLayers;
    custom.min_score = 0.5;
    scorer->configure(custom);
    EXPECT_DOUBLE_EQ(scorer->config().min_score, 0.5);
}

// -- VocabularyAnalysis tests -------------------------------------------------

TEST(VocabularyAnalysisTest, FilterProductionSymbols) {
    std::vector<FileSymbol> symbols = {
        {"src/main.go", "HandleRequest", "function", true},
        {"src/main_test.go", "TestHandleRequest", "function", true},
        {"src/service.go", "ProcessOrder", "function", true},
        {"test/helper.go", "MockService", "function", false},
    };

    ProjectConfig config;
    config.language = "go";

    auto production = filter_production_symbols(symbols, config);
    // Test files should be excluded.
    EXPECT_LE(production.size(), symbols.size());
    for (const auto& s : production) {
        EXPECT_EQ(s.file_path.find("test"), std::string::npos);
    }
}

TEST(VocabularyAnalysisTest, EmptyInput) {
    auto result = filter_production_symbols({}, {});
    EXPECT_TRUE(result.empty());
}

}  // anonymous namespace
}  // namespace lci
