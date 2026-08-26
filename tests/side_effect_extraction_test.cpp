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

// ---------------------------------------------------------------------------
// Second calibration round (trpc / fastapi hand-read, 2026-08-23). Each case
// is a verbatim shape that reported as a swallow and was not one.
// ---------------------------------------------------------------------------

// `result = [getTRPCErrorFromUnknown(cause), undefined]` — trpc
// resolveResponse. A callee whose NAME contains "error" is not a logger; the
// error is wrapped and assigned out of the block. The old rule read any
// "error" substring as a reporter, which also caught isAbortError(cause),
// errors.push(cause) (via the `errors` qualifier) and opts.onError(...).
TEST_F(SideEffectExtraction, JsErrorNamedWrapperIsPropagationNotLog) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(cb) {\n"
                               "  let result;\n"
                               "  try { g(); } catch (cause) {\n"
                               "    result = [getTRPCErrorFromUnknown(cause), undefined];\n"
                               "  }\n"
                               "  return result;\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, JsPushingTheCauseIntoACollectionIsPropagation) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(errors) {\n"
                               "  try { g(); } catch (cause) { errors.push(cause); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, JsErrorCallbackOnOptsIsPropagation) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(opts) {\n"
                               "  try { g(); } catch (cause) { opts.onError?.({ error: cause }); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

// Real loggers keep their credit: log.error(e), logger.warn(e), console.error.
TEST_F(SideEffectExtraction, JsLogDotErrorIsStillALog) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { log.error('x', e); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::LogAndSwallow), 1);
}

// Assigning the caught error to an outer binding hands it to whoever reads
// that binding — the same exit as a callback, spelled as a store.
TEST_F(SideEffectExtraction, JsAssigningTheCauseOutIsPropagation) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  let failure = null;\n"
                               "  try { g(); } catch (e) { failure = e; }\n"
                               "  return failure;\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

// trpc ws adapter: `new TRPCError({ cause })` + respond(...) + `return []`.
// The error was wrapped and reported before the sentinel; the sentinel is
// the value of a handled failure, not a renamed one.
TEST_F(SideEffectExtraction, JsSentinelAfterPropagationIsNotASwallow) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function parse(data, respond) {\n"
                               "  try { return decode(data); } catch (cause) {\n"
                               "    const error = new TRPCError({ code: 'PARSE_ERROR', cause });\n"
                               "    respond({ id: null, error });\n"
                               "    return [];\n"
                               "  }\n"
                               "}\n",
                               "parse");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::ErrorToSentinel), 0);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
}

// A guard that returns on one recognized condition and forwards every other
// error has not swallowed anything (trpc sseStreamProducer).
TEST_F(SideEffectExtraction, JsIgnoreOneConditionForwardTheRestIsClean) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function* f(opts) {\n"
                               "  try { g(); } catch (cause) {\n"
                               "    if (isAbortError(cause)) { return; }\n"
                               "    const error = getTRPCErrorFromUnknown(cause);\n"
                               "    yield opts.formatError({ error });\n"
                               "  }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
}

// Python: `errors.append(get_missing_field_error(loc))` collects a validation
// error for the caller; it is neither a log nor a swallow.
TEST_F(SideEffectExtraction, PyCollectingAnErrorIsPropagation) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f(body, errors):\n"
                               "    try:\n"
                               "        v = body.get('x')\n"
                               "    except AttributeError as e:\n"
                               "        errors.append(get_missing_field_error(e))\n"
                               "        return\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::LogAndSwallow), 0);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
}

// -- Typed catches of normal conditions ---------------------------------------
//
// `except TimeoutError: send keepalive`, `except EndOfStream: pass`,
// `except WebSocketDisconnect: cleanup()` (fastapi). The exception type names
// the normal end of a protocol, not a failure; the handler is the protocol.

TEST_F(SideEffectExtraction, PyTimeoutAsKeepaliveTriggerIsNotASwallow) {
    const auto* info = analyze(Language::Python, ".py",
                               "async def f(stream, send):\n"
                               "    try:\n"
                               "        data = await stream.receive()\n"
                               "    except TimeoutError:\n"
                               "        await send(KEEPALIVE)\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, PyEndOfStreamPassIsNotAnEmptyCatch) {
    const auto* info = analyze(Language::Python, ".py",
                               "async def f(stream):\n"
                               "    try:\n"
                               "        await stream.receive()\n"
                               "    except anyio.EndOfStream:\n"
                               "        pass\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, PyWebSocketDisconnectHandlerIsNotASwallow) {
    const auto* info = analyze(Language::Python, ".py",
                               "async def f(ws, manager):\n"
                               "    try:\n"
                               "        while True:\n"
                               "            await ws.receive_text()\n"
                               "    except WebSocketDisconnect:\n"
                               "        manager.disconnect(ws)\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

// A specific type that is NOT a normal condition still counts.
TEST_F(SideEffectExtraction, PyTypedIoErrorSwallowStillCounts) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f(path):\n"
                               "    try:\n"
                               "        return open(path).read()\n"
                               "    except IOError:\n"
                               "        retry()\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 1);
}

// A typed catch with a recovery is the anticipated-failure shape: still a
// finding (the cause is gone), but med — the author named what they expected.
TEST_F(SideEffectExtraction, PyTypedRecoveryIsMedNotHigh) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f(call):\n"
                               "    try:\n"
                               "        sig = signature(call)\n"
                               "    except NameError:\n"
                               "        sig = fallback_signature(call)\n"
                               "    return sig\n",
                               "f");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 1);
    EXPECT_EQ(info->error_findings[0].severity, FindingSeverity::Med);
    EXPECT_EQ(info->error_findings[0].detail, "caught=NameError, typed recovery");
}

