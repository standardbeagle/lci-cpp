#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <lci/analysis/side_effect_analyzer.h>

#include <tree_sitter/api.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace lci::parser {

// ---------------------------------------------------------------------------
// Side effect tracking during traversal.
//
// Ports the Go SideEffectTracker from internal/parser/side_effect_tracking.go.
// Drives the SideEffectAnalyzer per-function lifecycle from real AST facts
// instead of callee-name guessing:
// - Assignment / augmented-assignment / inc-dec -> record_access(Write) on the
//   lvalue base identifier, classified (param / receiver / global / closure) by
//   the analyzer from the parameters + receiver registered on function entry.
// - Function calls -> record_function_call (feeds Phase-2 transitive resolution).
// - Throw / raise / panic -> record_throw.
// - Go channel send -> record_channel_op; defer -> record_defer.
//
// Only runs when a sink is attached (mcp side-effect pass); the guard in
// visit_node keeps the hot indexing path free of this work.
// ---------------------------------------------------------------------------

namespace {

// Field-name lookup shorthand (tree-sitter wants the byte length).
TSNode field(TSNode node, const char* name) {
    return ts_node_child_by_field_name(
        node, name, static_cast<uint32_t>(std::strlen(name)));
}

// Identifier-shaped node types across the grammars we register params from.
bool is_identifier_type(std::string_view t) {
    return t == "identifier" || t == "simple_identifier" ||
           t == "field_identifier" || t == "property_identifier" ||
           t == "shorthand_property_identifier" || t == "variable_name" ||
           t == "name" || t == "dotted_name";
}

int line_of(TSNode node) {
    return static_cast<int>(ts_node_start_point(node).row) + 1;
}
int col_of(TSNode node) {
    return static_cast<int>(ts_node_start_point(node).column) + 1;
}

// Case-insensitive ASCII prefix match (local; the analyzer keeps its own).
bool iprefix(std::string_view name, std::string_view prefix) {
    if (name.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(name[i])));
        if (a != prefix[i]) return false;
    }
    return true;
}

// Log/report-category callee (bare last segment): the log-and-swallow
// signal. Two shapes qualify: a logging verb (log, print, warn, ...) or a
// logger's level method (`log.error`, `logger.fatal`), and a REPORTER — a
// handling verb compounded with "error"/"exception" (checkApiError,
// handleError, reportError, captureException) — which surfaces the error
// to a human, same credit tier as logging.
//
// An "error" substring on its own does NOT qualify. getTRPCErrorFromUnknown
// wraps, isAbortError tests, errors.push collects, opts.onError forwards;
// reading each as a log made every one of them a swallow (trpc calibration).
// Log severity of a callee, for the level=<x> annotation on log-and-swallow
// findings. "print" = an unleveled sink (console.log, puts, print) — the
// production-visibility question ("will this line even ship?") is the point,
// so debug/info/print are kept distinct from warn/error rather than folded.
std::string_view level_of_log_callee(std::string_view callee) {
    for (std::string_view p : {"fatal", "critical", "severe", "error", "err",
                               "exception"}) {
        if (iprefix(callee, p)) return "error";
    }
    if (iprefix(callee, "warn")) return "warn";
    if (iprefix(callee, "info") || iprefix(callee, "notice")) return "info";
    for (std::string_view p : {"debug", "trace", "verbose", "fine"}) {
        if (iprefix(callee, p)) return "debug";
    }
    // console.error / logger.error arrive as bare "error" handled above;
    // what is left (log, print, puts, console, captureException-style
    // reporters) carries no level of its own.
    return "print";
}

// Most-severe-wins ordering for note_log_level.
int log_level_rank(std::string_view level) {
    if (level == "error") return 4;
    if (level == "warn") return 3;
    if (level == "info") return 2;
    if (level == "debug") return 1;
    return 0;  // "print"
}

void note_log_level(CatchSiteInfo& site, std::string_view level) {
    if (site.log_level.empty() ||
        log_level_rank(level) > log_level_rank(site.log_level)) {
        site.log_level = std::string(level);
    }
}

bool is_log_callee(std::string_view callee) {
    static constexpr std::string_view log_prefixes[] = {
        "log", "print", "puts", "warn", "debug", "trace", "console"};
    for (auto p : log_prefixes) {
        if (iprefix(callee, p)) return true;
    }
    static constexpr std::string_view level_methods[] = {
        "error", "err", "fatal", "critical", "exception", "info"};
    for (auto m : level_methods) {
        if (callee.size() == m.size() && iprefix(callee, m)) return true;
    }
    bool names_error = false;
    for (size_t i = 0; i + 5 <= callee.size() && !names_error; ++i) {
        if (iprefix(callee.substr(i), "error") ||
            iprefix(callee.substr(i), "exception")) {
            names_error = true;
        }
    }
    if (!names_error) return false;
    // Not "handle": handleError / handle_exception!(e) DISPATCH the error
    // (sinatra's dispatch! hands it to the app's error blocks); that is
    // propagation and the fidelity walk credits it as such.
    static constexpr std::string_view reporter_verbs[] = {
        "report", "check", "capture", "notify", "record", "track",
        "show", "display", "alert", "present"};
    for (auto v : reporter_verbs) {
        if (iprefix(callee, v)) return true;
    }
    return false;
}

// A qualifier that makes the whole call a log regardless of the method name:
// `console.x`, `log.x`, `logger.x`. An `errors.push(e)` qualifier is a
// collection, not a logger, so the reporter rule above is not consulted here.
bool is_log_qualifier(std::string_view qualifier) {
    auto dot = qualifier.rfind('.');
    std::string_view last = dot == std::string_view::npos
                                ? qualifier
                                : qualifier.substr(dot + 1);
    static constexpr std::string_view q[] = {"log", "console", "print",
                                             "warn", "debug", "trace"};
    for (auto p : q) {
        if (iprefix(last, p)) return true;
    }
    return false;
}

// A caught type naming the NORMAL end of a protocol rather than a failure:
// a read timing out where the timeout is the keepalive trigger, a stream
// ending, a peer disconnecting, a task being cancelled. The handler IS the
// protocol, so none of the swallow signals apply. Substring on purpose —
// `anyio.EndOfStream`, `asyncio.TimeoutError`, `WebSocketDisconnect`,
// `ThreadInterruptedException` all spell the condition inside a longer name.
bool header_names_a_normal_condition(std::string_view header) {
    static constexpr std::string_view conditions[] = {
        "Timeout", "Disconnect", "EndOfStream", "EOFError", "StopIteration",
        "StopAsyncIteration", "GeneratorExit", "Cancel", "Abort",
        "Interrupt", "BrokenPipe", "ClosedResource"};
    for (auto c : conditions) {
        if (header.find(c) != std::string_view::npos) return true;
    }
    return false;
}

