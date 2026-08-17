// AST-driven error-handling + resource extraction tests.
//
// Drives UnifiedExtractor with a SideEffectAnalyzer sink over real parses
// (Karpathy #5: no mocks — every case goes through the actual grammar) and
// asserts the per-function ErrorHandlingInfo / findings the design doc
// specifies (docs/plans/2026-08-17-error-handling-score-design.md):
//   - phase 1: returns_error / try_finally wiring + Go panic, Ruby
//     raise/rescue, Kotlin throw, Zig try/errdefer grammar branches.
//   - phase 2: swallow detection (empty-catch, catch-and-continue,
//     broad-catch, log-and-swallow, dropped-error, rethrow-no-cause).
//   - phase 3: resource acquire/release pairing + leak findings.

#include <lci/analysis/side_effect_analyzer.h>
#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <gtest/gtest.h>
#include <tree_sitter/api.h>

#include <string>
#include <string_view>

namespace lci {
namespace {

using parser::Language;

// Parses `src`, walks it with the analyzer attached, and returns the record
// for `fname` (nullptr when the function was never recorded).
class SideEffectExtraction : public ::testing::Test {
  protected:
    const SideEffectInfo* analyze(Language lang, std::string_view ext,
                                  std::string_view src,
                                  std::string_view fname) {
        analyzer_ = std::make_unique<SideEffectAnalyzer>("generic");
        auto ts_parser = parser::make_parser(lang);
        if (!ts_parser) return nullptr;
        parser::UniqueTree tree(ts_parser_parse_string(
            ts_parser.get(), nullptr, src.data(),
            static_cast<uint32_t>(src.size())));
        if (!tree) return nullptr;

        parser::UnifiedExtractor ue;
        std::string path = "test" + std::string(ext);
        ue.init(src, 1, ext, path);
        ue.set_side_effect_sink(analyzer_.get());
        ue.extract(tree.get());

        for (const auto& [key, info] : analyzer_->results()) {
            if (info.function_name == fname) return &info;
        }
        return nullptr;
    }

    static int count_findings(const std::vector<EhFinding>& v, EhSignal s) {
        int n = 0;
        for (const auto& f : v) {
            if (f.signal == s) ++n;
        }
        return n;
    }

    std::unique_ptr<SideEffectAnalyzer> analyzer_;
};

// ---------------------------------------------------------------------------
// Phase 1 — wire the dead fields + missing grammar branches
// ---------------------------------------------------------------------------

TEST_F(SideEffectExtraction, GoErrorReturnSignatureSetsReturnsError) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f() error {\n"
                               "\treturn nil\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_handling.returns_error);
}

TEST_F(SideEffectExtraction, GoMultiValueErrorReturnSetsReturnsError) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f() (int, error) {\n"
                               "\treturn 0, nil\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_handling.returns_error);
}

TEST_F(SideEffectExtraction, GoPlainReturnDoesNotSetReturnsError) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f() int { return 1 }\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_FALSE(info->error_handling.returns_error);
}

TEST_F(SideEffectExtraction, GoPanicIsAPreciseThrowSite) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f() {\n"
                               "\tpanic(\"boom\")\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->error_handling.throw_count, 1);
    ASSERT_EQ(info->throw_sites.size(), 1u);
    EXPECT_EQ(info->throw_sites[0].line, 3);
    EXPECT_EQ(info->throw_sites[0].type, "panic");
}

TEST_F(SideEffectExtraction, PythonTryFinallyIsCounted) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f():\n"
                               "    try:\n"
                               "        g()\n"
                               "    finally:\n"
                               "        h()\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->error_handling.try_finally_count, 1);
    EXPECT_TRUE(info->error_handling.exception_safe);
}

TEST_F(SideEffectExtraction, JsTryFinallyIsCounted) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } finally { h(); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->error_handling.try_finally_count, 1);
}

