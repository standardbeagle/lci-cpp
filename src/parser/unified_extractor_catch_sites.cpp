#include <lci/parser/parser.h>
#include "unified_extractor_internal.h"
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