// Splits `recv.method`, `ptr->method`, `ns::fn` into receiver and bare name.
// The receiver is its LAST segment (`db` in `ctx.db.save`). Distinct from the
// resolver's '.'-only split: C++'s `update_cmd->add_flag` otherwise reads as
// a single callee beginning with "update".
struct CalleeParts {
    std::string_view qualifier;
    std::string_view bare;
};
CalleeParts split_callee(std::string_view callee) {
    size_t cut = std::string_view::npos;
    size_t sep_len = 0;
    for (auto [sep, len] : {std::pair{std::string_view("."), size_t{1}},
                            std::pair{std::string_view("->"), size_t{2}},
                            std::pair{std::string_view("::"), size_t{2}}}) {
        size_t at = callee.rfind(sep);
        if (at != std::string_view::npos &&
            (cut == std::string_view::npos || at > cut)) {
            cut = at;
            sep_len = len;
        }
    }
    if (cut == std::string_view::npos || cut + sep_len >= callee.size()) {
        return {{}, callee};
    }
    std::string_view qualifier = callee.substr(0, cut);
    auto q = split_callee(qualifier);
    return {q.bare, callee.substr(cut + sep_len)};
}

// Throw-shaped node inside a catch body, across the grammars we classify.
bool is_throw_node(std::string_view t) {
    return t == "throw_statement" || t == "throw_expression" ||
           t == "raise_statement" || t == "throw";
}

bool is_call_node(std::string_view t) {
    return t == "call_expression" || t == "call" || t == "method_invocation" ||
           t == "invocation_expression" || t == "function_call_expression" ||
           // `new TRPCError({ cause })` hands the error to a constructor.
           t == "new_expression" || t == "object_creation_expression" ||
           // PHP: `$deferred->reject($e)`, `Foo::bar($e)`, `$x?->f($e)`.
           t == "member_call_expression" || t == "scoped_call_expression" ||
           t == "nullsafe_member_call_expression";
}

// A store or a yield whose right-hand side is the caught error: the error
// leaves the block through a binding (`failure = e`, `result = [wrap(e)]`)
// or a generator (`yield format(e)`). Same exit as a callback, spelled
// differently.
bool is_assignment_node(std::string_view t) {
    return t == "assignment_expression" || t == "assignment" ||
           t == "augmented_assignment_expression" ||
           t == "variable_declarator" || t == "yield_expression" ||
           t == "yield";
}

}  // namespace

void UnifiedExtractor::record_lvalue_write(TSNode lvalue, int line, int column) {
    if (!side_effects_) return;
    // Assignment IS declaration in these languages: a bare `$x = 1` / `x = 1`
    // inside a function creates a LOCAL (writing a module global needs an
    // explicit `global` statement). Without registration every such write
    // classified as a global write — guzzle reported global_writes=725 with
    // zero `global` statements in the codebase (2026-08-26 re-panel).
    bool assignment_declares =
        ext_ == ".py" || ext_ == ".php" || ext_ == ".rb";
    TSNode n = lvalue;
    // Descend member / subscript / selector expressions to the base identifier
    // that owns the mutation (a.b.c = x mutates `a`; arr[i] = x mutates `arr`).
    for (int guard = 0; guard < 32 && !ts_node_is_null(n); ++guard) {
        std::string_view t = get_node_type(n);
        if (is_identifier_type(t)) {
            std::string_view id = node_text(n);
            // The blank identifier is a discard, not a state mutation
            // (`_ = k` classified as a GLOBAL write).
            if (id == "_") return;
            if (!id.empty()) {
                // Only a BARE lvalue declares (guard==0: no member/subscript
                // was peeled — `a.b = x` mutates an existing object). Ruby
                // spells globals with a `$` sigil, so those stay global.
                if (assignment_declares && guard == 0 &&
                    !(ext_ == ".rb" && id.front() == '$')) {
                    side_effects_->add_local_variable(id, line);
                }
                side_effects_->record_access(id, {}, AccessType::Write, line,
                                             column);
            }
            return;
        }
        if (ts_node_named_child_count(n) == 0) {
            // Leaf we don't recognise as an identifier (e.g. `this`/`self`);
            // record its text so the analyzer can classify it as a receiver.
            std::string_view txt = node_text(n);
            if (!txt.empty())
                side_effects_->record_access(txt, {}, AccessType::Write, line,
                                             column);
            return;
        }
        n = ts_node_named_child(n, 0);
    }
}

void UnifiedExtractor::register_function_signature(TSNode node,
                                                   std::string_view node_type) {
    if (!side_effects_) return;
    (void)node_type;

    // Go method receiver: `func (r *T) M()` -> classify writes to `r` as
    // receiver mutations rather than global writes.
    TSNode receiver = field(node, "receiver");
    if (!ts_node_is_null(receiver)) {
        uint32_t n = ts_node_named_child_count(receiver);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode decl = ts_node_named_child(receiver, i);
            TSNode name = field(decl, "name");
            if (!ts_node_is_null(name)) {
                side_effects_->set_receiver(node_text(name), {});
                break;
            }
        }
    }

    // Go error-return: `func f() error` / `func f() (T, error)`. The result
    // field's text carrying the `error` type is the precise signature-level
    // signal that this function participates in Go error propagation.
    if (ext_ == ".go") {
        TSNode result = field(node, "result");
        if (!ts_node_is_null(result)) {
            std::string_view rt = node_text(result);
            if (rt.find("error") != std::string_view::npos) {
                side_effects_->record_error_return();
            }
        }
    }

    // Zig error-union return: `fn f() !void`.
    if (ext_ == ".zig") {
        std::string_view sig = node_text(node);
        auto paren = sig.find(')');
        auto brace = sig.find('{');
        if (paren != std::string_view::npos && brace != std::string_view::npos &&
            brace > paren &&
            sig.substr(paren, brace - paren).find('!') !=
                std::string_view::npos) {
            side_effects_->record_error_return();
        }
    }

    // Parameter list. Field name is "parameters" across Go / JS / TS / Python /
    // Java / C# / PHP function-and-method grammars.
    TSNode params = field(node, "parameters");
    if (ts_node_is_null(params)) return;

    int index = 0;
    uint32_t n = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < n; ++i) {
        TSNode param = ts_node_named_child(params, i);
        std::string_view t = get_node_type(param);
        // Skip punctuation-ish nodes that slipped into named children.
        if (t == "comment") continue;

        std::string_view pname;
        TSNode name = field(param, "name");
        if (!ts_node_is_null(name)) {
            pname = node_text(name);
        } else if (is_identifier_type(t)) {
            pname = node_text(param);
        } else {
            // Pattern / typed parameter: first identifier-shaped descendant.
            uint32_t cc = ts_node_named_child_count(param);
            for (uint32_t j = 0; j < cc; ++j) {
                TSNode c = ts_node_named_child(param, j);
                if (is_identifier_type(get_node_type(c))) {
                    pname = node_text(c);
                    break;
                }
            }
        }
        if (!pname.empty()) {
            side_effects_->add_parameter(pname, index);
        }
        ++index;
    }
}