TEST_F(SideEffectExtraction, JsUntypedCatchAndContinueStaysHigh) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { recover(); }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 1);
    EXPECT_EQ(info->error_findings[0].severity, FindingSeverity::High);
}

// trpc SSE producer: five `controller.enqueue(...)` around a throw. A stream
// controller's queue is memory; nothing outlives the call to tear.
TEST_F(SideEffectExtraction, JsStreamEnqueueIsNotDurableWork) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function write(controller, chunk) {\n"
                               "  controller.enqueue('a');\n"
                               "  if (!chunk) { throw new Error('bad'); }\n"
                               "  controller.enqueue('b');\n"
                               "}\n",
                               "write");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 0);
}

// -- broad-catch that keeps the cause -----------------------------------------
//
// fastapi: `except Exception as e: raise HTTPException(...) from e`. The
// breadth is the point (translate anything into the API's error) and the
// cause chain survives. Nothing is lost, so nothing is reported.

TEST_F(SideEffectExtraction, PyBroadCatchRethrownWithCauseIsClean) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f():\n"
                               "    try:\n"
                               "        g()\n"
                               "    except Exception as e:\n"
                               "        raise HTTPException(400) from e\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, PyBroadCatchThatSwallowsStillReports) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f():\n"
                               "    try:\n"
                               "        g()\n"
                               "    except Exception:\n"
                               "        ctx = None\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 1);
}

// -- try*-named sentinel return, TS `catch {}` without a binding -------------
//
// trpc tryImportRouter: `catch { return null; }` with a leading comment. The
// try prefix exempts the sentinel; the comment must not turn it into a
// catch-and-continue.
TEST_F(SideEffectExtraction, TsTryPrefixedSentinelWithCommentIsClean) {
    const auto* info = analyze(Language::TypeScript, ".ts",
                               "async function tryImportRouter(p: string): Promise<Router | null> {\n"
                               "  try {\n"
                               "    const mod = await import(p);\n"
                               "    return findRouterExport(mod);\n"
                               "  } catch {\n"
                               "    // Dynamic import not available — fall back.\n"
                               "    return null;\n"
                               "  }\n"
                               "}\n",
                               "tryImportRouter");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

// -- partial-write-risk on object factories -----------------------------------
//
// trpc createRouterFactory: ten `createLazyLoader(...)` calls assigning into a
// local record. A bare `create*` call is a factory far more often than a
// durable write in JS/TS; only a receiver (`db.createUser`) says otherwise.
TEST_F(SideEffectExtraction, JsBareCreateFactoriesAreNotAPartialWrite) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function build(keys) {\n"
                               "  const out = {};\n"
                               "  out.a = createLazyLoader(keys[0]);\n"
                               "  out.b = createLazyLoader(keys[1]);\n"
                               "  try { check(out); } catch (e) { throw e; }\n"
                               "  out.c = createLazyLoader(keys[2]);\n"
                               "  return out;\n"
                               "}\n",
                               "build");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 0);
}

TEST_F(SideEffectExtraction, JsQualifiedCreateCallsStillTear) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function place(db, order) {\n"
                               "  db.createOrder(order);\n"
                               "  if (!order.ok) { throw new Error('bad'); }\n"
                               "  db.createShipment(order);\n"
                               "}\n",
                               "place");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 1);
}

// -- Suppression directives ---------------------------------------------------
//
// The eslint / clang-tidy shape: `lci-disable-next-line <rule>`,
// `lci-disable-line <rule>`, and a `lci-disable <rule>` ... `lci-enable`
// block, in any comment. A rule list is optional; bare disables everything.

TEST_F(SideEffectExtraction, DisableNextLineSuppressesThatRule) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  // lci-disable-next-line empty-catch\n"
                               "  try { g(); } catch (e) { }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, DisableNextLineForAnotherRuleDoesNotSuppress) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  // lci-disable-next-line broad-catch\n"
                               "  try { g(); } catch (e) { }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 1);
}

TEST_F(SideEffectExtraction, DisableLineTrailingCommentSuppresses) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  try { g(); } catch (e) { } // lci-disable-line\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, DisableBlockSuppressesUntilEnable) {
    const auto* info = analyze(Language::Python, ".py",
                               "# lci-disable empty-catch, catch-and-continue\n"
                               "def f():\n"
                               "    try:\n"
                               "        g()\n"
                               "    except IOError:\n"
                               "        pass\n"
                               "# lci-enable\n"
                               "def h():\n"
                               "    try:\n"
                               "        g()\n"
                               "    except IOError:\n"
                               "        pass\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
    const SideEffectInfo* after = nullptr;
    for (const auto& [key, i] : analyzer_->results()) {
        if (i.function_name == "h") after = &i;
    }
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(count_findings(after->error_findings, EhSignal::EmptyCatch), 1);
}

