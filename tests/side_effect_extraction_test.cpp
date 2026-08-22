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

#include <lci/analysis/error_handling_analyzer.h>
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

// pocketbase class: `catch (err) { app.checkApiError(err); }` reports the
// error — a log/report credit (med), not a blind catch-and-continue (high).
TEST_F(SideEffectExtraction, JsErrorReporterCalleeIsLogAndSwallow) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (err) { "
                               "app.checkApiError(err); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::LogAndSwallow), 1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue),
              0);
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

// -- Propagation is not a swallow ----------------------------------------------
//
// An error that leaves the catch block as a call ARGUMENT has been handled;
// `throw` is not the only exit. Each case below is a verbatim shape from the
// err-lookup corpus run that reported it as a swallow.

// express lib/application.js:628 — the repo's ONLY error-handling finding.
TEST_F(SideEffectExtraction, JsCatchPassingCauseToCallbackIsClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function tryRender(view, options, callback) {\n"
                               "  try { view.render(options, callback); }\n"
                               "  catch (err) { callback(err); }\n"
                               "}\n",
                               "tryRender");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty())
        << "an error handed to the callback is propagated, not swallowed";
}

// axios lib/core/Axios.js:243 — three sites of this shape in one function.
TEST_F(SideEffectExtraction, JsCatchRejectingWithCauseIsClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(config) {\n"
                               "  let promise;\n"
                               "  try { promise = dispatch(config); }\n"
                               "  catch (error) { promise = "
                               "Promise.reject(error); }\n"
                               "  return promise;\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue),
              0);
}

// flask src/flask/app.py:1020 — request dispatch handing off to the handler.
TEST_F(SideEffectExtraction, PythonExceptPassingCauseToHandlerIsClean) {
    const auto* info = analyze(Language::Python, ".py",
                               "def full_dispatch_request(self, ctx):\n"
                               "    try:\n"
                               "        rv = self.dispatch_request(ctx)\n"
                               "    except Exception as e:\n"
                               "        rv = self.handle_user_exception(ctx, e)\n",
                               "full_dispatch_request");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue),
              0);
}

// Logging the cause passes it as an argument but hands it to nobody — that is
// precisely what LogAndSwallow describes, so the propagation rule must not
// absorb it. (Caught by JsLogOnlyCatchIsLogAndSwallow on the first cut.)
TEST_F(SideEffectExtraction, JsLoggingTheCauseIsStillASwallow) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { console.error(e); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::LogAndSwallow), 1);
}

// -- Undo cost -----------------------------------------------------------------
//
// What an error COSTS, not just whether it was reported: work that must be
// undone, and work that never gets undone.

// A transaction opened and committed with no rollback anywhere. An error
// between the two leaves the unit half-applied and nothing reverses it.
TEST_F(SideEffectExtraction, TxWithoutRollbackIsUncompensated) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function transfer(a, b, amt) {\n"
                               "  begin();\n"
                               "  updateBalance(a, -amt);\n"
                               "  updateBalance(b, amt);\n"
                               "  commit();\n"
                               "}\n",
                               "transfer");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::UncompensatedTransaction),
              1);
}

// The same function with a rollback is clean — that is the whole point.
TEST_F(SideEffectExtraction, TxWithRollbackIsClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function transfer(a, b, amt) {\n"
                               "  begin();\n"
                               "  try {\n"
                               "    updateBalance(a, -amt);\n"
                               "    updateBalance(b, amt);\n"
                               "    commit();\n"
                               "  } catch (e) { rollback(); throw e; }\n"
                               "}\n",
                               "transfer");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::UncompensatedTransaction),
              0);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0);
}

// The case with no transaction at all — the one that actually loses data.
// Three writes, a fallible point among them, nothing compensates.
TEST_F(SideEffectExtraction, MultipleWritesAroundAThrowIsPartialWriteRisk) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function place(order) {\n"
                               "  saveOrder(order);\n"
                               "  if (!order.ok) { throw new Error('bad'); }\n"
                               "  updateInventory(order);\n"
                               "  createShipment(order);\n"
                               "}\n",
                               "place");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              1);
}

// One write cannot be torn: there is no earlier work left half-applied.
TEST_F(SideEffectExtraction, ASingleWriteIsNotAPartialWriteRisk) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function place(order) {\n"
                               "  if (!order.ok) { throw new Error('bad'); }\n"
                               "  saveOrder(order);\n"
                               "}\n",
                               "place");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0);
}