void UnifiedExtractor::process_side_effect_node(
    TSNode node, std::string_view node_type) {
    // Only record inside a tracked function body.
    if (!side_effects_ || se_func_depth_ == 0) return;

    int line = line_of(node);
    int column = col_of(node);

    // Local declarations register the name so later writes classify as
    // Local, not Global (locals were registered NOWHERE, so every re-assigned
    // local counted as a global write across all languages — the systemic
    // purity inflation found in the 2026-08-26 re-panel). Writes to names
    // that stay unregistered (package/module-level variables) still classify
    // global, which is exactly right.
    auto register_decl_identifiers = [&](TSNode decl) {
        // Register every identifier in declarator position (covers Go
        // multi-assign lists and C++ declarator chains); never harvest names
        // out of initializer expressions.
        std::vector<TSNode> stack{decl};
        while (!stack.empty()) {
            TSNode c = stack.back();
            stack.pop_back();
            std::string_view ct = get_node_type(c);
            if (ct == "identifier" || ct == "simple_identifier" ||
                ct == "variable_name" ||
                // JS/TS destructuring patterns bind through their own node
                // kinds (`const {a, b} = x`).
                ct == "shorthand_property_identifier_pattern" ||
                ct == "shorthand_property_identifier") {
                auto id = node_text(c);
                if (!id.empty())
                    side_effects_->add_local_variable(id, line_of(c));
                continue;
            }
            if (ct == "call_expression" || ct == "binary_expression" ||
                ct == "argument_list" || ct == "initializer")
                continue;
            uint32_t nc = ts_node_named_child_count(c);
            for (uint32_t i = 0; i < nc; ++i)
                stack.push_back(ts_node_named_child(c, i));
        }
    };
    if (node_type == "short_var_declaration" ||
        node_type == "range_clause") {
        // Go `x, y := f()` / `for k, v := range m` — left side only. Range
        // bindings were unregistered, so later writes through them counted
        // as global writes.
        TSNode left = field(node, "left");
        if (!ts_node_is_null(left)) register_decl_identifiers(left);
    } else if (node_type == "foreach_statement" ||   // PHP foreach bindings
               node_type == "catch_clause") {        // caught variable
        // Register the header's bound names (never the body): direct
        // variable_name / identifier children before the body block. In PHP
        // a bare $var inside a function is always local, so over-registering
        // the collection variable is harmless.
        uint32_t n = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode c = ts_node_named_child(node, i);
            std::string_view ct = get_node_type(c);
            // Stop at the body (defined below in this file; block-shaped).
            if (ct == "compound_statement" || ct == "block" ||
                ct == "statement_block" || ct == "colon_block")
                break;
            if (ct == "variable_name" || ct == "identifier") {
                auto id = node_text(c);
                if (!id.empty())
                    side_effects_->add_local_variable(id, line_of(c));
            } else if (ct == "catch_formal_parameter" ||
                       ct == "pair" || ct == "by_ref") {
                register_decl_identifiers(c);
            }
        }
    } else if (node_type == "for_in_statement" ||    // JS for..in/of
               node_type == "for_statement") {       // Python for target
        TSNode left = field(node, "left");
        if (!ts_node_is_null(left)) register_decl_identifiers(left);
    } else if (node_type == "let_declaration") {     // Rust let bindings
        TSNode pat = field(node, "pattern");
        if (!ts_node_is_null(pat)) register_decl_identifiers(pat);
    } else if (node_type == "var_spec" ||                    // Go var
               node_type == "variable_declarator" ||         // JS/TS/Java/C#
               node_type == "init_declarator" ||             // C/C++
               node_type == "variable_declaration") {        // Kotlin
        // Name field when the grammar provides one; otherwise the first
        // identifier child is the declared name.
        TSNode nm = field(node, "name");
        if (!ts_node_is_null(nm)) {
            if (is_identifier_type(get_node_type(nm))) {
                auto id = node_text(nm);
                if (!id.empty()) side_effects_->add_local_variable(id, line);
            } else {
                register_decl_identifiers(nm);
            }
        } else {
            TSNode decl = field(node, "declarator");
            if (!ts_node_is_null(decl)) {
                register_decl_identifiers(decl);
            } else {
                uint32_t n = ts_node_named_child_count(node);
                for (uint32_t i = 0; i < n; ++i) {
                    TSNode c = ts_node_named_child(node, i);
                    if (is_identifier_type(get_node_type(c))) {
                        auto id = node_text(c);
                        if (!id.empty())
                            side_effects_->add_local_variable(id, line_of(c));
                        break;  // first identifier is the declared name
                    }
                }
            }
        }
    }

    // Assignment patterns - writes to state.
    if (node_type == "assignment_expression" ||
        node_type == "assignment_statement" || node_type == "assignment") {
        if (ext_ == ".go") process_go_error_drop(node, node_type);
        TSNode left = field(node, "left");
        if (ts_node_is_null(left)) left = ts_node_named_child(node, 0);
        if (ts_node_is_null(left)) return;
        // Go multi-assign: `a, b = f()` -> left is an expression_list.
        if (get_node_type(left) == "expression_list") {
            uint32_t n = ts_node_named_child_count(left);
            for (uint32_t i = 0; i < n; ++i) {
                record_lvalue_write(ts_node_named_child(left, i), line, column);
            }
        } else {
            record_lvalue_write(left, line, column);
        }
        return;
    }

    if (node_type == "augmented_assignment_expression" ||
        node_type == "augmented_assignment" ||
        node_type == "compound_assignment_expr") {
        TSNode left = field(node, "left");
        if (ts_node_is_null(left)) left = ts_node_named_child(node, 0);
        if (!ts_node_is_null(left)) record_lvalue_write(left, line, column);
        return;
    }

    // Go-specific side effects.
    if (ext_ == ".go") {
        if (node_type == "send_statement") {
            side_effects_->record_channel_op(line);
            return;
        }
        if (node_type == "defer_statement") {
            side_effects_->record_defer();
            return;
        }
        if (node_type == "inc_statement" || node_type == "dec_statement") {
            TSNode t = ts_node_named_child(node, 0);
            if (!ts_node_is_null(t)) record_lvalue_write(t, line, column);
            return;
        }
        if (node_type == "short_var_declaration") {
            process_go_error_drop(node, node_type);
        }
    }

    // `return <expr>` — the function hands a value to its caller (factory
    // suppression for leak-no-release).
    if (ts_node_is_named(node) &&
        (node_type == "return_statement" || node_type == "return_expression") &&
        ts_node_named_child_count(node) > 0) {
        side_effects_->record_return_value();
        return;
    }

    // Catch/except/rescue sites (swallow detection). Named-node gate: the
    // grammar's anonymous `rescue`/`ensure` keyword tokens carry the same
    // type string as the clause node and must not double-fire.
    if (ts_node_is_named(node) &&
        (node_type == "catch_clause" || node_type == "except_clause" ||
         node_type == "rescue" || node_type == "catch_block")) {
        process_catch_site(node, node_type);
        return;
    }

    // try/finally (and Ruby ensure, Kotlin finally_block).
    if (ts_node_is_named(node) &&
        (node_type == "finally_clause" || node_type == "ensure" ||
         node_type == "finally_block")) {
        side_effects_->record_try_finally();
        return;
    }

    // Kotlin: throw is a jump_expression whose first token is `throw`.
    if (ext_ == ".kt" && node_type == "jump_expression") {
        std::string_view txt = node_text(node);
        if (iprefix(txt, "throw")) {
            side_effects_->record_throw({}, line, column);
        }
        return;
    }

    // Zig error propagation / cleanup guards.
    if (ext_ == ".zig") {
        if (node_type == "try_expression" || node_type == "try") {
            side_effects_->record_error_return(line);
            return;
        }
        if (node_type == "errdefer" || node_type == "errdefer_statement") {
            side_effects_->record_defer();
            return;
        }
    }

    // JavaScript/TypeScript-specific.
    if (ext_ == ".js" || ext_ == ".jsx" || ext_ == ".ts" || ext_ == ".tsx") {
        if (node_type == "update_expression") {
            TSNode arg = field(node, "argument");
            if (ts_node_is_null(arg)) arg = ts_node_named_child(node, 0);
            if (!ts_node_is_null(arg)) record_lvalue_write(arg, line, column);
            return;
        }
        if (node_type == "delete_expression") {
            TSNode arg = ts_node_named_child(node, 0);
            if (!ts_node_is_null(arg)) record_lvalue_write(arg, line, column);
            return;
        }
        // await_expression / yield_expression: async markers with no dedicated
        // recorder in the analyzer. Deferred (kept conservative) - the call
        // they wrap is still recorded via call_expression below.
    }

    // Python-specific.
    if (ext_ == ".py") {
        if (node_type == "raise_statement") {
            side_effects_->record_throw({}, line, column);
            return;
        }
        if (node_type == "delete_statement") {
            uint32_t n = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < n; ++i) {
                record_lvalue_write(ts_node_named_child(node, i), line, column);
            }
            return;
        }
    }

    // Rust-specific: panic-family macros are precise throw sites.
    if (ext_ == ".rs") {
        if (node_type == "macro_invocation") {
            TSNode name_node = field(node, "macro");
            if (!ts_node_is_null(name_node)) {
                std::string_view macro_name = node_text(name_node);
                if (macro_name == "panic" || macro_name == "unreachable" ||
                    macro_name == "unimplemented" || macro_name == "todo") {
                    side_effects_->record_throw(macro_name, line, column);
                    return;
                }
            }
        }
    }

    // Throw statements (Java / C# / C / C++ / Kotlin) and PHP throw
    // expressions plus the universal fallback (JS `throw`, etc.).
    if (node_type == "throw_statement" || node_type == "throw_expression") {
        side_effects_->record_throw({}, line, column);
        return;
    }

    // Call expressions - record the callee for Phase-2 transitive resolution.
    if (node_type == "call_expression" || node_type == "call" ||
        node_type == "call_statement") {
        TSNode func = field(node, "function");
        if (ts_node_is_null(func)) func = field(node, "name");
        if (ts_node_is_null(func)) func = field(node, "method");  // Ruby
        if (ts_node_is_null(func)) func = ts_node_named_child(node, 0);
        if (ts_node_is_null(func)) return;

        std::string_view callee = node_text(func);
        if (callee.empty()) return;

        // Go `panic(...)` / Ruby `raise ...` are precise throw sites, not
        // ordinary calls.
        if (ext_ == ".go" && callee == "panic") {
            side_effects_->record_throw("panic", line, column);
            return;
        }
        if (ext_ == ".rb" && callee == "raise") {
            side_effects_->record_throw({}, line, column);
            return;
        }

        // Qualified `pkg.Fn` / `recv.method` -> split into qualifier + name so
        // the resolver can match the method by receiver.
        auto dot = callee.rfind('.');
        std::string_view bare =
            (dot != std::string_view::npos && dot + 1 < callee.size())
                ? callee.substr(dot + 1)
                : callee;
        if (bare.data() != callee.data()) {
            side_effects_->record_function_call(bare, callee.substr(0, dot),
                                                /*is_method=*/true, line,
                                                column);
        } else {
            side_effects_->record_function_call(callee, {}, /*is_method=*/false,
                                                line, column);
        }

        // Resource acquire/release pairing: classify the bare callee; a call
        // under a defer/finally/ensure/using/with scope carries guard credit.
        auto parts = split_callee(callee);
        side_effects_->record_call_site_resources(
            parts.bare, line, se_guard_depth_ > 0, se_branch_id_,
            parts.qualifier);
        return;
    }
}