TEST_F(SideEffectExtraction, BlockCommentDirectiveIsRecognized) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "/* lci-disable partial-write-risk */\n"
                               "function place(db, order) {\n"
                               "  db.createOrder(order);\n"
                               "  if (!order.ok) { throw new Error('bad'); }\n"
                               "  db.createShipment(order);\n"
                               "}\n",
                               "place");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 0);
}

// ---------------------------------------------------------------------------
// Third calibration round (gson / okhttp / serilog / guzzle / sinatra /
// pocketbase, 2026-08-23).
// ---------------------------------------------------------------------------

// -- The caught variable's name is the developer's verdict --------------------
//
// `catch (NumberFormatException ignored)` (gson), `catch (_: Exception)`
// (okhttp). IntelliJ, detekt and checkstyle all read these names as the
// explicit "I know" marker; so does lci.

TEST_F(SideEffectExtraction, JavaIgnoredVariableIsAnExplicitDiscard) {
    const auto* info = analyze(Language::Java, ".java",
                               "class C {\n"
                               "  long next(String s) {\n"
                               "    try { return Long.parseLong(s); }\n"
                               "    catch (NumberFormatException ignored) {\n"
                               "      // Fall back to parse as a double below.\n"
                               "    }\n"
                               "    return 0;\n"
                               "  }\n"
                               "}\n",
                               "next");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, KotlinUnderscoreVariableIsAnExplicitDiscard) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun f(s: Socket) {\n"
                               "  try { s.close() } catch (_: Exception) { }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

// A plain `e` keeps the finding — the name says nothing.
TEST_F(SideEffectExtraction, JavaPlainVariableEmptyCatchStillCounts) {
    const auto* info = analyze(Language::Java, ".java",
                               "class C {\n"
                               "  void f() {\n"
                               "    try { g(); } catch (NumberFormatException e) { }\n"
                               "  }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 1);
}

// -- The function's name is the contract --------------------------------------
//
// okhttp: closeQuietly, ignoreIoExceptions, toHttpUrlOrNull, buildIfSupported.
// The name promises that failures are absorbed; the catch is the promise
// being kept. The sentinel rule already read these names for `return null`
// inside the catch; the same names now clear the site whatever its body.

TEST_F(SideEffectExtraction, KotlinQuietlyNamedFunctionAbsorbsByContract) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun Closeable.closeQuietly() {\n"
                               "  try { close() } catch (e: RuntimeException) { throw e }\n"
                               "  catch (e: Exception) { }\n"
                               "}\n",
                               "closeQuietly");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, KotlinOrNullFunctionWithSentinelOutsideCatchIsClean) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun String.toHttpUrlOrNull(): HttpUrl? {\n"
                               "  try { return toHttpUrl() } catch (_: IllegalArgumentException) { }\n"
                               "  return null\n"
                               "}\n",
                               "toHttpUrlOrNull");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

TEST_F(SideEffectExtraction, KotlinIgnorePrefixedFunctionAbsorbsByContract) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "inline fun ignoreIoExceptions(block: () -> Unit) {\n"
                               "  try { block() } catch (e: IOException) { }\n"
                               "}\n",
                               "ignoreIoExceptions");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

// -- Cleanup methods may not throw ---------------------------------------------
//
// .NET's Dispose guideline, PHP's __destruct, Java's close(): a throw from
// teardown masks the original failure or crashes the finalizer. serilog's
// sinks catch-log-continue in Dispose on purpose. Reported, but capped at
// low: the swallow is the documented behavior of the method.

TEST_F(SideEffectExtraction, CSharpDisposeSwallowIsCappedAtLow) {
    const auto* info = analyze(Language::CSharp, ".cs",
                               "class S {\n"
                               "  public void Dispose() {\n"
                               "    try { _inner.Dispose(); }\n"
                               "    catch (Exception ex) { SelfLog.WriteLine(\"x\", ex); }\n"
                               "  }\n"
                               "}\n",
                               "Dispose");
    ASSERT_NE(info, nullptr);
    ASSERT_FALSE(info->error_findings.empty());
    for (const auto& f : info->error_findings) {
        EXPECT_EQ(f.severity, FindingSeverity::Low) << to_string(f.signal);
    }
}

TEST_F(SideEffectExtraction, PhpDestructorSwallowIsCappedAtLow) {
    const auto* info = analyze(Language::PHP, ".php",
                               "<?php\n"
                               "class H {\n"
                               "  public function __destruct() {\n"
                               "    try { $this->close(); } catch (\\Throwable $e) { }\n"
                               "  }\n"
                               "}\n",
                               "__destruct");
    ASSERT_NE(info, nullptr);
    ASSERT_FALSE(info->error_findings.empty());
    for (const auto& f : info->error_findings) {
        EXPECT_EQ(f.severity, FindingSeverity::Low) << to_string(f.signal);
    }
}

// -- Local writes are not global writes (2026-08-26 re-panel) -----------------
// Locals were registered nowhere, so every reassigned local classified as a
// global write in every language (guzzle: global_writes=725 with zero
// `global` statements). Declarations (and assignment-declares languages)
// must register the name; writes to genuinely package-level names stay
// global.

