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
    if (ts_node_is_null(params)) {
        // Zig (and Kotlin's function_value_parameters): the parameter list
        // is an unfielded named CHILD. Without this every `self: *T`
        // mutation classified as a global write and param_writes stayed 0.
        uint32_t nc = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < nc; ++i) {
            TSNode c = ts_node_named_child(node, i);
            std::string_view ct = get_node_type(c);
            if (ct == "parameters" || ct == "function_value_parameters") {
                params = c;
                break;
            }
        }
    }
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

    // Zig statement-level assignment: the grammar parses `self.count += 1;`
    // as a variable_declaration with NO const/var keyword — there is no
    // assignment_expression node to hook. A keyword-less declaration whose
    // first child is an lvalue expression is an assignment.
    if (ext_ == ".zig" && node_type == "variable_declaration") {
        bool declares = false;
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc && i < 2; ++i) {
            std::string_view ct = get_node_type(ts_node_child(node, i));
            if (ct == "const" || ct == "var" || ct == "comptime") {
                declares = true;
                break;
            }
        }
        if (!declares && ts_node_named_child_count(node) > 0) {
            TSNode left = ts_node_named_child(node, 0);
            std::string_view lt = get_node_type(left);
            if (lt == "field_expression" || lt == "identifier" ||
                lt == "index_expression") {
                record_lvalue_write(left, line, column);
            }
            return;
        }
    }

    // Assignment patterns - writes to state. Field initializers inside a
    // struct/initializer literal are CONSTRUCTION, not mutation: Zig's
    // `return .{ .start = a };` parses its `.start = a` as an assignment and
    // flagged pure functions as global writers (zls audit).
    if (node_type == "assignment_expression" ||
        node_type == "assignment_statement" || node_type == "assignment") {
        for (TSNode p = ts_node_parent(node), guard = p; ; ) {
            (void)guard;
            if (ts_node_is_null(p)) break;
            std::string_view pt = get_node_type(p);
            if (pt == "struct_initializer" || pt == "initializer_list" ||
                pt == "anonymous_struct_initializer" ||
                pt == "initializer_expression") {
                return;
            }
            if (pt == "block" || pt == "compound_statement" ||
                is_function_node(pt)) {
                break;
            }
            p = ts_node_parent(p);
        }
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
        node_type == "call_statement" ||
        // PHP spells calls differently; without these no PHP call was ever
        // recorded in the AST pass, so builtin io (file_put_contents) never
        // classified (battery audit).
        node_type == "function_call_expression" ||
        node_type == "member_call_expression" ||
        node_type == "scoped_call_expression" ||
        node_type == "nullsafe_member_call_expression") {
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

}  // namespace lci::parser