// ---------------------------------------------------------------------------
// Cleanup-guard scopes
// ---------------------------------------------------------------------------

bool UnifiedExtractor::is_se_guard_node(std::string_view node_type) const {
    if (node_type == "finally_clause" || node_type == "finally_block")
        return true;                                     // JS/TS/Java/C#/PHP/Py/Kotlin
    if (node_type == "ensure") return true;              // Ruby
    if (node_type == "with_statement") return true;      // Python
    if (node_type == "using_statement") return true;     // C#
    if (node_type == "resource_specification") return true;  // Java try-with
    if (node_type == "defer_statement") return true;     // Go / Zig
    if (node_type == "errdefer" || node_type == "errdefer_statement")
        return true;                                     // Zig
    return false;
}

// True for a node that is ONE ARM of a multi-way choice. Two state changes in
// different arms never both execute, so they cannot leave each other
// half-applied — the undo-cost rules group ops by arm because of this.
//
// A plain `if` consequence is deliberately absent: `if (x) { save(a); }
// save(b);` really is a sequence. Only constructs whose arms are mutually
// exclusive by construction count.
bool UnifiedExtractor::is_se_branch_node(std::string_view node_type) const {
    return node_type == "switch_case" ||          // JS/TS/Java/C#/PHP
           node_type == "switch_default" ||
           node_type == "case_clause" ||
           node_type == "expression_case" ||      // Go
           node_type == "default_case" ||
           node_type == "communication_case" ||   // Go select
           node_type == "type_case" ||            // Go type switch
           node_type == "case_statement" ||       // C/C++/Ruby
           node_type == "when_entry" ||           // Kotlin
           node_type == "when_clause" ||          // Ruby
           node_type == "match_arm" ||            // Rust
           node_type == "case" ||                 // Python match
           node_type == "case_clause_body" ||
           node_type == "else_clause" ||          // if/else arms
           node_type == "elif_clause" ||
           node_type == "else_if_clause" ||
           node_type == "alternative" ||          // Python/Ruby else
           // Exception-handler clauses: at most one arm runs per raise, so
           // sibling handlers are alternatives exactly like switch cases.
           // rack's method_override_param writes one line to the error
           // stream in each of two rescue arms and read as a 2-change torn
           // write until these joined the list.
           node_type == "rescue" ||               // Ruby
           node_type == "except_clause" ||        // Python
           node_type == "except_group_clause" ||  // Python 3.11 except*
           node_type == "catch_clause" ||         // JS/TS/Java/C#/C++/PHP
           node_type == "catch_block";            // Kotlin
}

