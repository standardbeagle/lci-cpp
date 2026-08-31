// PathClassifier unit tests — the file attribute tagging authority.
//
// RED material comes from the verified D1 defects
// (benchmarks/repo-qa/ANALYSIS-insight-verification.md):
//   - chi: _examples/*/main.go analyzed as production entry points
//   - guzzle: tests/**, *Test.php analyzed as production
//   - pocketbase: ui/public/libs/uplot/uplot.iife.js (vendored minified)
//     supplying entry points / cycles / high-complexity stats

#include <lci/path_classifier.h>

#include <gtest/gtest.h>

#include <string>

namespace lci {
namespace {

// Attributes are named, not enumerated: assert on the name the registry
// resolves, so a test still says what it means when a project adds or
// renames attributes.
std::string attr_of(const PathClassifier& c, std::string_view path) {
    return std::string(c.registry().name(c.classify(path)));
}
std::string attr_of(const PathClassifier& c, std::string_view path,
                    std::string_view content) {
    return std::string(c.registry().name(c.classify(path, content)));
}

/// A classifier over the shipped ruleset plus one project's attributes block.
PathAttrRegistry registry_with(std::string_view attributes_kdl) {
    std::vector<AttrDef> defs;
    std::vector<PathAttrRule> rules;
    std::string error;
    EXPECT_TRUE(parse_attributes_block(attributes_kdl, defs, rules, error))
        << error;
    return PathAttrRegistry::with_config(defs, rules, error);
}

// -- Built-in defaults: Go ----------------------------------------------------

TEST(PathClassifierTest, GoTestFile) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "middleware/logger_test.go"), "test");
    EXPECT_EQ(attr_of(c, "mux_test.go"), "test");
}

TEST(PathClassifierTest, GoUnderscoreDirIsExample) {
    PathClassifier c;
    // chi D1: 12/12 entry points were _examples/*/main.go.
    EXPECT_EQ(attr_of(c, "_examples/custom-handler/main.go"), "example");
    EXPECT_EQ(attr_of(c, "_examples/rest/main.go"), "example");
}

TEST(PathClassifierTest, PythonUnderscorePackageIsProduction) {
    PathClassifier c;
    // fastapi audit: `_*` swallowed fastapi/_compat, so the real front door
    // (param_functions re-exports) never entered analysis. Underscore
    // packages are production internals in Python.
    EXPECT_EQ(attr_of(c, "fastapi/_compat/__init__.py"), "production");
    EXPECT_EQ(attr_of(c, "pkg/_internal/util.py"), "production");
    // The Go example spellings still classify.
    EXPECT_EQ(attr_of(c, "_example/main.go"), "example");
}

TEST(PathClassifierTest, GoTestdataDir) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "pkg/parser/testdata/input.go"), "test");
}

// -- Built-in defaults: PHP / JS / TS -----------------------------------------

TEST(PathClassifierTest, PhpTestsDirAndSuffix) {
    PathClassifier c;
    // guzzle D1: tests/Handler classified as API Layer; 4/12 entry points
    // were PHPUnit test methods.
    EXPECT_EQ(attr_of(c, "tests/Handler/CurlFactoryTest.php"), "test");
    EXPECT_EQ(attr_of(c, "tests/ClientTest.php"), "test");
    EXPECT_EQ(attr_of(c, "src/HandlerTest.php"), "test");
}

TEST(PathClassifierTest, JsTsTestPatterns) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "src/__tests__/router.js"), "test");
    EXPECT_EQ(attr_of(c, "src/router.test.ts"), "test");
    EXPECT_EQ(attr_of(c, "src/router.spec.tsx"), "test");
    EXPECT_EQ(attr_of(c, "tests/e2e/login.py"), "test");
    EXPECT_EQ(attr_of(c, "scripts/test_forge.py"), "test");
    EXPECT_EQ(attr_of(c, "tests/conftest.py"), "test");
}

// -- Vendored ------------------------------------------------------------------