// Writes with no fallible point between them are just writes.
TEST_F(SideEffectExtraction, WritesWithNoFalliblePointAreClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function seed() {\n"
                               "  insertA();\n"
                               "  insertB();\n"
                               "  insertC();\n"
                               "}\n",
                               "seed");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0);
}

// An explicit compensating call clears it — the author handled the undo.
TEST_F(SideEffectExtraction, CompensatingCallClearsPartialWriteRisk) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function place(order) {\n"
                               "  saveOrder(order);\n"
                               "  try {\n"
                               "    updateInventory(order);\n"
                               "    createShipment(order);\n"
                               "  } catch (e) { undoOrder(order); throw e; }\n"
                               "}\n",
                               "place");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0);
}

// A bare collection verb on a local is not durable work. flask's url_for
// calls `values.update(...)` twice around a raise; nothing outside the frame
// ever saw that dict, so no error can leave it torn.
TEST_F(SideEffectExtraction, BareCollectionVerbsAreNotDurableWork) {
    const auto* info = analyze(Language::Python, ".py",
                               "def url_for(endpoint, values):\n"
                               "    values.update(a=1)\n"
                               "    if endpoint is None:\n"
                               "        raise RuntimeError('no endpoint')\n"
                               "    values.update(b=2)\n"
                               "    return endpoint\n",
                               "url_for");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0)
        << "a local dict cannot be left half-written";
}

// The same holds when the function DOES do real I/O elsewhere. requests'
// Session.send makes network calls and also runs history.insert/history.pop
// on a local list; the list is still local. A first cut let any durable
// effect in the function license every bare verb, and reported both this and
// flask's rows.insert (beside a click.echo) as torn writes.
TEST_F(SideEffectExtraction, DurableIoElsewhereDoesNotMakeLocalListsDurable) {
    const auto* info = analyze(Language::Python, ".py",
                               "def send(self, request):\n"
                               "    r = self.adapter.send(request)\n"
                               "    history = []\n"
                               "    if not r.ok:\n"
                               "        raise ConnectionError('failed')\n"
                               "    history.insert(0, r)\n"
                               "    r = history.pop()\n"
                               "    return r\n",
                               "send");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0);
}

// The same verb compounded with a domain noun IS durable work: it names
// something that lives past the call.
TEST_F(SideEffectExtraction, CompoundDomainVerbsAreDurableWork) {
    const auto* info = analyze(Language::Python, ".py",
                               "def sync(order):\n"
                               "    updateInventory(order)\n"
                               "    if not order.ok:\n"
                               "        raise ValueError('bad')\n"
                               "    updateLedger(order)\n",
                               "sync");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              1);
}

// Ops in sibling switch arms never both run, so they are not a sequence.
// gin's SetMode stores four modes in four cases around a panic.
TEST_F(SideEffectExtraction, SiblingSwitchArmsAreNotASequence) {
    const auto* info = analyze(Language::Go, ".go",
                               "func SetMode(value string) {\n"
                               "\tswitch value {\n"
                               "\tcase DebugMode:\n"
                               "\t\tstoreMode(debugCode)\n"
                               "\tcase ReleaseMode:\n"
                               "\t\tstoreMode(releaseCode)\n"
                               "\tcase TestMode:\n"
                               "\t\tstoreMode(testCode)\n"
                               "\tdefault:\n"
                               "\t\tpanic(\"unknown mode\")\n"
                               "\t}\n"
                               "}\n",
                               "SetMode");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0)
        << "only one arm executes; they cannot tear each other";
}

// A bare word can be a builder rather than an action: zod's `z.email()`
// constructs a validator, it does not send mail.
TEST_F(SideEffectExtraction, BareIrreversibleNounsAreNotActions) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function convert(schema) {\n"
                               "  if (schema.format === 'email') {\n"
                               "    s = s.check(email());\n"
                               "  }\n"
                               "  if (!s) { throw new Error('bad'); }\n"
                               "  return s;\n"
                               "}\n",
                               "convert");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::IrreversibleBeforeFallible),
              0);
}