// ---------------------------------------------------------------------------
// Catch-site classification (swallow detection)
// ---------------------------------------------------------------------------

namespace {

// Depth-first walk of `root`'s subtree calling fn(node, type); fn returns
// false to stop early.
template <typename Fn>
void walk_subtree(TSNode root, Fn&& fn) {
    struct Frame { TSNode n; };
    std::vector<TSNode> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        TSNode n = stack.back();
        stack.pop_back();
        if (!fn(n)) return;
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = c; i > 0; --i) {
            stack.push_back(ts_node_named_child(n, i - 1));
        }
    }
}

bool is_body_node(std::string_view t) {
    return t == "statement_block" || t == "block" || t == "compound_statement" ||
           t == "then" || t == "body_statement";
}

bool is_comment_node(std::string_view t) {
    return t == "comment" || t == "line_comment" || t == "block_comment";
}

// A returned expression that carries NO information about the failure: the
// error became "no result". Text-based on purpose — the spellings are few and
// identical across grammars, and matching node types would need a table per
// language for no extra precision.
bool is_sentinel_expression(std::string_view text) {
    // Trim whitespace so `return  null ;` matches.
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    if (!text.empty() && text.back() == ';') text.remove_suffix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);

    for (std::string_view s :
         {"null", "nullptr", "None", "nil", "undefined", "false", "0", "-1",
          "\"\"", "''", "[]", "{}", "()", "list()", "dict()", "set()",
          "Optional.empty()", "None,", "empty"}) {
        if (text == s) return true;
    }
    return false;
}

// The member/method names that keep an error's full diagnostic payload. A
// projection NOT in this list, taken on a language whose errors carry more
// than a message, is lossy.
bool is_full_fidelity_accessor(std::string_view name, std::string_view ext) {
    // Stack and cause chains, every language that has them.
    for (std::string_view full :
         {"stack", "stacktrace", "getstacktrace", "printstacktrace",
          "cause", "getcause", "innerexception", "getsuppressed",
          "__traceback__", "with_traceback", "format_exc", "print_exc",
          "format_exception", "tb_frame", "backtrace", "full_message"}) {
        if (name == full) return true;
    }
    // .NET's ToString() renders message + stack + every InnerException, so it
    // loses nothing. JS's toString() is "Error: msg" — same spelling, lossy,
    // so it falls through to is_message_accessor below.
    if (name == "tostring" &&
        (ext == ".cs" || ext == ".fs" || ext == ".vb")) {
        return true;
    }
    // Python's traceback module renders the chain; Ruby's full_message too.
    return false;
}

// Projections that reduce an error to its message. Only meaningful where the
// language's error type carries more than that.
bool is_message_accessor(std::string_view name) {
    for (std::string_view msg :
         {"message", "msg", "getmessage", "getlocalizedmessage", "what",
          "tostring", "str", "string", "description", "reason", "detail",
          "error", "geterrormessage", "localizeddescription"}) {
        if (name == msg) return true;
    }
    return false;
}

// True when this language's caught error carries NOTHING beyond its message,
// so a message projection loses nothing that ever existed.
//
// Go's `error` is an interface whose whole contract is `Error() string` — no
// stack, no cause, unless the code wrapped with %w. C++'s std::exception is
// `what()` and nothing else. Calling those lossy would demand data the
// language never produced.
bool errors_are_message_only(std::string_view ext) {
    return ext == ".go" || ext == ".c" || ext == ".cc" || ext == ".cpp" ||
           ext == ".cxx" || ext == ".h" || ext == ".hpp" || ext == ".zig";
}

}  // namespace


// How much of the caught error reaches this call's arguments. Walks the
// argument region looking for the caught identifier, then asks what was taken
// FROM it: the bare binding is the whole error, `e.stack` keeps the payload,
// `e.message` keeps a sentence.
// A `return` (or `throw`) inside finally/ensure DISCARDS whatever exception
// was propagating through it — in JS, Java, C#, Python and Ruby alike. There
// is no catch site, so nothing in the source marks the deletion; this is the
// one error-handling defect with no visible handler to read.
//
// Only the finally's OWN control flow counts: a return inside a nested
// function literal in the block returns from that function, not through the
// finally, so nested function bodies are not descended into.
void UnifiedExtractor::check_finally_hijack(TSNode node) {
    if (side_effects_ == nullptr) return;
    bool found = false;
    walk_subtree(node, [&](TSNode n) {
        if (found) return false;
        std::string_view t = get_node_type(n);
        if (!ts_node_eq(n, node) && is_function_node(t)) {
            return false;  // a nested function's return is its own
        }
        if (t == "return_statement" || t == "return_expression") {
            side_effects_->record_finally_hijack(line_of(n), "return");
            found = true;
            return false;
        }
        if (is_throw_node(t)) {
            side_effects_->record_finally_hijack(line_of(n), "throw");
            found = true;
            return false;
        }
        return true;
    });
}

CauseFidelity UnifiedExtractor::cause_fidelity(TSNode args,
                                              std::string_view caught_var) {
    CauseFidelity best = CauseFidelity::None;
    walk_subtree(args, [&](TSNode a) {
        if (!is_identifier_type(get_node_type(a)) ||
            node_text(a) != caught_var) {
            return true;
        }
        // What encloses the identifier decides the verdict. A bare argument
        // has no accessor above it, so nothing was projected away.
        CauseFidelity here = CauseFidelity::Full;
        TSNode parent = ts_node_parent(a);
        if (!ts_node_is_null(parent)) {
            std::string_view pt = get_node_type(parent);
            bool is_access = pt == "member_expression" ||
                             pt == "field_expression" ||
                             pt == "attribute" || pt == "selector_expression" ||
                             pt == "navigation_expression" ||
                             pt == "member_access_expression" ||
                             pt == "scoped_identifier" ||
                             pt == "call" || pt == "method_invocation" ||
                             pt == "call_expression";
            if (is_access) {
                // The accessor name is the text after the caught variable.
                std::string_view whole = node_text(parent);
                std::string accessor;
                if (auto dot = whole.rfind('.'); dot != std::string_view::npos) {
                    accessor = std::string(whole.substr(dot + 1));
                }
                // A conversion wrapping the error — str(e), String(e) — reads
                // as the callee name instead.
                if (accessor.empty() &&
                    (pt == "call" || pt == "call_expression")) {
                    TSNode fn = field(parent, "function");
                    if (ts_node_is_null(fn)) fn = ts_node_named_child(parent, 0);
                    if (!ts_node_is_null(fn) && node_text(fn) != caught_var) {
                        accessor = std::string(node_text(fn));
                    }
                }
                // Trim a trailing "()" and lowercase for the tables.
                if (auto paren = accessor.find('('); paren != std::string::npos) {
                    accessor.resize(paren);
                }
                for (auto& c : accessor) {
                    c = static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c)));
                }
                if (!accessor.empty()) {
                    if (is_full_fidelity_accessor(accessor, ext_)) {
                        here = CauseFidelity::Full;
                    } else if (is_message_accessor(accessor)) {
                        // Where the error type IS its message, taking the
                        // message loses nothing that ever existed.
                        here = errors_are_message_only(ext_)
                                   ? CauseFidelity::Full
                                   : CauseFidelity::Lossy;
                    }
                    // An unrecognized accessor (e.g. a domain field) is not
                    // claimed either way; treat it as the whole error rather
                    // than inventing a loss.
                }
            }
        }
        if (here > best) best = here;
        return best != CauseFidelity::Full;  // cannot improve on Full
    });
    return best;
}

