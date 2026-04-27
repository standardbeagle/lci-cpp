#include <gtest/gtest.h>

#include <lci/mcp/formatter_compact.h>
#include <lci/mcp/pagination.h>
#include <lci/mcp/validation.h>

namespace lci {
namespace mcp {
namespace {

// =============================================================================
// Validation tests
// =============================================================================

TEST(ValidationErrorCode, StringRepresentation) {
    EXPECT_EQ(error_code_string(ValidationErrorCode::kRequired), "REQUIRED");
    EXPECT_EQ(error_code_string(ValidationErrorCode::kInvalid), "INVALID");
    EXPECT_EQ(error_code_string(ValidationErrorCode::kTooLong), "TOO_LONG");
    EXPECT_EQ(error_code_string(ValidationErrorCode::kTooShort), "TOO_SHORT");
    EXPECT_EQ(error_code_string(ValidationErrorCode::kOutOfRange),
              "OUT_OF_RANGE");
    EXPECT_EQ(error_code_string(ValidationErrorCode::kInvalidFormat),
              "INVALID_FORMAT");
    EXPECT_EQ(error_code_string(ValidationErrorCode::kInvalidEnum),
              "INVALID_ENUM");
    EXPECT_EQ(error_code_string(ValidationErrorCode::kConflict), "CONFLICT");
}

TEST(ValidationError, ToStringFormat) {
    ValidationError err{"name", "cannot be empty", nullptr,
                        ValidationErrorCode::kRequired, nullptr};
    EXPECT_EQ(err.to_string(),
              "validation error for field 'name': cannot be empty");
}

// -- RequiredRule -------------------------------------------------------------

TEST(RequiredRule, RejectsNull) {
    auto rule = required_rule();
    auto err = rule.validate(nullptr);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, ValidationErrorCode::kRequired);
    EXPECT_EQ(err->message, "field is required");
}

TEST(RequiredRule, RejectsEmptyString) {
    auto rule = required_rule();
    auto err = rule.validate("  ");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->message, "field cannot be empty");
}

TEST(RequiredRule, RejectsEmptyArray) {
    auto rule = required_rule();
    auto err = rule.validate(nlohmann::json::array());
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->message, "field cannot be empty array");
}

TEST(RequiredRule, AcceptsNonEmpty) {
    auto rule = required_rule();
    EXPECT_FALSE(rule.validate("hello").has_value());
    EXPECT_FALSE(rule.validate(42).has_value());
    EXPECT_FALSE(
        rule.validate(nlohmann::json::array({"a"})).has_value());
}

// -- MinLengthRule ------------------------------------------------------------

TEST(MinLengthRule, RejectsTooShort) {
    auto rule = min_length_rule(3);
    auto err = rule.validate("ab");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, ValidationErrorCode::kTooShort);
}

TEST(MinLengthRule, AcceptsSufficientLength) {
    auto rule = min_length_rule(3);
    EXPECT_FALSE(rule.validate("abc").has_value());
    EXPECT_FALSE(rule.validate("abcd").has_value());
}

TEST(MinLengthRule, SkipsNull) {
    auto rule = min_length_rule(3);
    EXPECT_FALSE(rule.validate(nullptr).has_value());
}

TEST(MinLengthRule, ChecksArrayLength) {
    auto rule = min_length_rule(2);
    auto err = rule.validate(nlohmann::json::array({"a"}));
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, ValidationErrorCode::kTooShort);

    EXPECT_FALSE(
        rule.validate(nlohmann::json::array({"a", "b"})).has_value());
}

// -- MaxLengthRule ------------------------------------------------------------

TEST(MaxLengthRule, RejectsTooLong) {
    auto rule = max_length_rule(5);
    auto err = rule.validate("abcdef");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, ValidationErrorCode::kTooLong);
}

TEST(MaxLengthRule, AcceptsWithinLimit) {
    auto rule = max_length_rule(5);
    EXPECT_FALSE(rule.validate("abc").has_value());
    EXPECT_FALSE(rule.validate("abcde").has_value());
}

// -- RangeRule ----------------------------------------------------------------

TEST(RangeRule, RejectsOutOfRange) {
    auto rule = range_rule(1, 10);

    auto low = rule.validate(0);
    ASSERT_TRUE(low.has_value());
    EXPECT_EQ(low->code, ValidationErrorCode::kOutOfRange);

    auto high = rule.validate(11);
    ASSERT_TRUE(high.has_value());
}