// Ordering: the charge leaves the process, THEN validation can still throw.
// Doing the fallible part first would leave nothing to undo.
TEST_F(SideEffectExtraction, IrreversibleWorkBeforeAThrowIsFlagged) {
    const auto* info = analyze(Language::Python, ".py",
                               "def checkout(order):\n"
                               "    charge_card(order)\n"
                               "    if not order.valid:\n"
                               "        raise ValueError('bad order')\n",
                               "checkout");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(count_findings(info->error_findings,
                             EhSignal::IrreversibleBeforeFallible),
              1);
    // Advice, not a defect: it must cost less than a swallow.
    for (const auto& f : info->error_findings) {
        if (f.signal != EhSignal::IrreversibleBeforeFallible) continue;
        EXPECT_LT(ErrorHandlingAnalyzer::finding_deduction(f.severity,
                                                           f.confidence, 1.0),
                  ErrorHandlingAnalyzer::finding_deduction(
                      FindingSeverity::High, 0.7, 1.0));
    }
}

// Validate first, then charge: nothing to undo, nothing to report.
TEST_F(SideEffectExtraction, FallibleWorkBeforeIrreversibleIsClean) {
    const auto* info = analyze(Language::Python, ".py",
                               "def checkout(order):\n"
                               "    if not order.valid:\n"
                               "        raise ValueError('bad order')\n"
                               "    charge_card(order)\n",
                               "checkout");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::IrreversibleBeforeFallible),
              0);
}

// Go: a deferred rollback is the idiom, and it covers every return path.
TEST_F(SideEffectExtraction, GoDeferredRollbackIsCompensated) {
    const auto* info = analyze(Language::Go, ".go",
                               "func transfer(db *DB) error {\n"
                               "\ttx := db.Begin()\n"
                               "\tdefer tx.Rollback()\n"
                               "\tif err := tx.Insert(a); err != nil {\n"
                               "\t\treturn err\n"
                               "\t}\n"
                               "\ttx.Insert(b)\n"
                               "\treturn tx.Commit()\n"
                               "}\n",
                               "transfer");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::UncompensatedTransaction),
              0);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0);

    // Discrimination: the SAME function without the defer must fire, so the
    // zeros above are a verdict rather than an absence of Go analysis.
    const auto* undeferred = analyze(Language::Go, ".go",
                                     "func transfer2(db *DB) error {\n"
                                     "\ttx := db.Begin()\n"
                                     "\ttx.Insert(a)\n"
                                     "\ttx.Insert(b)\n"
                                     "\treturn tx.Commit()\n"
                                     "}\n",
                                     "transfer2");
    ASSERT_NE(undeferred, nullptr);
    EXPECT_EQ(count_findings(undeferred->error_findings,
                             EhSignal::UncompensatedTransaction),
              1)
        << "no rollback on any path";
}

// A pure function has no undo cost — none of these may fire on read-only code.
TEST_F(SideEffectExtraction, ReadOnlyCodeHasNoUndoCost) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function total(items) {\n"
                               "  let sum = 0;\n"
                               "  for (const i of items) { sum += i.price; }\n"
                               "  if (sum < 0) { throw new Error('neg'); }\n"
                               "  return sum;\n"
                               "}\n",
                               "total");
    ASSERT_NE(info, nullptr);
    for (const auto& f : info->error_findings) {
        EXPECT_NE(f.signal, EhSignal::PartialWriteRisk);
        EXPECT_NE(f.signal, EhSignal::UncompensatedTransaction);
        EXPECT_NE(f.signal, EhSignal::IrreversibleBeforeFallible);
    }
}

// -- Error becomes a sentinel --------------------------------------------------
//
// `catch (e) { return null; }` is the most common swallow in modern code and
// used to score PERFECT: has_return alone was read as "the error may be
// surfaced through the return value". What is returned decides that — a
// sentinel surfaces nothing, and the caller cannot tell an empty answer from
// a broken one.

TEST_F(SideEffectExtraction, CatchReturningNullIsASwallow) {
    for (const char* sentinel : {"null", "undefined", "false", "0", "{}", "[]"}) {
        std::string src = std::string("function f() {\n"
                                      "  try { return g(); } catch (e) { "
                                      "return ") + sentinel + "; }\n}\n";
        const auto* info = analyze(Language::JavaScript, ".js", src, "f");
        ASSERT_NE(info, nullptr) << sentinel;
        EXPECT_EQ(count_findings(info->error_findings,
                                 EhSignal::ErrorToSentinel),
                  1)
            << "return " << sentinel << " surfaces nothing";
    }
}

TEST_F(SideEffectExtraction, PythonCatchReturningNoneIsASwallow) {
    const auto* info = analyze(Language::Python, ".py",
                               "def find(uid):\n"
                               "    try:\n"
                               "        return db.query(uid)\n"
                               "    except Exception:\n"
                               "        return None\n",
                               "find");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::ErrorToSentinel),
              1);
}

