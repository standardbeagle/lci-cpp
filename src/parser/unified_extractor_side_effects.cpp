#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <lci/analysis/side_effect_analyzer.h>

#include <tree_sitter/api.h>

#include <cstring>

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

}  // namespace

void UnifiedExtractor::record_lvalue_write(TSNode lvalue, int line, int column) {
    if (!side_effects_) return;
    TSNode n = lvalue;
    // Descend member / subscript / selector expressions to the base identifier
    // that owns the mutation (a.b.c = x mutates `a`; arr[i] = x mutates `arr`).
    for (int guard = 0; guard < 32 && !ts_node_is_null(n); ++guard) {
        std::string_view t = get_node_type(n);
        if (is_identifier_type(t)) {
            std::string_view id = node_text(n);
            if (!id.empty())
                side_effects_->record_access(id, {}, AccessType::Write, line,
                                             column);
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

    // Assignment patterns - writes to state.
    if (node_type == "assignment_expression" ||
        node_type == "assignment_statement" || node_type == "assignment") {
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

    // Throw statements (Java / C# / C / C++) and PHP throw expressions plus the
    // universal fallback (JS `throw`, etc.).
    if (node_type == "throw_statement" || node_type == "throw_expression") {
        side_effects_->record_throw({}, line, column);
        return;
    }

    // Call expressions - record the callee for Phase-2 transitive resolution.
    if (node_type == "call_expression" || node_type == "call" ||
        node_type == "call_statement") {
        TSNode func = field(node, "function");
        if (ts_node_is_null(func)) func = field(node, "name");
        if (ts_node_is_null(func)) func = ts_node_named_child(node, 0);
        if (ts_node_is_null(func)) return;

        std::string_view callee = node_text(func);
        if (callee.empty()) return;

        // Qualified `pkg.Fn` / `recv.method` -> split into qualifier + name so
        // the resolver can match the method by receiver.
        auto dot = callee.rfind('.');
        if (dot != std::string_view::npos && dot + 1 < callee.size()) {
            side_effects_->record_function_call(callee.substr(dot + 1),
                                                callee.substr(0, dot),
                                                /*is_method=*/true, line,
                                                column);
        } else {
            side_effects_->record_function_call(callee, {}, /*is_method=*/false,
                                                line, column);
        }
        return;
    }
}

}  // namespace lci::parser