TEST(RangeRule, AcceptsInRange) {
    auto rule = range_rule(1, 10);
    EXPECT_FALSE(rule.validate(1).has_value());
    EXPECT_FALSE(rule.validate(5).has_value());
    EXPECT_FALSE(rule.validate(10).has_value());
}

TEST(RangeRule, RejectsNonNumeric) {
    auto rule = range_rule(1, 10);
    auto err = rule.validate("abc");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, ValidationErrorCode::kInvalidFormat);
}

TEST(RangeRule, SkipsNull) {
    auto rule = range_rule(1, 10);
    EXPECT_FALSE(rule.validate(nullptr).has_value());
}

// -- EnumRule -----------------------------------------------------------------

TEST(EnumRule, RejectsInvalidValue) {
    auto rule = enum_rule({"a", "b", "c"});
    auto err = rule.validate("d");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, ValidationErrorCode::kInvalidEnum);
    EXPECT_NE(err->message.find("a, b, c"), std::string::npos);
}

TEST(EnumRule, AcceptsValidValue) {
    auto rule = enum_rule({"a", "b", "c"});
    EXPECT_FALSE(rule.validate("b").has_value());
}

TEST(EnumRule, SkipsNull) {
    auto rule = enum_rule({"a"});
    EXPECT_FALSE(rule.validate(nullptr).has_value());
}

TEST(EnumRule, RejectsNonString) {
    auto rule = enum_rule({"a"});
    auto err = rule.validate(42);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, ValidationErrorCode::kInvalidFormat);
}

// -- RegexRule ----------------------------------------------------------------

TEST(RegexRule, RejectsNonMatch) {
    auto rule = regex_rule("^[a-z]+$", "must be lowercase");
    auto err = rule.validate("ABC");
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->message, "must be lowercase");
}

TEST(RegexRule, AcceptsMatch) {
    auto rule = regex_rule("^[a-z]+$");
    EXPECT_FALSE(rule.validate("abc").has_value());
}

// -- RequestValidator ---------------------------------------------------------

TEST(RequestValidator, ValidatesMultipleFields) {
    RequestValidator v;
    v.add_rule("name", required_rule());
    v.add_rule("age", range_rule(0, 150));

    nlohmann::json params = {{"name", ""}, {"age", 200}};
    auto result = v.validate(params);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errors.size(), 2u);
}

TEST(RequestValidator, PassesWhenValid) {
    RequestValidator v;
    v.add_rule("name", required_rule());
    v.add_rule("age", range_rule(0, 150));

    nlohmann::json params = {{"name", "Alice"}, {"age", 30}};
    auto result = v.validate(params);
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.errors.empty());
}

TEST(RequestValidator, RejectsNonObject) {
    RequestValidator v;
    auto result = v.validate("not an object");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errors[0].code, ValidationErrorCode::kInvalid);
}

// -- Search validator ---------------------------------------------------------

TEST(SearchValidator, RejectsNegativeMax) {
    auto v = create_search_validator();
    nlohmann::json params = {{"pattern", "test"}, {"max", -1}};
    auto result = v.validate(params);
    EXPECT_FALSE(result.valid);
}

TEST(SearchValidator, AcceptsValidSearch) {
    auto v = create_search_validator();
    nlohmann::json params = {{"pattern", "test"}, {"max", 10}};
    auto result = v.validate(params);
    EXPECT_TRUE(result.valid);
}

// -- Symbol validator ---------------------------------------------------------

TEST(SymbolValidator, RejectsEmptySymbol) {
    auto v = create_symbol_validator();
    nlohmann::json params = {{"symbol", ""}};
    auto result = v.validate(params);
    EXPECT_FALSE(result.valid);
}

TEST(SymbolValidator, AcceptsValidSymbol) {
    auto v = create_symbol_validator();
    nlohmann::json params = {{"symbol", "MyClass"}};
    auto result = v.validate(params);
    EXPECT_TRUE(result.valid);
}

// -- Tree validator -----------------------------------------------------------

TEST(TreeValidator, RejectsEmptyFunction) {
    auto v = create_tree_validator();
    nlohmann::json params = {{"function", ""}};
    auto result = v.validate(params);
    EXPECT_FALSE(result.valid);
}