// Returning something DERIVED from the error is real handling and must stay
// clean — otherwise the rule would condemn every Result-style API.
// A name that PROMISES a sentinel makes the sentinel the answer, not a
// swallow. Every real-world hit of the sentinel rule across the corpus was
// this: express's tryStat and four zod validators, all correct code.
TEST_F(SideEffectExtraction, PredicateAndTryNamesMayReturnSentinels) {
    for (const char* fn : {"isValidIPv6", "tryStat", "hasAccess", "canRetry",
                           "is_valid_token", "try_parse",
                           // Suffix form: axios's stringifySafely, and the
                           // Kotlin/Rust getOrNull family.
                           "stringifySafely", "getOrNull", "parseOrDefault"}) {
        std::string src = std::string("function ") + fn +
                          "(x) {\n  try { return g(x); } catch (e) { return "
                          "false; }\n}\n";
        const auto* info = analyze(Language::JavaScript, ".js", src, fn);
        ASSERT_NE(info, nullptr) << fn;
        EXPECT_EQ(count_findings(info->error_findings,
                                 EhSignal::ErrorToSentinel),
                  0)
            << fn << ": the sentinel is the documented answer";
    }
}

// The prefix must be followed by a word boundary — `issue()` and `canvas()`
// are not predicates.
TEST_F(SideEffectExtraction, PrefixLookalikesAreStillSwallows) {
    for (const char* fn : {"issue", "canvas", "testify"}) {
        std::string src = std::string("function ") + fn +
                          "(x) {\n  try { return g(x); } catch (e) { return "
                          "null; }\n}\n";
        const auto* info = analyze(Language::JavaScript, ".js", src, fn);
        ASSERT_NE(info, nullptr) << fn;
        EXPECT_EQ(count_findings(info->error_findings,
                                 EhSignal::ErrorToSentinel),
                  1)
            << fn << " is not a predicate";
    }
}

TEST_F(SideEffectExtraction, CatchReturningAnErrorValueIsClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { return g(); } catch (e) { return "
                               "wrapError(e); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::ErrorToSentinel),
              0);
}

// A sentinel return IS a swallow for the exposure paths: it must feed
// swallow_sites, not just the finding list.
TEST_F(SideEffectExtraction, SentinelReturnCountsAsASwallowSignal) {
    EXPECT_TRUE(
        ErrorHandlingAnalyzer::is_swallow_signal(EhSignal::ErrorToSentinel));
    EXPECT_TRUE(ErrorHandlingAnalyzer::is_swallow_signal(
        EhSignal::FinallyHijacksControlFlow));
    // Narrower than "is a finding": these are defects, but the error did not
    // stop here.
    EXPECT_FALSE(
        ErrorHandlingAnalyzer::is_swallow_signal(EhSignal::LossyPropagation));
    EXPECT_FALSE(
        ErrorHandlingAnalyzer::is_swallow_signal(EhSignal::PartialWriteRisk));
}

// -- finally hijacks control flow ----------------------------------------------
//
// A `return` inside finally DISCARDS whatever exception was propagating —
// in JS, Java, C#, Python and Ruby alike. It is the one error-handling defect
// with no catch site for a reader to notice.

TEST_F(SideEffectExtraction, ReturnInsideFinallyDiscardsTheException) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(x) {\n"
                               "  try { return work(x); } finally { return "
                               "cleanup(); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::FinallyHijacksControlFlow),
              1);
}

TEST_F(SideEffectExtraction, PythonReturnInsideFinallyIsFlagged) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f(x):\n"
                               "    try:\n"
                               "        return work(x)\n"
                               "    finally:\n"
                               "        return cleanup()\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::FinallyHijacksControlFlow),
              1);
}

// A finally that only cleans up is the whole point of finally.
TEST_F(SideEffectExtraction, FinallyThatOnlyCleansUpIsClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(x) {\n"
                               "  try { return work(x); } finally { "
                               "cleanup(); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::FinallyHijacksControlFlow),
              0);
}

// A return inside a callback DEFINED in the finally returns from that
// callback, not through the finally, so it discards nothing.
TEST_F(SideEffectExtraction, ReturnInANestedFunctionInFinallyIsClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(x) {\n"
                               "  try { return work(x); } finally {\n"
                               "    items.forEach(function (i) { return "
                               "log(i); });\n"
                               "  }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::FinallyHijacksControlFlow),
              0);
}