// okhttp audit (2026-08-30): the test classifier was filename-suffix-based, so
// Kotlin/Gradle source-set LAYOUTS leaked whole test trees into the shipping
// view — 14 non-*Test.kt files under okhttp/src/jvmTest/ (AutobahnTester,
// MockHttp2Peer, FakeRoutePlanner…), okhttp-testing-support/src/main, and the
// module-tests / maven-tests Gradle modules all ranked as api/binaries entry
// points while their modules were simultaneously typed Test.
TEST(PathClassifierTest, JvmSourceSetTestLayouts) {
    PathClassifier c;
    // Kotlin multiplatform <target>Test source sets.
    EXPECT_EQ(attr_of(c, "okhttp/src/jvmTest/kotlin/okhttp3/AutobahnTester.kt"),
              "test");
    EXPECT_EQ(attr_of(c, "lib/src/commonTest/kotlin/Foo.kt"), "test");
    EXPECT_EQ(attr_of(c, "app/src/androidTest/kotlin/Probe.kt"), "test");
    // Gradle testFixtures source set.
    EXPECT_EQ(attr_of(c, "core/src/testFixtures/java/Fixture.java"), "test");
    // Test-support / *-tests module directories.
    EXPECT_EQ(attr_of(
                  c, "okhttp-testing-support/src/main/kotlin/TestUtilJvm.kt"),
              "test");
    EXPECT_EQ(attr_of(c, "module-tests/src/main/java/Main.java"), "test");
    EXPECT_EQ(attr_of(c, "maven-tests/pom-checker/src/main/java/C.java"),
              "test");
    // Guards: production source sets and names merely containing "test"
    // stay production.
    EXPECT_EQ(attr_of(c, "okhttp/src/commonJvmAndroid/kotlin/Headers.kt"),
              "production");
    EXPECT_EQ(attr_of(c, "src/main/kotlin/Contest.kt"), "production");
    EXPECT_EQ(attr_of(c, "protest/src/main/kotlin/March.kt"), "production");
}

TEST(PathClassifierTest, VendorDirs) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "vendor/guzzlehttp/psr7/src/Uri.php"), "vendored");
    EXPECT_EQ(attr_of(c, "node_modules/react/index.js"), "vendored");
    EXPECT_EQ(attr_of(c, "third_party/zlib/inflate.c"), "vendored");
}

TEST(PathClassifierTest, MinifiedBasename) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "ui/public/js/app.min.js"), "vendored");
    EXPECT_EQ(attr_of(c, "assets/style.min.css"), "vendored");
}

TEST(PathClassifierTest, MinifiedContentHeuristic) {
    PathClassifier c;
    // pocketbase D1: ui/public/libs/uplot/uplot.iife.js — one enormous line.
    std::string minified(50000, 'x');
    EXPECT_EQ(attr_of(c, "ui/public/libs/uplot/uplot.iife.js", minified), "vendored");
    // Normal multi-line code stays production.
    std::string code;
    for (int i = 0; i < 500; ++i) code += "const x = 1;\n";
    EXPECT_EQ(attr_of(c, "ui/src/app.js", code), "production");
}

// -- Generated -----------------------------------------------------------------

TEST(PathClassifierTest, GeneratedPatterns) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "api/service.pb.go"), "generated");
    EXPECT_EQ(attr_of(c, "proto/service_pb2.py"), "generated");
    EXPECT_EQ(attr_of(c, "src/schema_generated.ts"), "generated");
    EXPECT_EQ(attr_of(c, "types/global.d.ts"), "generated");
    EXPECT_EQ(attr_of(c, "pkg/apis/zz_generated.deepcopy.go"), "generated");
}

TEST(PathClassifierTest, GeneratedHeaderContent) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "internal/types/types.go",
                      "// Code generated by jsvm. DO NOT EDIT.\n"
                      "package types\n"),
              "generated");
    EXPECT_EQ(attr_of(c, "internal/types/types.go", "package types\n"),
              "production");
}

// -- Docs ----------------------------------------------------------------------

TEST(PathClassifierTest, DocsDirAndMarkdown) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "docs/guide/install.js"), "docs");
    EXPECT_EQ(attr_of(c, "README.md"), "docs");
}

// -- Production stays production ----------------------------------------------

TEST(PathClassifierTest, ProductionPaths) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "mux.go"), "production");
    EXPECT_EQ(attr_of(c, "middleware/logger.go"), "production");
    EXPECT_EQ(attr_of(c, "src/Client.php"), "production");
    // "testing"-adjacent words must not fuzzy-match.
    EXPECT_EQ(attr_of(c, "src/contest/rank.go"), "production");
    EXPECT_EQ(attr_of(c, "src/attestation/verify.go"), "production");
}