TEST(TreeValidator, RejectsDepthOutOfRange) {
    auto v = create_tree_validator();
    nlohmann::json params = {{"function", "main"}, {"max_depth", 100}};
    auto result = v.validate(params);
    EXPECT_FALSE(result.valid);
}

TEST(TreeValidator, AcceptsValid) {
    auto v = create_tree_validator();
    nlohmann::json params = {{"function", "main"}, {"max_depth", 5}};
    auto result = v.validate(params);
    EXPECT_TRUE(result.valid);
}

// -- Business logic -----------------------------------------------------------

TEST(SearchBusinessLogic, RejectsNoPattern) {
    nlohmann::json params = {{"max", 10}};
    auto err = validate_search_business_logic(params);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->field, "pattern");
}

TEST(SearchBusinessLogic, AcceptsPattern) {
    nlohmann::json params = {{"pattern", "test"}};
    EXPECT_FALSE(validate_search_business_logic(params).has_value());
}

TEST(SearchBusinessLogic, AcceptsPatterns) {
    nlohmann::json params = {{"patterns", "a|b"}};
    EXPECT_FALSE(validate_search_business_logic(params).has_value());
}

TEST(ObjectContextBusinessLogic, RejectsNoIdOrName) {
    nlohmann::json params = {};
    auto err = validate_object_context_business_logic(params);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, ValidationErrorCode::kRequired);
}

TEST(ObjectContextBusinessLogic, RejectsBothIdAndName) {
    nlohmann::json params = {{"id", "AB"}, {"name", "Foo"}};
    auto err = validate_object_context_business_logic(params);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, ValidationErrorCode::kConflict);
}

TEST(ObjectContextBusinessLogic, AcceptsIdOnly) {
    nlohmann::json params = {{"id", "AB"}};
    EXPECT_FALSE(
        validate_object_context_business_logic(params).has_value());
}

TEST(ObjectContextBusinessLogic, AcceptsNameOnly) {
    nlohmann::json params = {{"name", "Foo"}};
    EXPECT_FALSE(
        validate_object_context_business_logic(params).has_value());
}

// -- Object ID validation -----------------------------------------------------

TEST(IsValidObjectId, ValidIds) {
    EXPECT_TRUE(is_valid_object_id("AB"));
    EXPECT_TRUE(is_valid_object_id("a1_Z"));
    EXPECT_TRUE(is_valid_object_id("_"));
}

TEST(IsValidObjectId, InvalidIds) {
    EXPECT_FALSE(is_valid_object_id(""));
    EXPECT_FALSE(is_valid_object_id("a-b"));
    EXPECT_FALSE(is_valid_object_id("a.b"));
    EXPECT_FALSE(is_valid_object_id("a b"));
}

// -- Structured error responses -----------------------------------------------

TEST(ValidationErrorResponse, SingleError) {
    ValidationError err{"name", "cannot be empty", nullptr,
                        ValidationErrorCode::kRequired, nullptr};
    auto resp = create_validation_error_response("search", err);
    EXPECT_FALSE(resp["success"].get<bool>());
    EXPECT_EQ(resp["error"]["type"], "validation_error");
    EXPECT_EQ(resp["error"]["tool"], "search");
    EXPECT_EQ(resp["error"]["code"], "REQUIRED");
    EXPECT_EQ(resp["error"]["field"], "name");
}

TEST(ValidationErrorResponse, MultipleErrors) {
    std::vector<ValidationError> errs = {
        {"name", "required", nullptr, ValidationErrorCode::kRequired, nullptr},
        {"age", "out of range", 200, ValidationErrorCode::kOutOfRange, nullptr},
    };
    auto resp = create_multi_validation_error_response("search", errs);
    EXPECT_FALSE(resp["success"].get<bool>());
    EXPECT_EQ(resp["error"]["type"], "multiple_validation_errors");
    EXPECT_EQ(resp["error"]["count"], 2);
    EXPECT_EQ(resp["validation_errors"].size(), 2u);
}

TEST(ValidationSummary, Passed) {
    ValidationResult r;
    EXPECT_EQ(validation_summary(r), "Validation passed");
}

TEST(ValidationSummary, SingleError) {
    ValidationResult r;
    r.add_error({"name", "required", nullptr, ValidationErrorCode::kRequired, nullptr});
    auto s = validation_summary(r);
    EXPECT_NE(s.find("name"), std::string::npos);
    EXPECT_NE(s.find("REQUIRED"), std::string::npos);
}

