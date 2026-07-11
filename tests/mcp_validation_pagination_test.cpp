#include <gtest/gtest.h>

#include <lci/mcp/formatter_compact.h>
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