// -- Precedence ----------------------------------------------------------------

TEST(PathClassifierTest, VendoredBeatsTest) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "vendor/pkg/foo_test.go"), "vendored");
    EXPECT_EQ(attr_of(c, "node_modules/lib/__tests__/a.js"), "vendored");
}

// -- Config rules override builtins -------------------------------------------

TEST(PathClassifierTest, ConfigRuleAddsTag) {
    auto reg = registry_with(R"(attributes { test "src/legacy_tests/" })");
    PathClassifier c(reg);
    EXPECT_EQ(attr_of(c, "src/legacy_tests/old.go"), "test");
}

TEST(PathClassifierTest, ConfigProductionOverridesBuiltin) {
    auto reg = registry_with(R"(attributes { production "vendor/mycompany/" })");
    PathClassifier c(reg);
    EXPECT_EQ(attr_of(c, "vendor/mycompany/core.go"), "production");
    EXPECT_EQ(attr_of(c, "vendor/other/core.go"), "vendored");
}

TEST(PathClassifierTest, ConfigBasenameGlob) {
    auto reg = registry_with(R"(attributes { vendored "*.iife.js" })");
    PathClassifier c(reg);
    EXPECT_EQ(attr_of(c, "ui/public/libs/uplot/uplot.iife.js"), "vendored");
}

TEST(PathClassifierTest, ConfigProductionBeatsContentHeuristic) {
    auto reg = registry_with(R"(attributes { production "big/blob.js" })");
    PathClassifier c(reg);
    std::string minified(50000, 'x');
    EXPECT_EQ(attr_of(c, "big/blob.js", minified), "production");
}

// -- Benchmarks ----------------------------------------------------------------

// Benchmark harnesses are not shipping code: they exercise the product, carry
// their own tooling, and their error handling is deliberately lax (a bench
// script swallowing an exception is not a defect in the product). Analysis
// sections default to shipping code, so these must classify out.
TEST(PathClassifierTest, BenchmarkDirs) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "benchmarks/repo-qa/scripts/bench.py"), "benchmark");
    EXPECT_EQ(attr_of(c, "bench/latency_bench.cpp"), "benchmark");
    EXPECT_EQ(attr_of(c, "src/benchmark/runner.go"), "benchmark");
}

// Go's own convention: *_bench.go / *_benchmark.go sit beside production code.
TEST(PathClassifierTest, BenchmarkBasenames) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "src/index/index_bench.cpp"), "benchmark");
    EXPECT_EQ(attr_of(c, "pkg/store/store_benchmark.go"), "benchmark");
}

// A real package named "benchmarking" is production, the same way "testing"
// is not treated as a test dir.
TEST(PathClassifierTest, BenchmarkDoesNotSwallowSimilarNames) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "src/benchmarking/service.go"), "production");
}

// Precedence: a test file inside a benchmark tree is a test (Test outranks
// Benchmark), and a vendored dep inside one is vendored.
TEST(PathClassifierTest, BenchmarkLosesToStrongerAttributes) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "benchmarks/harness/runner_test.go"), "test");
    EXPECT_EQ(attr_of(c, "benchmarks/vendor/dep/lib.go"), "vendored");
}

// -- Ambiguous trees found on real corpora -------------------------------------

// zod's `packages/resolution/test-resolution.ts` is a build-verification
// script that owned zod's worst error-handling module: JS/TS has no
// leading-prefix test convention the way Python does, so nothing claimed it.
TEST(PathClassifierTest, ScriptStyleTestEntryPointsAreTests) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "packages/resolution/test-resolution.ts"), "test");
    EXPECT_EQ(attr_of(c, "scripts/test-build.js"), "test");
    EXPECT_EQ(attr_of(c, "tools/test_helpers.ts"), "test");
    // Not every file that merely starts with the letters "test".
    EXPECT_EQ(attr_of(c, "src/testimonials.ts"), "production");
    EXPECT_EQ(attr_of(c, "src/testable-api.ts"), "production");
}