TEST_F(SideEffectExtraction, GoLocalReassignmentIsNotAGlobalWrite) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f() int {\n"
                               "\tx := 1\n"
                               "\tx = 2\n"
                               "\tx += 3\n"
                               "\treturn x\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->categories & side_effect::kGlobalWrite, 0u)
        << (info->impurity_reasons.empty() ? ""
                                           : info->impurity_reasons[0]);
}

TEST_F(SideEffectExtraction, GoPackageVarWriteStaysGlobal) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "var counter int\n"
                               "func bump() {\n"
                               "\tcounter = counter + 1\n"
                               "}\n",
                               "bump");
    ASSERT_NE(info, nullptr);
    EXPECT_NE(info->categories & side_effect::kGlobalWrite, 0u);
}

TEST_F(SideEffectExtraction, PhpLocalAssignmentIsNotAGlobalWrite) {
    const auto* info = analyze(Language::PHP, ".php",
                               "<?php\n"
                               "class C {\n"
                               "  private $conf;\n"
                               "  public function build($opts) {\n"
                               "    $result = [];\n"
                               "    $result = $opts;\n"
                               "    $this->conf = $result;\n"
                               "    return $result;\n"
                               "  }\n"
                               "}\n",
                               "build");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->categories & side_effect::kGlobalWrite, 0u)
        << (info->impurity_reasons.empty() ? ""
                                           : info->impurity_reasons[0]);
    // $this->conf mutation is receiver state.
    EXPECT_NE(info->categories & side_effect::kReceiverWrite, 0u);
}

TEST_F(SideEffectExtraction, PythonLocalAssignmentIsNotAGlobalWrite) {
    const auto* info = analyze(Language::Python, ".py",
                               "def f(n):\n"
                               "    acc = 0\n"
                               "    acc = acc + n\n"
                               "    return acc\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->categories & side_effect::kGlobalWrite, 0u);
}

// -- Grammar gaps surfaced by the corpora -------------------------------------

// sinatra dispatch!: `rescue ::Exception => e; invoke { handle_exception!(e) }`
// was catch-and-continue — the error is handed to a handler inside a block.
TEST_F(SideEffectExtraction, RubyRescueHandingTheErrorToABlockCallIsPropagation) {
    const auto* info = analyze(Language::Ruby, ".rb",
                               "def dispatch!\n"
                               "  route!\n"
                               "rescue ::Exception => e\n"
                               "  invoke { handle_exception!(e) }\n"
                               "end\n",
                               "dispatch!");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
}

// sinatra last_modified: `rescue ArgumentError` followed by `end` is an empty
// rescue; it read as catch-and-continue because the body was not found.
TEST_F(SideEffectExtraction, RubyEmptyRescueIsAnEmptyCatch) {
    const auto* info = analyze(Language::Ruby, ".rb",
                               "def last_modified(time)\n"
                               "  since = Time.httpdate(time).to_i\n"
                               "rescue ArgumentError\n"
                               "end\n",
                               "last_modified");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
}

// guzzle createHeaderFn: `$easy->createResponseException = $e; return -1;`
// stores the error for the caller. PHP's caught variable is a variable_name
// node, which the header scan did not recognize.
TEST_F(SideEffectExtraction, PhpStoringTheCaughtErrorIsPropagation) {
    const auto* info = analyze(Language::PHP, ".php",
                               "<?php\n"
                               "function run($easy) {\n"
                               "  try { $easy->createResponse(); } catch (\\Throwable $e) {\n"
                               "    $easy->createResponseException = $e;\n"
                               "    return -1;\n"
                               "  }\n"
                               "}\n",
                               "run");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::ErrorToSentinel), 0);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 1);
}

// -- Go: an explicit discard is errcheck's default exclusion --------------------
//
// `_ = w.Close()` is the developer writing "I know". errcheck, the Go
// community's own tool, does not report it unless asked (-blank). pocketbase
// had 29 of these, every one a cleanup or best-effort call. Reported at low;
// inside a defer it is the cleanup idiom and not reported at all.

TEST_F(SideEffectExtraction, GoExplicitDiscardIsLow) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(w io.Closer) {\n"
                               "\t_ = w.Close()\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 1);
    EXPECT_EQ(info->error_findings[0].severity, FindingSeverity::Low);
}

TEST_F(SideEffectExtraction, GoDeferredExplicitDiscardIsNotReported) {
    const auto* info = analyze(Language::Go, ".go",
                               "package p\n"
                               "func f(w io.Closer) {\n"
                               "\tdefer func() { _ = w.Close() }()\n"
                               "\twork()\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::DroppedError), 0);
}

// sinatra runner.rb: `rescue Exception; @log; end` — Ruby's implicit return.
// The rescue body's last expression IS the value; it is not a return node.
TEST_F(SideEffectExtraction, RubyImplicitReturnInRescueSurfacesAValue) {
    const auto* info = analyze(Language::Ruby, ".rb",
                               "def log\n"
                               "  loop { @log << pipe.read_nonblock(1) }\n"
                               "rescue Exception\n"
                               "  @log\n"
                               "end\n",
                               "log");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 1);
}