// -- Cause fidelity ------------------------------------------------------------
//
// Forwarding the error is not the same as forwarding a sentence about it.
// `record(e.message)` throws away the stack and the cause chain — .NET's
// InnerException, Java's getCause, Python's __cause__ — leaving a report
// nobody can act on. Partial credit, not a pass.

TEST_F(SideEffectExtraction, JsForwardingOnlyTheMessageIsPartialCredit) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { record(e.message); "
                               "}\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(count_findings(info->error_findings, EhSignal::LossyPropagation),
              1);
    // Strictly cheaper than swallowing outright.
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue),
              0);
    for (const auto& fi : info->error_findings) {
        if (fi.signal != EhSignal::LossyPropagation) continue;
        EXPECT_LT(ErrorHandlingAnalyzer::finding_deduction(fi.severity,
                                                           fi.confidence, 1.0),
                  ErrorHandlingAnalyzer::finding_deduction(
                      FindingSeverity::High, 0.7, 1.0))
            << "forwarding the message must cost less than swallowing";
        EXPECT_NE(fi.detail.find("message only"), std::string::npos);
    }
}

TEST_F(SideEffectExtraction, JsForwardingTheWholeErrorIsClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { record(e); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

// A stack- or cause-preserving projection is not lossy.
TEST_F(SideEffectExtraction, JsForwardingTheStackIsClean) {
    for (const char* body : {"record(e.stack);", "record(e.cause);"}) {
        std::string src = std::string("function f() {\n  try { g(); } catch "
                                      "(e) { ") + body + " }\n}\n";
        const auto* info = analyze(Language::JavaScript, ".js", src, "f");
        ASSERT_NE(info, nullptr) << body;
        EXPECT_TRUE(info->error_findings.empty()) << body;
    }
}

// Same spelling, opposite verdicts. .NET's ToString() renders the message,
// the stack, AND every InnerException; JS's toString() is "Error: msg".
TEST_F(SideEffectExtraction, ToStringIsFullInDotNetAndLossyInJs) {
    const auto* cs = analyze(Language::CSharp, ".cs",
                             "class C {\n"
                             "  void F() {\n"
                             "    try { G(); } catch (Exception ex) { "
                             "Record(ex.ToString()); }\n"
                             "  }\n"
                             "}\n",
                             "F");
    ASSERT_NE(cs, nullptr);
    EXPECT_EQ(count_findings(cs->error_findings, EhSignal::LossyPropagation), 0)
        << ".NET ToString() carries stack + InnerException";

    const auto* js = analyze(Language::JavaScript, ".js",
                             "function f() {\n"
                             "  try { g(); } catch (e) { record(e.toString()); "
                             "}\n"
                             "}\n",
                             "f");
    ASSERT_NE(js, nullptr);
    EXPECT_EQ(count_findings(js->error_findings, EhSignal::LossyPropagation), 1)
        << "JS toString() is the message and nothing else";
}

// Java/Python message accessors are lossy — those errors carry a stack and a
// cause chain that the projection discards.
TEST_F(SideEffectExtraction, JavaGetMessageIsLossy) {
    const auto* info = analyze(Language::Java, ".java",
                               "class C {\n"
                               "  void f() {\n"
                               "    try { g(); } catch (Exception e) { "
                               "record(e.getMessage()); }\n"
                               "  }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::LossyPropagation),
              1);
}

// "Unless that's all the useful data": Go's `error` interface IS
// `Error() string` — no stack, no cause unless the code wrapped with %w. C++'s
// std::exception is `what()`. Demanding more would demand data the language
// never produced.
TEST_F(SideEffectExtraction, GoErrorStringIsFullFidelity) {
    const auto* info = analyze(Language::Go, ".go",
                               "func f() {\n"
                               "\tif err := g(); err != nil {\n"
                               "\t\trecord(err.Error())\n"
                               "\t}\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::LossyPropagation),
              0)
        << "a Go error carries nothing beyond its message";

    // Discrimination: prove the pipeline reaches Go at all, so the zero above
    // is a verdict rather than an absence of analysis.
    const auto* swallowed = analyze(Language::Go, ".go",
                                    "func g2() {\n"
                                    "\t_ = doWork()\n"
                                    "}\n",
                                    "g2");
    ASSERT_NE(swallowed, nullptr);
    EXPECT_EQ(count_findings(swallowed->error_findings, EhSignal::DroppedError),
              1);
}

TEST_F(SideEffectExtraction, CppWhatIsFullFidelity) {
    const auto* info = analyze(Language::Cpp, ".cpp",
                               "void f() {\n"
                               "  try { g(); } catch (const std::exception& e) "
                               "{ record(e.what()); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::LossyPropagation),
              0)
        << "std::exception is what(); there is no stack to lose";

    // Discrimination: an empty catch in the same language still fires, so the
    // zero above is a verdict rather than an absence of analysis.
    const auto* empty = analyze(Language::Cpp, ".cpp",
                                "void g2() {\n"
                                "  try { h(); } catch (const std::exception& "
                                "e) { }\n"
                                "}\n",
                                "g2");
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(count_findings(empty->error_findings, EhSignal::EmptyCatch), 1);
}

// Logging is graded the same way: the bare error prints a stack, the message
// prints a sentence. Both swallow; one is diagnosable.
TEST_F(SideEffectExtraction, LoggingTheMessageCostsMoreThanLoggingTheError) {
    auto deduction_for = [&](const char* body) {
        std::string src = std::string("function f() {\n  try { g(); } catch "
                                      "(e) { ") + body + " }\n}\n";
        const auto* info = analyze(Language::JavaScript, ".js", src, "f");
        EXPECT_NE(info, nullptr) << body;
        double total = 0;
        if (info) {
            for (const auto& fi : info->error_findings) {
                total += ErrorHandlingAnalyzer::finding_deduction(
                    fi.severity, fi.confidence, 1.0);
            }
        }
        return total;
    };
    EXPECT_GT(deduction_for("console.error(e.message);"),
              deduction_for("console.error(e);"));
}

// The discrimination case: a catch that calls something WITHOUT the cause is
// still a swallow. Without this the rule would excuse every non-empty catch.
TEST_F(SideEffectExtraction, JsCatchCallingWithoutCauseStaysAContinue) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { recover(); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue),
              1);
}