// axios's sandbox/server.js owned its worst module. A dev playground is not
// shipped and is conventionally the laxest code in a repo.
TEST(PathClassifierTest, DevPlaygroundsAreExamples) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "sandbox/server.js"), "example");
    EXPECT_EQ(attr_of(c, "playground/app/main.ts"), "example");
    EXPECT_EQ(attr_of(c, "src/sandboxing/limits.go"), "production")
        << "a real package named for sandboxing is not a playground";
}

// -- The shipped ruleset -------------------------------------------------------

// The default attributes and rules ship inside the binary as KDL. A typo in
// them is a build defect that would otherwise degrade silently to
// "everything is production", so parse them explicitly here.
TEST(PathClassifierTest, ShippedRulesetParses) {
    const auto& reg = PathAttrRegistry::builtin();
    EXPECT_EQ(reg.name(kFallbackAttr), "production")
        << "the fallback attribute must be id 0";
    for (std::string_view expected :
         {"production", "test", "benchmark", "example", "vendored",
          "generated", "docs"}) {
        PathAttrId id{};
        EXPECT_TRUE(reg.find(expected, id)) << expected;
    }
}

// Capabilities are what "shipping code" means: the analysis gate is the set
// of attributes that activate it, not a flag named after production.
TEST(PathClassifierTest, ShippedCapabilities) {
    const auto& reg = PathAttrRegistry::builtin();
    auto activates = [&](std::string_view attr, Capability cap) {
        PathAttrId id{};
        EXPECT_TRUE(reg.find(attr, id)) << attr;
        return reg.activates(id, cap);
    };
    EXPECT_TRUE(activates("production", Capability::Analysis));
    for (std::string_view attr :
         {"test", "benchmark", "example", "vendored", "generated", "docs"}) {
        EXPECT_FALSE(activates(attr, Capability::Analysis)) << attr;
    }
    // Everything is indexed and searchable by default; nothing but production
    // is a preferred reference-resolution target.
    for (std::string_view attr :
         {"production", "test", "benchmark", "vendored", "docs"}) {
        EXPECT_TRUE(activates(attr, Capability::Index)) << attr;
        EXPECT_TRUE(activates(attr, Capability::Search)) << attr;
    }
    EXPECT_TRUE(activates("production", Capability::Refs));
    EXPECT_FALSE(activates("test", Capability::Refs));
}

// -- Open attribute set --------------------------------------------------------

// A project can name an attribute this binary has never heard of. It gets an
// id, a rank, and capabilities like any other.
TEST(PathClassifierTest, ConfigDeclaresANewAttribute) {
    auto reg = registry_with(R"(
        attributes {
            internal-tooling rank=6 {
                activates "index" "search"
                dir "scripts" "tools"
                glob "*.tmpl"
            }
        }
    )");
    PathClassifier c(reg);
    EXPECT_EQ(attr_of(c, "scripts/release/publish.py"), "internal-tooling");
    EXPECT_EQ(attr_of(c, "src/templates/page.tmpl"), "internal-tooling");
    PathAttrId id{};
    ASSERT_TRUE(reg.find("internal-tooling", id));
    EXPECT_FALSE(reg.activates(id, Capability::Analysis));
    EXPECT_TRUE(reg.activates(id, Capability::Search));
}

// A pattern naming an attribute nobody defined declares it too, so the
// shorthand form does not need a matching block.
TEST(PathClassifierTest, ShorthandForAnUnknownAttributeDeclaresIt) {
    auto reg = registry_with(R"(attributes { fixtures-only "testfixtures/" })");
    PathClassifier c(reg);
    EXPECT_EQ(attr_of(c, "testfixtures/data/a.go"), "fixtures-only");
    PathAttrId id{};
    ASSERT_TRUE(reg.find("fixtures-only", id));
    EXPECT_FALSE(reg.activates(id, Capability::Analysis))
        << "a tagged tree defaults out of analysis";
}

// Redefining a shipped attribute replaces its patterns: a project that writes
// the block means it, and leaving shipped patterns in force invisibly is the
// surprise this avoids.
TEST(PathClassifierTest, ConfigRedefinesAShippedAttribute) {
    auto reg = registry_with(R"(
        attributes {
            benchmark rank=3 {
                activates "index" "search" "analysis"
                dir "perf"
            }
        }
    )");
    PathClassifier c(reg);
    EXPECT_EQ(attr_of(c, "perf/latency.go"), "benchmark");
    EXPECT_EQ(attr_of(c, "benchmarks/repo-qa/bench.py"), "production")
        << "the shipped benchmark dirs were replaced, not merged";
    PathAttrId id{};
    ASSERT_TRUE(reg.find("benchmark", id));
    EXPECT_TRUE(reg.activates(id, Capability::Analysis))
        << "a project can opt its benchmarks back into analysis";
}