TEST_F(SideEffectExtraction, RubyImplicitNilInRescueIsASentinel) {
    const auto* info = analyze(Language::Ruby, ".rb",
                               "def host(ref)\n"
                               "  URI.parse(ref).host\n"
                               "rescue URI::InvalidURIError\n"
                               "  nil\n"
                               "end\n",
                               "host");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(count_findings(info->error_findings, EhSignal::ErrorToSentinel), 1);
    EXPECT_EQ(info->error_findings[0].detail, "caught=URI::InvalidURIError, typed");
}

// gson JavaVersion: `catch (NumberFormatException e) { return -1; }`. A
// sentinel for a NAMED failure is the anticipated-failure shape; same
// confidence cut as typed recovery, same severity.
TEST_F(SideEffectExtraction, JavaTypedSentinelHasReducedConfidence) {
    const auto* info = analyze(Language::Java, ".java",
                               "class C {\n"
                               "  int parse(String s) {\n"
                               "    try { return Integer.parseInt(s); }\n"
                               "    catch (NumberFormatException e) { return -1; }\n"
                               "  }\n"
                               "}\n",
                               "parse");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(count_findings(info->error_findings, EhSignal::ErrorToSentinel), 1);
    EXPECT_DOUBLE_EQ(info->error_findings[0].confidence, 0.5);
}

// -- Kotlin: arguments live under call_suffix, and a catch is an expression --
//
// okhttp DnsOverHttps: `catch (e: Exception) { synchronized(failures) {
// failures.add(e) } }` collects the error; RealWebSocket: `failWebSocket(e =
// e)` hands it on. Both read as catch-and-continue because the Kotlin
// grammar keeps arguments in call_suffix > value_arguments, not an
// `arguments` field.
TEST_F(SideEffectExtraction, KotlinCollectingTheErrorInsideSynchronizedIsPropagation) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun f() {\n"
                               "  try { g() } catch (e: Exception) {\n"
                               "    synchronized(failures) { failures.add(e) }\n"
                               "  }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
}

TEST_F(SideEffectExtraction, KotlinNamedArgumentHandoffIsPropagation) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun f() {\n"
                               "  try { g() } catch (e: IOException) { failWebSocket(e = e) }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->error_findings.empty());
}

// Jdk9Platform: `catch (e: UnsupportedOperationException) { null }` — the
// try is an expression and the catch's last expression is its value.
TEST_F(SideEffectExtraction, KotlinCatchExpressionValueNullIsATypedSentinel) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun proto(s: SSLSocket): String? =\n"
                               "  try {\n"
                               "    s.applicationProtocol\n"
                               "  } catch (e: UnsupportedOperationException) {\n"
                               "    null\n"
                               "  }\n",
                               "proto");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 0);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
    ASSERT_EQ(count_findings(info->error_findings, EhSignal::ErrorToSentinel), 1);
    EXPECT_DOUBLE_EQ(info->error_findings[0].confidence, 0.5);
}

// FastFallbackExchangeFinder: `catch (e: Throwable) { FailedPlan(e) }` —
// the error is wrapped into the expression's value.
TEST_F(SideEffectExtraction, KotlinCatchExpressionWrappingTheErrorIsPropagation) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun plan(): Plan =\n"
                               "  try { routePlanner.plan() } catch (e: Throwable) { FailedPlan(e) }\n",
                               "plan");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
}

// okhttp CallServerInterceptor: one durable call in the if arm, one in the
// else arm. Kotlin's else is a control_structure_body under `alternative`,
// not an else_clause node, so the arms were not separated.
TEST_F(SideEffectExtraction, KotlinIfElseArmsAreNotASequence) {
    const auto* info = analyze(Language::Kotlin, ".kt",
                               "fun send(duplex: Boolean) {\n"
                               "  if (duplex) {\n"
                               "    exchange.createRequestBody(request, true)\n"
                               "  } else {\n"
                               "    exchange.createRequestBody(request, false)\n"
                               "  }\n"
                               "  try { finish() } catch (e: IOException) { throw e }\n"
                               "}\n",
                               "send");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 0);
}

// ---------------------------------------------------------------------------
// Self-hosted round (lci-cpp itself, 2026-08-23): C++ and CLI-builder shapes.
// ---------------------------------------------------------------------------

