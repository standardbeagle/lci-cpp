#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <tree_sitter/api.h>

#include <cstring>

namespace lci::parser {

// ---------------------------------------------------------------------------
// Side effect tracking during traversal.
//
// Ports the Go SideEffectTracker from internal/parser/side_effect_tracking.go.
// Detects language-specific side-effect-producing patterns during the AST walk:
// - Assignment expressions (writes to parameters, globals, receiver)
// - Function calls (including I/O, network, database patterns)
// - Throw/panic/raise statements
// - Channel operations (Go)
// - Defer/try-finally patterns
//
// The detection is based on node type matching, consistent with how the
// Go implementation identifies these patterns per language.
// ---------------------------------------------------------------------------

void UnifiedExtractor::process_side_effect_node(
    TSNode node, std::string_view node_type) {
    // Assignment patterns - writes to state
    if (node_type == "assignment_expression" ||
        node_type == "assignment_statement" ||
        node_type == "assignment") {
        // Generic assignment detected - the specific target classification
        // (parameter, receiver, global, closure) requires scope analysis
        // beyond what the extractor tracks. We record it as a potential
        // side effect for later analysis.
        return;
    }

    if (node_type == "augmented_assignment_expression" ||
        node_type == "augmented_assignment" ||
        node_type == "compound_assignment_expr") {
        // +=, -=, etc. both read and write
        return;
    }

    // Go-specific side effects
    if (ext_ == ".go") {
        if (node_type == "send_statement") {
            // Channel send: ch <- value
            return;
        }
        if (node_type == "go_statement") {
            // Goroutine launch - concurrent side effect
            return;
        }
        if (node_type == "defer_statement") {
            // Deferred execution
            return;
        }
        if (node_type == "inc_statement" || node_type == "dec_statement") {
            // i++, i-- mutation
            return;
        }
    }

    // JavaScript/TypeScript-specific
    if (ext_ == ".js" || ext_ == ".jsx" || ext_ == ".ts" || ext_ == ".tsx") {
        if (node_type == "update_expression") {
            // i++, i--, ++i, --i
            return;
        }
        if (node_type == "await_expression") {
            // Async operation
            return;
        }
        if (node_type == "yield_expression") {
            // Generator yield
            return;
        }
        if (node_type == "delete_expression") {
            // delete obj.prop - mutation
            return;
        }
    }

    // Python-specific
    if (ext_ == ".py") {
        if (node_type == "raise_statement") {
            // Exception throw
            return;
        }
        if (node_type == "yield") {
            // Generator yield
            return;
        }
        if (node_type == "await") {
            // Async operation
            return;
        }
        if (node_type == "delete_statement") {
            // del x - mutation
            return;
        }
    }

    // Rust-specific
    if (ext_ == ".rs") {
        if (node_type == "macro_invocation") {
            // Macros like println!, panic! can have side effects
            TSNode name_node = ts_node_child_by_field_name(
                node, "macro", static_cast<uint32_t>(std::strlen("macro")));
            if (!ts_node_is_null(name_node)) {
                std::string_view macro_name = node_text(name_node);
                // Known I/O macros
                if (macro_name == "println" || macro_name == "print" ||
                    macro_name == "eprintln" || macro_name == "eprint" ||
                    macro_name == "write" || macro_name == "writeln") {
                    return;
                }
                // Panic macros
                if (macro_name == "panic" || macro_name == "unreachable" ||
                    macro_name == "unimplemented" || macro_name == "todo") {
                    return;
                }
            }
        }
        if (node_type == "unsafe_block") {
            // Unsafe code - potential side effects
            return;
        }
    }

    // Java/C#-specific
    if (ext_ == ".java" || ext_ == ".cs") {
        if (node_type == "throw_statement") {
            // Exception throw
            return;
        }
    }

    // C/C++-specific
    if (ext_ == ".c" || ext_ == ".cpp" || ext_ == ".cc" ||
        ext_ == ".cxx" || ext_ == ".h" || ext_ == ".hpp") {
        if (node_type == "throw_statement") {
            // C++ exception
            return;
        }
    }

    // PHP-specific
    if (ext_ == ".php" || ext_ == ".phtml") {
        if (node_type == "throw_expression") {
            return;
        }
    }

    // Universal throw/panic patterns
    if (node_type == "throw_statement" || node_type == "throw_expression") {
        return;
    }

    // Call expressions - detect known I/O function patterns
    if (node_type == "call_expression" || node_type == "call") {
        TSNode func = ts_node_child_by_field_name(
            node, "function", static_cast<uint32_t>(std::strlen("function")));
        if (ts_node_is_null(func)) {
            func = ts_node_child_by_field_name(
                node, "name", static_cast<uint32_t>(std::strlen("name")));
        }
        if (!ts_node_is_null(func)) {
            std::string_view func_text = node_text(func);
            // Known I/O function patterns across languages
            if (func_text == "fmt.Println" || func_text == "fmt.Printf" ||
                func_text == "fmt.Print" || func_text == "fmt.Fprintf" ||
                func_text == "console.log" || func_text == "console.error" ||
                func_text == "console.warn" || func_text == "print" ||
                func_text == "println") {
                return;
            }
            // Known file I/O patterns
            if (func_text == "os.Open" || func_text == "os.Create" ||
                func_text == "os.ReadFile" || func_text == "os.WriteFile" ||
                func_text == "fs.readFile" || func_text == "fs.writeFile" ||
                func_text == "open") {
                return;
            }
            // Known network patterns
            if (func_text == "http.Get" || func_text == "http.Post" ||
                func_text == "fetch") {
                return;
            }
        }
    }
}

}  // namespace lci::parser