TEST(ValidationSummary, MultipleErrors) {
    ValidationResult r;
    r.add_error({"a", "err1", nullptr, ValidationErrorCode::kRequired, nullptr});
    r.add_error({"b", "err2", nullptr, ValidationErrorCode::kInvalid, nullptr});
    auto s = validation_summary(r);
    EXPECT_NE(s.find("2 errors"), std::string::npos);
}

// =============================================================================
// Pagination tests
// =============================================================================

TEST(TokenEstimator, EstimatesNonZero) {
    TokenEstimator e;
    nlohmann::json val = {{"file", "main.go"}, {"line", 42}};
    int tokens = e.estimate_tokens(val);
    EXPECT_GT(tokens, 0);
}

TEST(PaginationConfig, DefaultValues) {
    auto cfg = default_pagination_config();
    EXPECT_EQ(cfg.default_max_tokens, 20000);
    EXPECT_EQ(cfg.min_page_size, 5);
    EXPECT_EQ(cfg.max_page_size, 1000);
    EXPECT_DOUBLE_EQ(cfg.token_safety_margin, 0.9);
    EXPECT_TRUE(cfg.smart_limit_enabled);
}

TEST(PaginationConstants, ExpectedValues) {
    EXPECT_EQ(kPaginationDefaultContextLines, 3);
    EXPECT_EQ(kPaginationBaseTokens, 50);
    EXPECT_EQ(kPaginationMetadataTokens, 100);
    EXPECT_GT(kPaginationMetadataTokens, kPaginationBaseTokens);
}

TEST(AdaptivePaginator, CalculatePageSizePositive) {
    AdaptivePaginator p;
    nlohmann::json sample = {{"file", "a.go"}, {"line", 1}};
    int size = p.calculate_optimal_page_size(8000, sample, "");
    EXPECT_GT(size, 0);
    EXPECT_LE(size, 1000);
}

TEST(AdaptivePaginator, EmptyResults) {
    AdaptivePaginator p;
    auto result = p.apply_pagination(nlohmann::json::array(), 0, 0, "q",
                                     1.0, "");
    EXPECT_EQ(result.count, 0);
    EXPECT_FALSE(result.has_more);
    EXPECT_EQ(result.query, "q");
}

TEST(AdaptivePaginator, PaginatesResults) {
    AdaptivePaginator p;
    nlohmann::json results = nlohmann::json::array();
    for (int i = 0; i < 50; ++i) {
        results.push_back({{"file", "f" + std::to_string(i) + ".go"},
                           {"line", i},
                           {"match", "content"}});
    }

    auto pr = p.apply_pagination(results, 10, 50, "test", 5.0, "");
    EXPECT_EQ(pr.count, 10);
    EXPECT_TRUE(pr.has_more);
    EXPECT_EQ(pr.total_count, 50);
    EXPECT_EQ(pr.page, 0);
}

TEST(AdaptivePaginator, TokenTruncation) {
    AdaptivePaginator p;
    nlohmann::json results = nlohmann::json::array();
    std::string big_content(2000, 'x');
    for (int i = 0; i < 100; ++i) {
        results.push_back({{"file", "f.go"}, {"line", i},
                           {"match", big_content}});
    }

    auto pr = p.apply_pagination(results, 100, 100, "q", 1.0, "");
    EXPECT_LT(pr.count, 100);
    EXPECT_TRUE(pr.has_more);
    EXPECT_GE(pr.count, kMinGuaranteedResults);
}

TEST(AdaptivePaginator, GroupByFile) {
    AdaptivePaginator p;
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"file", "a.go"}, {"line", 1}});
    results.push_back({{"file", "a.go"}, {"line", 2}});
    results.push_back({{"file", "b.go"}, {"line", 1}});

    auto grouped = p.group_results(results, "file");
    EXPECT_EQ(grouped["a.go"]["count"], 2);
    EXPECT_EQ(grouped["b.go"]["count"], 1);
}