TEST_F(SideEffectExtraction, RubyRaiseIsAPreciseThrowSite) {
    const auto* info = analyze(Language::Ruby, ".rb",
                               "def f\n"
                               "  raise ArgumentError, \"bad\"\n"
                               "end\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_GE(info->error_handling.throw_count, 1);
}

TEST_F(SideEffectExtraction, RubyEnsureCountsAsTryFinally) {
    const auto* info = analyze(Language::Ruby, ".rb",
                               "def f\n"
                               "  g\n"
                               "ensure\n"
                               "  h\n"
                               "end\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->error_handling.try_finally_count, 1);
}

TEST_F(SideEffectExtraction, KotlinThrowIsAPreciseThrowSite) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun f() {\n"
                               "    throw IllegalStateException(\"bad\")\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_GE(info->error_handling.throw_count, 1);
}

TEST_F(SideEffectExtraction, KotlinFinallyCountsAsTryFinally) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun f() {\n"
                               "    try { g() } finally { h() }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->error_handling.try_finally_count, 1);
}

TEST_F(SideEffectExtraction, ZigTrySetsReturnsError) {
    const auto* info = analyze(Language::Zig, ".zig",
                               "fn f() !void {\n"
                               "    try g();\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_handling.returns_error);
}

TEST_F(SideEffectExtraction, ZigErrdeferCountsAsDefer) {
    const auto* info = analyze(Language::Zig, ".zig",
                               "fn f() !void {\n"
                               "    errdefer cleanup();\n"
                               "    try g();\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_GE(info->error_handling.defer_count, 1);
}

// ---------------------------------------------------------------------------
// Phase 2 — swallow detection
// ---------------------------------------------------------------------------

TEST_F(SideEffectExtraction, JsEmptyCatchIsFlagged) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) {}\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->error_handling.catch_count, 1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 1);
}

TEST_F(SideEffectExtraction, JsLogOnlyCatchIsLogAndSwallow) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { console.log(e); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::LogAndSwallow), 1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 0);
}

TEST_F(SideEffectExtraction, JsCatchWithWorkIsCatchAndContinue) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { recover(); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue),
              1);
}

TEST_F(SideEffectExtraction, JsRethrowWithCauseIsClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { throw e; }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, JsRethrowWithoutCauseIsFlagged) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { throw new "
                               "Error(\"wrapped\"); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::RethrowNoCause),
              1);
}

TEST_F(SideEffectExtraction, PythonBareExceptPassIsEmptyAndBroad) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f():\n"
                               "    try:\n"
                               "        g()\n"
                               "    except:\n"
                               "        pass\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 1);
}

TEST_F(SideEffectExtraction, PythonBroadExceptionTypeIsFlagged) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f():\n"
                               "    try:\n"
                               "        g()\n"
                               "    except Exception as e:\n"
                               "        recover(e)\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 1);
}

TEST_F(SideEffectExtraction, PythonNarrowExceptWithRaiseIsClean) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f():\n"
                               "    try:\n"
                               "        g()\n"
                               "    except ValueError:\n"
                               "        raise\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 0);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue),
              0);
}

