// Smoketest for nlohmann-json-schema-validator dependency wiring.
//
// Scope (FIX-A): verify the dep is reachable, header compiles, and a trivial
// validator round-trips one passing + one failing payload. Wiring into real
// handlers / validation.cpp is FIX-B, not this task.
//
// Stability target: 10/10 stable per karpathy-principles parity rule.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

using nlohmann::json;
using nlohmann::json_schema::json_validator;

namespace {

// Trivial schema: object requiring an integer field "n" >= 0.
// Kept inline so the test has no FS dependency and is hermetic.
const json kTrivialSchema = R"({
    "$schema": "https://json-schema.org/draft/2019-09/schema",
    "type": "object",
    "properties": {
        "n": { "type": "integer", "minimum": 0 }
    },
    "required": ["n"],
    "additionalProperties": false
})"_json;

}  // namespace

TEST(JsonSchemaSmoketest, ConstructValidator) {
    json_validator validator;
    ASSERT_NO_THROW(validator.set_root_schema(kTrivialSchema));
}

TEST(JsonSchemaSmoketest, ValidatesPassingPayload) {
    json_validator validator;
    validator.set_root_schema(kTrivialSchema);

    const json passing = {{"n", 42}};
    EXPECT_NO_THROW(validator.validate(passing));
}

TEST(JsonSchemaSmoketest, RejectsFailingPayload) {
    json_validator validator;
    validator.set_root_schema(kTrivialSchema);

    // n is negative → violates minimum:0.
    const json failing = {{"n", -1}};
    EXPECT_THROW(validator.validate(failing), std::exception);
}

TEST(JsonSchemaSmoketest, RejectsMissingRequired) {
    json_validator validator;
    validator.set_root_schema(kTrivialSchema);

    const json failing = json::object();  // missing required "n"
    EXPECT_THROW(validator.validate(failing), std::exception);
}