TEST(AdaptivePaginator, GroupByDirectory) {
    AdaptivePaginator p;
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"file", "src/a.go"}, {"line", 1}});
    results.push_back({{"file", "src/b.go"}, {"line", 1}});
    results.push_back({{"file", "pkg/c.go"}, {"line", 1}});

    auto grouped = p.group_results(results, "directory");
    EXPECT_EQ(grouped["src"]["count"], 2);
    EXPECT_EQ(grouped["pkg"]["count"], 1);
}

TEST(AdaptivePaginator, GenerateSummary) {
    AdaptivePaginator p;
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"file", "a.go"}});
    results.push_back({{"file", "a.go"}});
    results.push_back({{"file", "b.go"}});

    auto summary = p.generate_summary(results);
    EXPECT_EQ(summary["total_matches"], 3);
    EXPECT_EQ(summary["unique_files"], 2);
}

TEST(PaginationResult, ToJson) {
    PaginationResult pr;
    pr.query = "test";
    pr.time_ms = 1.5;
    pr.page = 0;
    pr.page_size = 10;
    pr.count = 5;
    pr.total_count = 50;
    pr.has_more = true;
    pr.next_page = 1;
    pr.results = nlohmann::json::array();

    auto j = pr.to_json();
    EXPECT_EQ(j["query"], "test");
    EXPECT_EQ(j["page"], 0);
    EXPECT_TRUE(j["has_more"].get<bool>());
    EXPECT_EQ(j["next_page"], 1);
    EXPECT_FALSE(j.contains("prev_page"));
}

// =============================================================================
// Compact formatter tests
// =============================================================================

TEST(CompactFormatter, SearchResponseFormat) {
    CompactFormatter f;
    nlohmann::json response = {
        {"total_matches", 2},
        {"showing", 2},
        {"max_results", 50},
        {"results", {{
            {"file", "main.go"},
            {"line", 10},
            {"column", 5},
            {"object_id", "AB"},
            {"score", 95.0},
            {"match", "func main()"},
            {"symbol_type", "function"},
            {"symbol_name", "main"},
            {"is_exported", false},
        }}}
    };

    auto text = f.format_search_response(response);
    EXPECT_NE(text.find("LCF/1.0"), std::string::npos);
    EXPECT_NE(text.find("total=2"), std::string::npos);
    EXPECT_NE(text.find("main.go:10:5"), std::string::npos);
    EXPECT_NE(text.find("o=AB"), std::string::npos);
    EXPECT_NE(text.find("t=function"), std::string::npos);
    EXPECT_NE(text.find("n=main"), std::string::npos);
    EXPECT_EQ(text.find("e=1"), std::string::npos);
}

TEST(CompactFormatter, SearchResponseWithExported) {
    CompactFormatter f;
    nlohmann::json response = {
        {"total_matches", 1},
        {"showing", 1},
        {"max_results", 50},
        {"results", {{
            {"file", "lib.go"},
            {"line", 1},
            {"column", 0},
            {"object_id", "XY"},
            {"score", 80.0},
            {"match", "type Handler struct"},
            {"is_exported", true},
            {"file_match_count", 3},
        }}}
    };

    auto text = f.format_search_response(response);
    EXPECT_NE(text.find("e=1"), std::string::npos);
    EXPECT_NE(text.find("m=3"), std::string::npos);
}

TEST(CompactFormatter, MatchTruncation) {
    CompactFormatter f;
    std::string long_match(200, 'x');
    nlohmann::json response = {
        {"total_matches", 1}, {"showing", 1}, {"max_results", 50},
        {"results", {{
            {"file", "a.go"}, {"line", 1}, {"column", 0},
            {"object_id", "AB"}, {"score", 50.0}, {"match", long_match},
        }}}
    };

    auto text = f.format_search_response(response);
    EXPECT_NE(text.find("..."), std::string::npos);
    EXPECT_EQ(text.find(long_match), std::string::npos);
}

TEST(CompactFormatter, FilesOnlyFormat) {
    CompactFormatter f;
    nlohmann::json response = {
        {"total_matches", 5},
        {"unique_files", 2},
        {"files", {"main.go", "lib.go"}},
    };

    auto text = f.format_files_only_response(response);
    EXPECT_NE(text.find("LCF/1.0 mode=files"), std::string::npos);
    EXPECT_NE(text.find("total=5 files=2"), std::string::npos);
    EXPECT_NE(text.find("main.go"), std::string::npos);
    EXPECT_NE(text.find("lib.go"), std::string::npos);
}