// `catch (const nlohmann::json::exception&) { error_response(res, 400, ..);
// return; }` — a typed catch with no capital letter in the type. Typed means
// "names a type other than the variable", not "has an uppercase word".
TEST_F(SideEffectExtraction, CppLowercaseQualifiedTypeIsATypedCatch) {
    const auto* info = analyze(Language::Cpp, ".cpp",
                               "void f(Req& req, Res& res) {\n"
                               "  try { parse(req.body); }\n"
                               "  catch (const nlohmann::json::exception&) {\n"
                               "    error_response(res, 400, \"invalid JSON body\");\n"
                               "    return;\n"
                               "  }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 1);
    EXPECT_EQ(info->error_findings[0].severity, FindingSeverity::Med);
    EXPECT_EQ(info->error_findings[0].detail,
              "caught=nlohmann::json::exception, typed recovery");
}

// `std::cerr << "..." << e.what()` is how C++ logs. Not a call node.
TEST_F(SideEffectExtraction, CppStreamInsertionToCerrIsALog) {
    const auto* info = analyze(Language::Cpp, ".cpp",
                               "void f() {\n"
                               "  try { parse(); }\n"
                               "  catch (const std::exception& e) {\n"
                               "    std::cerr << \"dropping: \" << e.what() << '\\n';\n"
                               "  }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::LogAndSwallow), 1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
}

// `line.begin()` is an iterator and `std::cout.flush()` drains a stream;
// neither is a transaction. The bare verbs count only on a receiver that
// looks like a store (db, tx, session, repo, ...).
TEST_F(SideEffectExtraction, CppIteratorBeginAndStreamFlushAreNotATransaction) {
    const auto* info = analyze(Language::Cpp, ".cpp",
                               "void f(std::string line) {\n"
                               "  auto it = line.begin();\n"
                               "  std::cout << *it;\n"
                               "  std::cout.flush();\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::UncompensatedTransaction), 0);
}

TEST_F(SideEffectExtraction, JsTxBeginCommitOnADbReceiverStillCounts) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(db) {\n"
                               "  db.begin();\n"
                               "  db.updateBalance(1);\n"
                               "  db.commit();\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings,
                             EhSignal::UncompensatedTransaction), 1);
}

// `app.add_option(...)` x45 in main() is a CLI11 builder; `crypto.createHash`
// is a factory on a library namespace. A receiver decides, and neither of
// these looks like a store.
TEST_F(SideEffectExtraction, JsCreateOnALibraryNamespaceIsNotDurable) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f(data, tarball) {\n"
                               "  const h = crypto.createHash('sha256').update(data);\n"
                               "  if (!h) { throw new Error('x'); }\n"
                               "  fs.writeFileSync(tarball, data);\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 0);
}

TEST_F(SideEffectExtraction, CppCliBuilderAddOptionIsNotDurable) {
    const auto* info = analyze(Language::Cpp, ".cpp",
                               "int main() {\n"
                               "  CLI::App app;\n"
                               "  app.add_option(\"-r\", root);\n"
                               "  app.add_flag(\"-v\", verbose);\n"
                               "  try { app.parse(argc, argv); } catch (const CLI::ParseError& e) { return app.exit(e); }\n"
                               "  app.add_subcommand(\"x\");\n"
                               "}\n",
                               "main");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 0);
}

TEST_F(SideEffectExtraction, JsRepoCreateCallsStillTear) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function place(repo, order) {\n"
                               "  repo.createOrder(order);\n"
                               "  if (!order.ok) { throw new Error('bad'); }\n"
                               "  repo.createShipment(order);\n"
                               "}\n",
                               "place");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 1);
}

// `shutdown_locked`, `handle_shutdown`, `onClose`: cleanup by any spelling.
TEST_F(SideEffectExtraction, CppShutdownHelperIsACleanupMethod) {
    const auto* info = analyze(Language::Cpp, ".cpp",
                               "void IndexServer::shutdown_locked() {\n"
                               "  try { watcher_->stop(); } catch (...) { }\n"
                               "}\n",
                               "IndexServer::shutdown_locked");
    ASSERT_NE(info, nullptr);
    ASSERT_FALSE(info->error_findings.empty());
    for (const auto& f : info->error_findings) {
        EXPECT_EQ(f.severity, FindingSeverity::Low) << to_string(f.signal);
    }
}

TEST_F(SideEffectExtraction, CppArrowCallSplitsOnTheReceiver) {
    const auto* info = analyze(Language::Cpp, ".cpp",
                               "int main() {\n"
                               "  auto* update_cmd = app.add_subcommand(\"update\");\n"
                               "  update_cmd->add_flag(\"--check\", check);\n"
                               "  update_cmd->add_flag(\"--force\", force);\n"
                               "  try { app.parse(argc, argv); } catch (const CLI::ParseError& e) { return app.exit(e); }\n"
                               "  update_cmd->add_option(\"--to\", to);\n"
                               "}\n",
                               "main");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 0);
}

// A typed empty catch named the failure it ignores: med, like typed
// recovery. The untyped forms stay high.
TEST_F(SideEffectExtraction, TypedEmptyCatchIsMedUntypedStaysHigh) {
    const auto* typed = analyze(Language::Cpp, ".cpp",
                                "void f() {\n"
                                "  try { g(); } catch (const nlohmann::json::exception&) { }\n"
                                "}\n",
                                "f");
    ASSERT_NE(typed, nullptr);
    ASSERT_EQ(count_findings(typed->error_findings, EhSignal::EmptyCatch), 1);
    EXPECT_EQ(typed->error_findings[0].severity, FindingSeverity::Med);
    const auto* untyped = analyze(Language::Cpp, ".cpp",
                                  "void f() {\n"
                                  "  try { g(); } catch (...) { }\n"
                                  "}\n",
                                  "f");
    ASSERT_NE(untyped, nullptr);
    ASSERT_EQ(count_findings(untyped->error_findings, EhSignal::EmptyCatch), 1);
    EXPECT_EQ(untyped->error_findings[0].severity, FindingSeverity::High);
}