void UnifiedExtractor::process_catch_site(TSNode node,
                                          std::string_view node_type) {
    CatchSiteInfo site;
    site.line = line_of(node);

    // Split the clause into header (caught type / variable) and body.
    TSNode body = field(node, "body");
    if (ts_node_is_null(body)) {
        // Fall back: last named child that is a block-shaped node; for Python
        // except_clause / Ruby rescue the block is the last named child.
        uint32_t n = ts_node_named_child_count(node);
        for (uint32_t i = n; i > 0; --i) {
            TSNode c = ts_node_named_child(node, i - 1);
            if (is_body_node(get_node_type(c))) {
                body = c;
                break;
            }
        }
        if (ts_node_is_null(body) && n > 0) {
            TSNode last = ts_node_named_child(node, n - 1);
            std::string_view lt = get_node_type(last);
            // Ruby's rescue carries its header as named children
            // (`exceptions`, `exception_variable`); an empty rescue has
            // nothing else, and the header is not a body.
            if (!is_comment_node(lt) && !is_identifier_type(lt) &&
                lt != "exceptions" && lt != "exception_variable" &&
                lt != "user_type") {
                body = last;
            }
        }
        // Kotlin: a catch whose only content is the anonymous `null` token
        // (`catch (e: E) { null }`) has no statements node at all. The
        // block's value is the sentinel; it is not an empty catch.
        if (ts_node_is_null(body) && ext_ == ".kt") {
            uint32_t nc = ts_node_child_count(node);
            for (uint32_t i = 0; i < nc; ++i) {
                TSNode c = ts_node_child(node, i);
                if (!ts_node_is_named(c) && is_sentinel_expression(node_text(c))) {
                    site.has_return = true;
                    site.returns_sentinel = true;
                }
            }
        }
    }

    // Header text = clause text before the body (or whole clause when there is
    // no body at all — an empty rescue). Broad-type + caught-type come from it.
    std::string_view clause_text = node_text(node);
    size_t header_len = clause_text.size();
    if (!ts_node_is_null(body)) {
        uint32_t body_off = ts_node_start_byte(body) - ts_node_start_byte(node);
        if (body_off < header_len) header_len = body_off;
    }
    std::string_view header = clause_text.substr(0, header_len);

    // Whole-word match: `Exception` is broad, `IOException` is not.
    auto header_word = [&](std::string_view word) {
        size_t pos = 0;
        while ((pos = header.find(word, pos)) != std::string_view::npos) {
            bool start_ok =
                pos == 0 || !std::isalnum(static_cast<unsigned char>(
                                header[pos - 1]));
            size_t end = pos + word.size();
            bool end_ok =
                end >= header.size() ||
                !std::isalnum(static_cast<unsigned char>(header[end]));
            if (start_ok && end_ok) return true;
            pos = end;
        }
        return false;
    };
    auto header_has = [&](std::string_view needle) {
        return header.find(needle) != std::string_view::npos;
    };
    if (header_word("Throwable")) {
        site.broad_type = true;
        site.caught_type = "Throwable";
    } else if (header_word("Exception")) {
        site.broad_type = true;
        site.caught_type = "Exception";
    } else if (header_has("...")) {
        site.broad_type = true;
        site.caught_type = "...";
    } else if (node_type == "except_clause" &&
               ts_node_named_child_count(node) <= 1) {
        site.broad_type = true;  // bare `except:`
        site.caught_type = "bare except";
    } else if (header_names_a_normal_condition(header)) {
        site.normal_condition = true;
    }
    // Caught variable name (for rethrow-uses-cause): field-based per grammar.
    std::string_view caught_var;
    {
        TSNode p = field(node, "parameter");  // JS/TS
        if (ts_node_is_null(p)) p = field(node, "variable");  // Ruby =>
        // Ruby wraps the variable: `exception_variable > identifier`.
        if (!ts_node_is_null(p) && get_node_type(p) == "exception_variable" &&
            ts_node_named_child_count(p) > 0) {
            p = ts_node_named_child(p, 0);
        }
        if (!ts_node_is_null(p) && is_identifier_type(get_node_type(p))) {
            caught_var = node_text(p);
        } else {
            // Java/C#/Python/Kotlin: last identifier in the header region
            // (`catch (Exception e)`, `except E as e`, `catch (e: E)`).
            walk_subtree(node, [&](TSNode n) {
                if (!ts_node_is_null(body) &&
                    ts_node_start_byte(n) >= ts_node_start_byte(body))
                    return false;
                std::string_view t = get_node_type(n);
                // PHP's `$e` is a variable_name.
                if (((t == "identifier" || t == "simple_identifier") &&
                     ts_node_named_child_count(n) == 0) ||
                    t == "variable_name") {
                    caught_var = node_text(n);
                }
                return true;
            });
        }
    }
    // The header's non-keyword words. One word is a type with no variable
    // (`except NameError:`, `catch (Exception)`); the fallback scan above
    // would have taken it as the variable. Two are type and variable.
    {
        std::vector<std::string_view> words;
        size_t i = 0;
        while (i < header.size()) {
            if (!(std::isalnum(static_cast<unsigned char>(header[i])) ||
                  header[i] == '_' || header[i] == '$')) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < header.size() &&
                   (std::isalnum(static_cast<unsigned char>(header[j])) ||
                    header[j] == '_' || header[j] == '.' || header[j] == '$' ||
                    (header[j] == ':' && j + 1 < header.size() &&
                     header[j + 1] == ':'))) {
                j += header[j] == ':' ? 2 : 1;
            }
            std::string_view word = header.substr(i, j - i);
            i = j;
            bool keyword = false;
            for (std::string_view k : {"catch", "except", "rescue", "as",
                                       "const", "final", "var", "val",
                                       "when", "in", "if", "_"}) {
                if (word == k) keyword = true;
            }
            if (!keyword) words.push_back(word);
        }
        // JS/TS bind the variable through a grammar field and carry no
        // type, so a lone word there IS the variable.
        bool var_from_field = false;
        {
            TSNode p = field(node, "parameter");
            var_from_field = !ts_node_is_null(p);
        }
        if (words.size() == 1 && !var_from_field && words[0] == caught_var) {
            caught_var = {};
        }
        if (!site.broad_type && !site.normal_condition) {
            for (auto w : words) {
                if (w == caught_var || w.size() == 1) continue;
                site.specific_type = true;
                site.caught_type = std::string(w);
                break;
            }
        }
    }
    // `catch (NumberFormatException ignored)`, `catch (_: Exception)`: the
    // variable's name is the developer's own verdict, and the convention
    // IntelliJ, detekt and checkstyle all honor.
    for (std::string_view name : {"_", "ignored", "ignore", "unused",
                                  "expected", "ignoreexception"}) {
        if (caught_var.size() == name.size() && iprefix(caught_var, name)) {
            site.explicit_discard = true;
        }
    }

    // Body classification.
    if (ts_node_is_null(body)) {
        site.body_empty = !site.returns_sentinel;
    } else {
        int stmts = 0;
        uint32_t nkids = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < nkids; ++i) {
            std::string_view t = get_node_type(ts_node_named_child(body, i));
            if (is_comment_node(t)) continue;
            if (t == "pass_statement") continue;  // Python `pass` = empty
            ++stmts;
        }
        site.body_empty = stmts == 0;

        // Ruby's implicit return: the rescue body's last expression is the
        // method's value. `rescue Exception; @log` surfaces a value and
        // `rescue E; nil` a sentinel, and neither is a return node.
        // Kotlin's try is an expression too: `catch (e: E) { null }`.
        // Kotlin's `null` is an anonymous token, so the last child of any
        // kind is inspected by text.
        if ((ext_ == ".rb" || ext_ == ".kt") && ts_node_child_count(body) > 0) {
            TSNode last = ts_node_child(body, ts_node_child_count(body) - 1);
            // Skip the closing brace / trailing comments.
            for (uint32_t i = ts_node_child_count(body); i > 0; --i) {
                TSNode c = ts_node_child(body, i - 1);
                std::string_view ct = get_node_type(c);
                if (ct == "}" || is_comment_node(ct)) continue;
                last = c;
                break;
            }
            std::string_view lt = get_node_type(last);
            // Kotlin wraps the block's statements.
            if (lt == "statements" && ts_node_named_child_count(last) > 0) {
                last = ts_node_named_child(
                    last, ts_node_named_child_count(last) - 1);
                lt = get_node_type(last);
            }
            std::string_view text = node_text(last);
            if (is_sentinel_expression(text)) {
                site.has_return = true;
                site.returns_sentinel = true;
            } else if (lt == "instance_variable" || lt == "identifier" ||
                       lt == "simple_identifier" || lt == "constant" ||
                       lt == "true" || lt == "integer" || lt == "string" ||
                       lt == "array" || lt == "hash" ||
                       lt == "boolean_literal" || lt == "integer_literal" ||
                       lt == "string_literal") {
                site.has_return = true;
            }
            // An anonymous `null` is a value, not an empty body.
            if (site.has_return) site.body_empty = false;
        }

        // Java's chain-after-construct idiom: `npe.initCause(e); throw npe;`
        // (RxJava spells every subscribe() this way — ~690 of its 699
        // round-5 findings). The throw statement itself never mentions the
        // caught variable, so the mention check below cannot see the chain;
        // a body-level pre-scan for an initCause call that receives the
        // caught variable stands in for it.
        // A fatal-guard call (`Exceptions.throwIfFatal(ex)` — RxJava,
        // Reactor) is a conditional rethrow: Errors and fatal exceptions
        // re-propagate with their cause. A broad catch built around one is
        // exactly as broad as its guard, so it reads as a cause-keeping
        // rethrow (687 of RxJava's round-5 findings were this shape).
        bool cause_chained = false;
        bool fatal_guarded = false;
        if (!caught_var.empty()) {
            walk_subtree(body, [&](TSNode n) {
                bool is_fatal_guard = false;
                if (is_identifier_type(get_node_type(n))) {
                    std::string_view id = node_text(n);
                    is_fatal_guard = id == "throwIfFatal" ||
                                     id == "rethrowIfFatal" ||
                                     id == "propagateIfFatal";
                    if (!is_fatal_guard && id != "initCause") return true;
                } else {
                    return true;
                }
                // identifier -> invocation carrying the argument list
                // (Java: method_invocation; Kotlin: navigation_expression
                // under call_expression).
                TSNode inv = ts_node_parent(n);
                if (!ts_node_is_null(inv) &&
                    ts_node_is_null(field(inv, "arguments"))) {
                    inv = ts_node_parent(inv);
                }
                if (ts_node_is_null(inv)) return true;
                TSNode args = field(inv, "arguments");
                if (ts_node_is_null(args)) return true;
                bool arg_is_cause = false;
                walk_subtree(args, [&](TSNode a) {
                    if (is_identifier_type(get_node_type(a)) &&
                        node_text(a) == caught_var) {
                        arg_is_cause = true;
                    }
                    return !arg_is_cause;
                });
                if (arg_is_cause) {
                    (is_fatal_guard ? fatal_guarded : cause_chained) = true;
                }
                return !(cause_chained && fatal_guarded);
            });
        }
        if (fatal_guarded) {
            site.has_rethrow = true;
            site.rethrow_uses_cause = true;
        }

        walk_subtree(body, [&](TSNode n) {
            std::string_view t = get_node_type(n);
            if (is_throw_node(t) ||
                (ext_ == ".kt" && t == "jump_expression" &&
                 iprefix(node_text(n), "throw"))) {
                site.has_rethrow = true;
                // A bare `raise` (Python re-raise) or a rethrow mentioning the
                // caught variable keeps the cause.
                bool mentions_var = false;
                if (!caught_var.empty()) {
                    walk_subtree(n, [&](TSNode m) {
                        if (is_identifier_type(get_node_type(m)) &&
                            node_text(m) == caught_var)
                            mentions_var = true;
                        return !mentions_var;
                    });
                }
                if (mentions_var || cause_chained ||
                    ts_node_named_child_count(n) == 0) {
                    site.rethrow_uses_cause = true;
                }
                return true;
            }
            if (ext_ == ".rb" && t == "call") {
                TSNode m = field(n, "method");
                if (!ts_node_is_null(m) && node_text(m) == "raise") {
                    site.has_rethrow = true;
                    site.rethrow_uses_cause = true;  // conservative for Ruby
                    return true;
                }
            }
            if (is_assignment_node(t) && !caught_var.empty()) {
                // Only the value side counts: `e = null` stores nothing.
                TSNode rhs = field(n, "right");
                if (ts_node_is_null(rhs)) rhs = field(n, "value");
                if (ts_node_is_null(rhs) && (t == "yield_expression" || t == "yield"))
                    rhs = n;
                if (!ts_node_is_null(rhs)) {
                    CauseFidelity fid = cause_fidelity(rhs, caught_var);
                    if (fid != CauseFidelity::None) {
                        site.propagates_cause = true;
                        if (fid > site.propagated_fidelity)
                            site.propagated_fidelity = fid;
                    }
                }
                return true;  // calls inside the RHS were just credited
            }
            if (t == "return_statement" || t == "return_expression") {
                if (ts_node_named_child_count(n) > 0) {
                    site.has_return = true;
                    // WHAT it returns decides whether the error survived.
                    // `return null` is not surfacing the failure, it is
                    // renaming it "no result".
                    TSNode v = ts_node_named_child(n, 0);
                    if (is_sentinel_expression(node_text(v))) {
                        site.returns_sentinel = true;
                    }
                }
                return true;
            }
            // C++ logs through a stream: `std::cerr << "..." << e.what()`.
            if (t == "binary_expression" && ext_ != ".py") {
                std::string_view text = node_text(n);
                if (text.find("<<") != std::string_view::npos &&
                    (text.compare(0, 9, "std::cerr") == 0 ||
                     text.compare(0, 9, "std::cout") == 0 ||
                     text.compare(0, 9, "std::clog") == 0 ||
                     text.compare(0, 4, "cerr") == 0 ||
                     text.compare(0, 4, "cout") == 0 ||
                     text.compare(0, 4, "clog") == 0 ||
                     text.compare(0, 3, "LOG") == 0 ||
                     text.compare(0, 3, "log") == 0)) {
                    site.has_log_call = true;
                    note_log_level(site, "print");  // streams carry no level
                    if (!caught_var.empty()) {
                        CauseFidelity fid = cause_fidelity(n, caught_var);
                        if (fid > site.logged_fidelity) site.logged_fidelity = fid;
                    }
                    return false;  // the nested << chain is this one log
                }
            }
            if (is_call_node(t)) {
                TSNode func = field(n, "function");
                if (ts_node_is_null(func)) func = field(n, "name");
                if (ts_node_is_null(func)) func = field(n, "method");
                if (ts_node_is_null(func)) func = ts_node_named_child(n, 0);
                std::string_view callee =
                    ts_node_is_null(func) ? std::string_view{} : node_text(func);
                auto parts = split_callee(callee);
                std::string_view bare = parts.bare;
                // `console.log` qualifies via its qualifier as well.
                bool log = is_log_callee(bare) ||
                           is_log_qualifier(parts.qualifier);
                if (ext_ == ".rb" && bare == "raise") return true;  // handled
                // How much of the caught error travels into this call?
                // Logging and propagation are tracked apart: `console.error(e)`
                // reports the error, it does not hand it to anyone, which is
                // what LogAndSwallow means — but the fidelity question ("did
                // the stack survive?") applies to both.
                CauseFidelity fid = CauseFidelity::None;
                if (!caught_var.empty()) {
                    TSNode args = field(n, "arguments");
                    if (ts_node_is_null(args)) args = field(n, "argument_list");
                    // Kotlin keeps them in call_suffix > value_arguments.
                    if (ts_node_is_null(args)) {
                        uint32_t nc = ts_node_named_child_count(n);
                        for (uint32_t i = 0; i < nc; ++i) {
                            TSNode c = ts_node_named_child(n, i);
                            if (get_node_type(c) == "call_suffix") {
                                args = c;
                                break;
                            }
                        }
                    }
                    if (!ts_node_is_null(args)) fid = cause_fidelity(args, caught_var);
                }
                if (log) {
                    site.has_log_call = true;
                    note_log_level(site, level_of_log_callee(bare));
                    if (fid > site.logged_fidelity) site.logged_fidelity = fid;
                } else {
                    site.has_other_call = true;
                    if (fid != CauseFidelity::None) {
                        site.propagates_cause = true;
                        if (fid > site.propagated_fidelity) {
                            site.propagated_fidelity = fid;
                        }
                    }
                }
            }
            return true;
        });
    }

    side_effects_->record_catch(site);
}