TEST(CompactFormatter, CountOnlyFormat) {
    CompactFormatter f;
    nlohmann::json response = {{"total_matches", 42}, {"unique_files", 7}};

    auto text = f.format_count_only_response(response);
    EXPECT_EQ(text, "LCF/1.0 mode=count\ntotal=42 files=7");
}

TEST(CompactFormatter, ContextResponseFormat) {
    CompactFormatter f;
    nlohmann::json response = {
        {"count", 1},
        {"contexts", {{
            {"file_path", "main.go"},
            {"line", 10},
            {"object_id", "AB"},
            {"symbol_type", "function"},
            {"symbol_name", "main"},
            {"is_exported", true},
            {"signature", "func main()"},
        }}}
    };

    auto text = f.format_context_response(response);
    EXPECT_NE(text.find("LCF/1.0"), std::string::npos);
    EXPECT_NE(text.find("c=1"), std::string::npos);
    EXPECT_NE(text.find("main.go:10"), std::string::npos);
    EXPECT_NE(text.find("o=AB"), std::string::npos);
    EXPECT_NE(text.find("t=function"), std::string::npos);
    EXPECT_NE(text.find("n=main"), std::string::npos);
    EXPECT_NE(text.find("e=1"), std::string::npos);
    EXPECT_NE(text.find("s=func main()"), std::string::npos);
}

TEST(CompactFormatter, ContextWithDefinition) {
    CompactFormatter f;
    nlohmann::json response = {
        {"count", 1},
        {"contexts", {{
            {"file_path", "a.go"},
            {"line", 1},
            {"object_id", "CD"},
            {"symbol_type", "variable"},
            {"symbol_name", "x"},
            {"definition", "var x = 1"},
        }}}
    };

    auto text = f.format_context_response(response);
    EXPECT_NE(text.find("d=var x = 1"), std::string::npos);
}

TEST(CompactFormatter, ContextWithIncludedContext) {
    CompactFormatter f;
    f.include_context = true;
    nlohmann::json response = {
        {"count", 1},
        {"contexts", {{
            {"file_path", "a.go"},
            {"line", 1},
            {"object_id", "EF"},
            {"symbol_type", "function"},
            {"context", {"line1", "line2"}},
        }}}
    };

    auto text = f.format_context_response(response);
    EXPECT_NE(text.find("> line1"), std::string::npos);
    EXPECT_NE(text.find("> line2"), std::string::npos);
}

TEST(CompactFormatter, MetadataWithBreadcrumbs) {
    CompactFormatter f;
    f.include_metadata = true;
    f.include_breadcrumbs = true;

    nlohmann::json response = {
        {"total_matches", 1}, {"showing", 1}, {"max_results", 50},
        {"results", {{
            {"file", "a.go"}, {"line", 1}, {"column", 0},
            {"object_id", "AB"}, {"score", 50.0}, {"match", "test"},
            {"breadcrumbs", {{{"name", "pkg"}}, {{"name", "foo"}}}},
            {"safety", {{"edit_safety", "safe"}, {"complexity_score", 1.5}}},
            {"references", {{"incoming_count", 3}, {"outgoing_count", 2}}},
            {"dependencies", {"dep1", "dep2"}},
        }}}
    };

    auto text = f.format_search_response(response);
    EXPECT_NE(text.find("@bc=pkg.foo"), std::string::npos);
    EXPECT_NE(text.find("safety=safe"), std::string::npos);
    EXPECT_NE(text.find("complexity=1.50"), std::string::npos);
    EXPECT_NE(text.find("refs=3,2"), std::string::npos);
    EXPECT_NE(text.find("deps=2"), std::string::npos);
}

TEST(CompactFormatter, SearchContextLines) {
    CompactFormatter f;
    f.include_context = true;

    nlohmann::json response = {
        {"total_matches", 1}, {"showing", 1}, {"max_results", 50},
        {"results", {{
            {"file", "a.go"}, {"line", 1}, {"column", 0},
            {"object_id", "AB"}, {"score", 50.0}, {"match", "test"},
            {"context_lines", {"before line", "after line"}},
        }}}
    };

    auto text = f.format_search_response(response);
    EXPECT_NE(text.find("> before line"), std::string::npos);
    EXPECT_NE(text.find("> after line"), std::string::npos);
}

}  // namespace
}  // namespace mcp
}  // namespace lci