// guzzle CurlMultiHandler: `$entry['deferred']->reject($e); continue;` — the
// error goes to the promise. PHP's method call is a member_call_expression.
TEST_F(SideEffectExtraction, PhpMethodCallHandoffIsPropagation) {
    const auto* info = analyze(Language::PHP, ".php",
                               "<?php\n"
                               "function tick($entry) {\n"
                               "  try { $r = finish($entry); } catch (\\Throwable $e) {\n"
                               "    $entry['deferred']->reject($e);\n"
                               "    return;\n"
                               "  }\n"
                               "}\n",
                               "tick");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::CatchAndContinue), 0);
}

// A silenced finding is counted, so the report can print `suppressed=N`
// and a silenced report never looks like a clean one.
TEST_F(SideEffectExtraction, SuppressedFindingsAreCounted) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function f() {\n"
                               "  // lci-disable-next-line empty-catch\n"
                               "  try { g(); } catch (e) { }\n"
                               "  try { h(); } catch (e) { }\n"
                               "}\n",
                               "f");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->suppressed_findings, 1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::EmptyCatch), 1);
}

// pocketbase ui: `app.utils.deleteByPath(app.store.errors, "fields")` — a
// utility namespace mutates an in-memory object. A receiver that names a
// helper bag is the opposite signal of a store, whatever the verb.
TEST_F(SideEffectExtraction, JsUtilityNamespaceMutationIsNotDurable) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function reset(app) {\n"
                               "  app.utils.deleteByPath(app.store.errors, 'fields');\n"
                               "  if (!app.ok) { throw new Error('x'); }\n"
                               "  app.utils.setByPath(app.store.errors, 'fields', {});\n"
                               "}\n",
                               "reset");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 0);
}

// The same verbs on a bare call or a store receiver still count.
TEST_F(SideEffectExtraction, JsBareCompoundDeleteStillCounts) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function purge(order) {\n"
                               "  deleteOrderRecord(order);\n"
                               "  if (!order.ok) { throw new Error('x'); }\n"
                               "  deleteShipmentRecord(order);\n"
                               "}\n",
                               "purge");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk), 1);
}

// -- Round 5 (redis, RxJava, rack, Newtonsoft.Json, click) --------------------

// click's TextIOWrapper.__del__ is a Python finalizer: `try { detach() }
// except Exception: pass` is the documented teardown behavior, same as
// Dispose/__destruct. Was reported at high.
TEST_F(SideEffectExtraction, PythonDelFinalizerSwallowIsCappedAtLow) {
    const auto* info = analyze(Language::Python, ".py",
                               "class W:\n"
                               "    def __del__(self):\n"
                               "        try:\n"
                               "            self.detach()\n"
                               "        except Exception:\n"
                               "            pass\n",
                               "__del__");
    ASSERT_NE(info, nullptr);
    ASSERT_FALSE(info->error_findings.empty());
    for (const auto& f : info->error_findings) {
        EXPECT_EQ(f.severity, FindingSeverity::Low) << to_string(f.signal);
    }
}

// RxJava's Flowable.subscribe: catch (Throwable e) { npe.initCause(e);
// throw npe; }. The cause is chained through initCause on the line before
// the throw, so neither rethrow-no-cause nor broad-catch applies. This one
// idiom produced ~690 of RxJava's 699 findings.
TEST_F(SideEffectExtraction, JavaInitCauseBeforeRethrowKeepsTheCause) {
    const auto* info = analyze(
        Language::Java, ".java",
        "class F {\n"
        "  void subscribe(S s) {\n"
        "    try {\n"
        "      subscribeActual(s);\n"
        "    } catch (Throwable e) {\n"
        "      Exceptions.throwIfFatal(e);\n"
        "      RxJavaPlugins.onError(e);\n"
        "      NullPointerException npe = new NullPointerException(\"x\");\n"
        "      npe.initCause(e);\n"
        "      throw npe;\n"
        "    }\n"
        "  }\n"
        "}\n",
        "subscribe");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::RethrowNoCause),
              0)
        << "initCause(e) chained the cause before the throw";
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 0)
        << "cause-keeping rethrow forgives the breadth";
}

// The other RxJava shape (687 of the remaining findings): catch (Throwable
// ex) { Exceptions.throwIfFatal(ex); ...; onError(ex); }. throwIfFatal IS a
// conditional rethrow of the fatal subset — Errors and fatal exceptions
// re-propagate with their cause, the rest are forwarded whole to onError.
// The breadth is exactly what the guard exists for.
TEST_F(SideEffectExtraction, JavaThrowIfFatalGuardForgivesTheBreadth) {
    const auto* info = analyze(
        Language::Java, ".java",
        "class C {\n"
        "  public void onNext(T t) {\n"
        "    try { collector.accumulate(container, t); }\n"
        "    catch (Throwable ex) {\n"
        "      Exceptions.throwIfFatal(ex);\n"
        "      upstream.cancel();\n"
        "      onError(ex);\n"
        "    }\n"
        "  }\n"
        "}\n",
        "onNext");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 0)
        << "throwIfFatal(ex) re-propagates the fatal subset";
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::RethrowNoCause),
              0);
}

