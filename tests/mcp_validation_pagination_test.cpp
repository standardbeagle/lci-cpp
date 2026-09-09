#include <gtest/gtest.h>

#include <lci/mcp/validation.h>
#include <lci/pagination.h>

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
// Shared pagination semantics (lci/pagination.h) — the one implementation
// both the MCP list_symbols handler and HTTP /list-symbols must use.
// =============================================================================

TEST(PageWindow, DefaultsWhenAbsent) {
    auto w = normalize_page(nlohmann::json::object());
    EXPECT_EQ(w.max, 50);
    EXPECT_EQ(w.offset, 0);
}

TEST(PageWindow, ExplicitValuesPassThrough) {
    auto w = normalize_page({{"max", 7}, {"offset", 3}});
    EXPECT_EQ(w.max, 7);
    EXPECT_EQ(w.offset, 3);
}

TEST(PageWindow, ZeroAndNegativeMaxMeanDefault) {
    EXPECT_EQ(normalize_page({{"max", 0}}).max, 50);
    EXPECT_EQ(normalize_page({{"max", -5}}).max, 50);
}

TEST(PageWindow, MaxClampsToCap) {
    EXPECT_EQ(normalize_page({{"max", 501}}).max, 500);
    EXPECT_EQ(normalize_page({{"max", 9}}, 20, 8).max, 8);
}

TEST(PageWindow, NegativeOffsetClampsToZero) {
    EXPECT_EQ(normalize_page({{"offset", -4}}).offset, 0);
}

TEST(PageWindow, CustomDefaultMax) {
    EXPECT_EQ(normalize_page(nlohmann::json::object(), 25).max, 25);
    EXPECT_EQ(normalize_page({{"max", 0}}, 25).max, 25);
}

TEST(PageHasMore, ExactBoundaryIsFalse) {
    EXPECT_FALSE(page_has_more(/*total=*/4, /*offset=*/2, /*shown=*/2));
}

TEST(PageHasMore, RemainderIsTrue) {
    EXPECT_TRUE(page_has_more(4, 1, 2));
}

TEST(PageHasMore, EmptyTotalIsFalse) {
    EXPECT_FALSE(page_has_more(0, 0, 0));
}

TEST(PageHasMore, OffsetPastTotalIsFalse) {
    EXPECT_FALSE(page_has_more(4, 10, 0));
}

}  // namespace
}  // namespace mcp
}  // namespace lci