// ---------------------------------------------------------------------------
// Go dropped-error detection
// ---------------------------------------------------------------------------

void UnifiedExtractor::process_go_error_drop(TSNode node,
                                             std::string_view node_type) {
    (void)node_type;
    TSNode left = field(node, "left");
    TSNode right = field(node, "right");
    if (ts_node_is_null(left) || ts_node_is_null(right)) return;

    // Go convention: the error is the LAST result. Only a blank in the final
    // left position discards it — `host, _, err := ...` captures the error,
    // and `v, _ := x.(T)` / `v, _ := m[k]` discard ok-bools, not errors.
    int total = 0;
    bool last_is_blank = false;
    if (get_node_type(left) == "expression_list") {
        uint32_t n = ts_node_named_child_count(left);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode c = ts_node_named_child(left, i);
            ++total;
            if (i + 1 == n) last_is_blank = node_text(c) == "_";
        }
    } else {
        total = 1;
        last_is_blank = node_text(left) == "_";
    }
    if (!last_is_blank) return;
    // Multi-value assigns with a trailing blank are as likely ok-bool
    // discards (strings.Cut's found, map reads, multi-result lookups) as
    // errors, and without return types telling them apart is guesswork that
    // was mostly noise on chi. Only the sole-discard form fires:
    // `_ = err` / `_ = f()`. That is also why record_dropped_error has one
    // severity tier — everything reaching it is unambiguous.
    if (total != 1) return;

    // The right side must be a CALL producing the discarded result. A
    // type-assertion or map index in the terminal value position yields an
    // ok-bool, not an error — never a drop.
    bool right_is_call = false;
    bool right_is_err_ident = false;
    {
        TSNode value = right;
        if (get_node_type(value) == "expression_list") {
            uint32_t n = ts_node_named_child_count(value);
            if (n > 0) value = ts_node_named_child(value, n - 1);
        }
        std::string_view vt = get_node_type(value);
        if (vt == "call_expression") {
            right_is_call = true;
        } else if (vt == "type_assertion_expression" ||
                   vt == "index_expression") {
            return;  // ok-bool discard, not an error
        } else if (is_identifier_type(vt) && iprefix(node_text(value), "err")) {
            right_is_err_ident = true;  // `_ = err`
        }
    }
    if (!right_is_call && !right_is_err_ident) return;

    // `_ = c.Error(err)` hands the error to a recorder and discards THAT
    // call's return value — the error is kept, not dropped. gin's
    // context.go:1213 is the canonical case, and it was this repo's only
    // "unchecked error" on the whole gin corpus. An err-like identifier in
    // the argument list is the signal.
    if (right_is_call) {
        TSNode value = right;
        if (get_node_type(value) == "expression_list") {
            uint32_t n = ts_node_named_child_count(value);
            if (n > 0) value = ts_node_named_child(value, n - 1);
        }
        TSNode args = field(value, "arguments");
        if (!ts_node_is_null(args)) {
            bool hands_off_error = false;
            walk_subtree(args, [&](TSNode a) {
                if (is_identifier_type(get_node_type(a)) &&
                    iprefix(node_text(a), "err")) {
                    hands_off_error = true;
                }
                return !hands_off_error;
            });
            if (hands_off_error) return;
        }
    }

    int line = line_of(node);
    std::string detail = "`" + std::string(node_text(node)) + "`";
    if (detail.size() > 60) {
        detail.resize(57);
        detail += "...`";
    }
    // Inside a defer the discard is the cleanup idiom (`defer func() { _ =
    // f.Close() }()`): there is no caller left to tell.
    if (se_guard_depth_ > 0) return;
    side_effects_->record_dropped_error(line, detail);
}

}  // namespace lci::parser