// A rethrow that never chains the cause still fires both.
TEST_F(SideEffectExtraction, JavaRethrowWithoutInitCauseStillFires) {
    const auto* info = analyze(
        Language::Java, ".java",
        "class F {\n"
        "  void run() {\n"
        "    try { work(); }\n"
        "    catch (Throwable e) { throw new IllegalStateException(\"x\"); }\n"
        "  }\n"
        "}\n",
        "run");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::RethrowNoCause),
              1);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::BroadCatch), 1);
}

// rack's method_override_param: two rescue arms each write one line to the
// error stream. Only one arm can run per raise, so they are alternatives,
// not a sequence — same rule as sibling switch cases. Read as a 2-change
// torn write before rescue/except/catch clauses joined the arm list.
TEST_F(SideEffectExtraction, SiblingRescueArmsAreNotASequence) {
    const auto* info = analyze(Language::Ruby, ".rb",
                               "def override(req)\n"
                               "  req.check\n"
                               "rescue InvalidParameterError\n"
                               "  req.get_header(RACK_ERRORS).puts \"bad params\"\n"
                               "rescue EOFError\n"
                               "  req.get_header(RACK_ERRORS).puts \"bad body\"\n"
                               "end\n",
                               "override");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0)
        << "only one rescue arm executes per raise";
}

// Same for Python except arms.
TEST_F(SideEffectExtraction, SiblingExceptArmsAreNotASequence) {
    const auto* info = analyze(Language::Python, ".py",
                               "def override(req):\n"
                               "    try:\n"
                               "        req.check()\n"
                               "    except ValueError:\n"
                               "        session.flush()\n"
                               "    except EOFError:\n"
                               "        session.flush()\n",
                               "override");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0)
        << "only one except arm executes per raise";
}

// rack's show_exceptions `pretty`: `frame.post_context = lines[...]` is a
// Ruby SETTER call named `post_context=`, which reached the work classifier
// and matched the "post" (publish) verb prefix. A setter is an assignment,
// not a verb; and "post" the verb needs a store-like receiver — English
// "post-" (after: post_process, post_init) is a false friend everywhere.
TEST_F(SideEffectExtraction, RubySetterCallsAreAssignmentsNotWork) {
    const auto* info = analyze(Language::Ruby, ".rb",
                               "def pretty(v)\n"
                               "  frame = Frame.new\n"
                               "  begin\n"
                               "    frame.pre_context_lineno = [v, 0].max\n"
                               "    frame.post_context_lineno = [v, 9].min\n"
                               "    frame.post_context = v\n"
                               "  rescue\n"
                               "  end\n"
                               "  frame\n"
                               "end\n",
                               "pretty");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0)
        << "a setter is an assignment, not a publish";
}

TEST_F(SideEffectExtraction, PostPrefixedHooksAreNotPublishes) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function build(x) {\n"
                               "  post_process(x);\n"
                               "  postInit(x);\n"
                               "  if (!x.ok) { throw new Error('bad'); }\n"
                               "  post_validate(x);\n"
                               "}\n",
                               "build");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              0)
        << "post_* hooks run after something; they publish nothing";
}

// The verb survives on a receiver that names where state lives.
TEST_F(SideEffectExtraction, PostOnAStoreReceiverIsStillDurable) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function submit(order) {\n"
                               "  client.post('/orders', order);\n"
                               "  if (!order.ok) { throw new Error('bad'); }\n"
                               "  client.post('/audit', order);\n"
                               "}\n",
                               "submit");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(count_findings(info->error_findings, EhSignal::PartialWriteRisk),
              1)
        << "client.post is the HTTP verb; the torn pair is real";
}

// -- Log level annotation ------------------------------------------------------

// Production log configs routinely drop debug and info: a log-and-swallow at
// level=debug is invisible exactly when the incident happens. The level rides
// on the finding detail; severity is unchanged (annotation, not judgment).
TEST_F(SideEffectExtraction, LogAndSwallowCarriesTheLogLevel) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function load(x) {\n"
                               "  try { return parse(x); }\n"
                               "  catch (e) { logger.debug(e); recover(); }\n"
                               "}\n",
                               "load");
    ASSERT_NE(info, nullptr);
    bool found = false;
    for (const auto& f : info->error_findings) {
        if (f.signal != EhSignal::LogAndSwallow) continue;
        found = true;
        EXPECT_NE(f.detail.find("level=debug"), std::string::npos)
            << f.detail;
    }
    EXPECT_TRUE(found);
}

// The strongest level in the body wins; console.error is level=error.
TEST_F(SideEffectExtraction, StrongestLogLevelWins) {
    const auto* info = analyze(Language::JavaScript, ".js",
                               "function load(x) {\n"
                               "  try { return parse(x); }\n"
                               "  catch (e) {\n"
                               "    console.log('while loading');\n"
                               "    console.error(e);\n"
                               "    recover();\n"
                               "  }\n"
                               "}\n",
                               "load");
    ASSERT_NE(info, nullptr);
    bool found = false;
    for (const auto& f : info->error_findings) {
        if (f.signal != EhSignal::LogAndSwallow) continue;
        found = true;
        EXPECT_NE(f.detail.find("level=error"), std::string::npos)
            << f.detail;
    }
    EXPECT_TRUE(found);
}

}  // namespace
}  // namespace lci