// A method called ON the error is not propagation either — nothing receives it.
TEST_F(SideEffectExtraction, JsCatchCallingAMethodOnTheCauseStaysAContinue) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { record(e.message); "
                               "}\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue),
              0)
        << "e.message IS an argument — the cause reaches the call";
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

// Without return-type knowledge, a trailing blank in a multi-value assign is
// as likely an ok-bool (strings.Cut, map reads, chi's FindRoute) as an error —
// chi spot-checks disproved the medium tier, so only sole-discard forms fire.
TEST_F(SideEffectExtraction, GoMultiValueTrailingBlankIsNotFlagged) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f() {\n"
                               "\tv, _ := parse()\n"
                               "\tuse(v)\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 0);
}

// `_ = f()` — the call's only (conventionally error) result thrown away.
TEST_F(SideEffectExtraction, GoSoleDiscardedCallResultIsDropped) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(l *Logger) {\n"
                               "\t_ = l.Flush()\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 1);
}

// `v, _ := x.(T)` discards a type-assertion ok-bool, not an error.
// gin context.go:1213 — `_ = c.Error(err)`. The blank discards c.Error's
// RETURN value; the error itself is handed to the recorder. This was the only
// "unchecked error" reported across the whole gin corpus.
TEST_F(SideEffectExtraction, GoBlankAssignOfAnErrorHandoffIsNotDropped) {
    const auto* info = analyze(Language::Go, ".go",
                               "func (c *Context) Render(r Render) {\n"
                               "\tif err := r.Render(c.Writer); err != nil {\n"
                               "\t\t_ = c.Error(err)\n"
                               "\t\tc.Abort()\n"
                               "\t}\n"
                               "}\n",
                               "Render");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 0)
        << "the error is passed to c.Error; only its return value is blanked";
}

// The discrimination case: a call that receives no error still drops one.
TEST_F(SideEffectExtraction, GoBlankAssignWithoutAnErrorArgStaysDropped) {
    const auto* info = analyze(Language::Go, ".go",
                               "func f(c *Client) {\n"
                               "\t_ = c.Close()\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 1);
}

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

// Factory/constructor pattern (pocketbase DefaultDBConnect): the acquired
// resource escapes via the return — not a leak claim.
TEST_F(SideEffectExtraction, GoConstructorReturnIsNotALeak) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(p string) (*os.File, error) {\n"
                               "\th, err := os.Open(p)\n"
                               "\tif err != nil {\n"
                               "\t\treturn nil, err\n"
                               "\t}\n"
                               "\treturn h, nil\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->resource_findings.empty());
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