TEST_F(SideEffectExtraction, JavaEmptyBroadCatchIsFlagged) {
    const auto* info = analyze(Language::Java, ".java",
                               "class C {\n"
                               "  void f() {\n"
                               "    try { g(); } catch (Exception e) {}\n"
                               "  }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 1);
}

TEST_F(SideEffectExtraction, CppCatchAllEmptyIsFlagged) {
    const auto* info = analyze(Language::Cpp, ".cpp",
                               "void f() {\n"
                               "  try { g(); } catch (...) {}\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 1);
}

TEST_F(SideEffectExtraction, RubyEmptyRescueIsFlagged) {
    const auto* info = analyze(Language::Ruby, ".rb",
                               "def f\n"
                               "  g\n"
                               "rescue\n"
                               "end\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 1);
}

TEST_F(SideEffectExtraction, GoBlankAssignedErrIsDroppedError) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(w io.Closer) {\n"
                               "\t_ = w.Close()\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 1);
}

TEST_F(SideEffectExtraction, GoBlankSecondResultIsDroppedError) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f() {\n"
                               "\tv, _ := parse()\n"
                               "\tuse(v)\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 1);
}

// `v, _ := x.(T)` discards a type-assertion ok-bool, not an error.
TEST_F(SideEffectExtraction, GoTypeAssertionBlankIsNotDroppedError) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(ctx context.Context) {\n"
                               "\tval, _ := ctx.Value(key).(*Context)\n"
                               "\tuse(val)\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 0);
}

// A middle blank with the error still captured last is not a drop:
// `host, _, err := net.SplitHostPort(...)`.
TEST_F(SideEffectExtraction, GoMiddleBlankWithErrCapturedIsNotDropped) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(addr string) error {\n"
                               "\thost, _, err := net.SplitHostPort(addr)\n"
                               "\tuse(host)\n"
                               "\treturn err\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 0);
}

// `v, _ := m[k]` discards a map ok-bool.
TEST_F(SideEffectExtraction, GoMapReadBlankIsNotDroppedError) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(m map[string]int) {\n"
                               "\tv, _ := m[\"k\"]\n"
                               "\tuse(v)\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 0);
}

TEST_F(SideEffectExtraction, GoCheckedErrIsNotDropped) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f() error {\n"
                               "\terr := run()\n"
                               "\tif err != nil {\n"
                               "\t\treturn err\n"
                               "\t}\n"
                               "\treturn nil\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 0);
}

// ---------------------------------------------------------------------------
// Phase 3 — resource acquire/release pairing
// ---------------------------------------------------------------------------

TEST_F(SideEffectExtraction, GoOpenWithoutCloseIsLeakNoRelease) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(p string) {\n"
                               "\th, _ := os.Open(p)\n"
                               "\tuse(h)\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(info->resource_acquires.size(), 1u);
    EXPECT_EQ(info->resource_acquires[0].callee, "Open");
    EXPECT_EQ(count_findings(info->resource_findings, EhSignal::LeakNoRelease),
              1);
}

TEST_F(SideEffectExtraction, GoDeferredCloseIsGuardedAndClean) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(p string) {\n"
                               "\th, _ := os.Open(p)\n"
                               "\tdefer h.Close()\n"
                               "\tuse(h)\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(info->resource_releases.size(), 1u);
    EXPECT_TRUE(info->resource_releases[0].guarded);
    EXPECT_TRUE(info->resource_findings.empty());
}

TEST_F(SideEffectExtraction, PythonWithOpenIsGuardedAcquire) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f(p):\n"
                               "    with open(p) as h:\n"
                               "        return h.read()\n",
                               "f");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(info->resource_acquires.size(), 1u);
    EXPECT_TRUE(info->resource_acquires[0].guarded);
    EXPECT_TRUE(info->resource_findings.empty());
}

TEST_F(SideEffectExtraction, JsUnguardedCloseAfterThrowIsErrorPathLeak) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(p) {\n"
                               "  const h = openSync(p);\n"
                               "  if (!h.ok) throw new Error(\"bad\");\n"
                               "  h.closeSync();\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(
        count_findings(info->resource_findings, EhSignal::LeakOnErrorPath), 1);
}

TEST_F(SideEffectExtraction, PythonFinallyCloseIsGuardedRelease) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f(p):\n"
                               "    h = open(p)\n"
                               "    try:\n"
                               "        return h.read()\n"
                               "    finally:\n"
                               "        h.close()\n",
                               "f");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(info->resource_releases.size(), 1u);
    EXPECT_TRUE(info->resource_releases[0].guarded);
    EXPECT_TRUE(info->resource_findings.empty());
}

}  // namespace
}  // namespace lci