// -- Ruleset errors ------------------------------------------------------------

TEST(PathClassifierTest, RejectsUnknownCapability) {
    std::vector<AttrDef> defs;
    std::vector<PathAttrRule> rules;
    std::string error;
    EXPECT_FALSE(parse_attributes_block(
        R"(attributes { weird { activates "teleport" } })", defs, rules,
        error));
    EXPECT_NE(error.find("teleport"), std::string::npos) << error;
}

TEST(PathClassifierTest, RejectsUnknownKeyInsideADefinition) {
    std::vector<AttrDef> defs;
    std::vector<PathAttrRule> rules;
    std::string error;
    EXPECT_FALSE(parse_attributes_block(
        R"(attributes { weird { directory "x" } })", defs, rules, error));
    EXPECT_NE(error.find("directory"), std::string::npos) << error;
}

TEST(PathClassifierTest, RejectsUnknownContentHeuristic) {
    std::vector<AttrDef> defs;
    std::vector<PathAttrRule> rules;
    std::string error;
    EXPECT_FALSE(parse_attributes_block(
        R"(attributes { weird { content "vibes" } })", defs, rules, error));
    EXPECT_NE(error.find("vibes"), std::string::npos) << error;
}

TEST(PathClassifierTest, RejectsAnAttributeWithNeitherPatternsNorBlock) {
    std::vector<AttrDef> defs;
    std::vector<PathAttrRule> rules;
    std::string error;
    EXPECT_FALSE(
        parse_attributes_block(R"(attributes { lonely })", defs, rules, error));
    EXPECT_NE(error.find("lonely"), std::string::npos) << error;
}

// -- Round-5 calibration gaps (redis, Newtonsoft.Json) ------------------------

// redis vendors jemalloc under deps/; its handleOOM owned a med finding.
TEST(PathClassifierTest, DepsDirIsVendored) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "deps/jemalloc/src/jemalloc_cpp.cpp"), "vendored");
    EXPECT_EQ(attr_of(c, "deps/lua/src/lua.c"), "vendored");
}

// C# convention: the test project is a sibling DIRECTORY named *.Tests.
// Newtonsoft.Json.Tests/Documentation/Samples supplied most of the repo's
// findings while the shipped serializer scored clean.
TEST(PathClassifierTest, CsharpTestsDirSuffix) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "Src/Newtonsoft.Json.Tests/Documentation/"
                         "Samples/Serializer/Sample.cs"),
              "test");
    EXPECT_EQ(attr_of(c, "Src/Newtonsoft.Json.Test/A.cs"), "test");
    // The product directory next to it stays production.
    EXPECT_EQ(attr_of(c, "Src/Newtonsoft.Json/JsonSerializer.cs"),
              "production");
}

// Script-style test entry points without the underscore convention:
// redis has modules/vector-sets/test.py and utils/lru/test-lru.rb.
TEST(PathClassifierTest, BareAndDashTestScriptBasenames) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "modules/vector-sets/test.py"), "test");
    EXPECT_EQ(attr_of(c, "utils/lru/test-lru.rb"), "test");
    EXPECT_EQ(attr_of(c, "utils/req-res-validation/test.rb"), "test");
    // "attest.py" / "contest.rb" must not match.
    EXPECT_EQ(attr_of(c, "src/attest.py"), "production");
}

// Dash-spelled bench scripts: redis tools/array-bench.py drove three
// exposure paths in the round-5 report.
TEST(PathClassifierTest, DashBenchScriptBasenames) {
    PathClassifier c;
    EXPECT_EQ(attr_of(c, "tools/array-bench.py"), "benchmark");
    EXPECT_EQ(attr_of(c, "scripts/parse-bench.js"), "benchmark");
    EXPECT_EQ(attr_of(c, "src/workbench.py"), "production");
}

}  // namespace
}  // namespace lci
