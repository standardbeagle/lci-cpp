#pragma once

// Internal tree-sitter node helpers shared by the unified_extractor_*.cpp
// translation units. Not part of the public parser interface.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include <lci/side_effects.h>

namespace lci::parser {

/// First named child of `node` whose grammar type equals `type`; null node
/// if none.
inline TSNode first_named_child_typed(TSNode node, std::string_view type) {
    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        TSNode c = ts_node_named_child(node, i);
        if (std::string_view(ts_node_type(c)) == type) return c;
    }
    return TSNode{};
}

// -- Shared node/name inspection helpers for the side-effect and catch-site
// extraction TUs (moved from unified_extractor_side_effects.cpp when it was
// split; both halves lean on them heavily).


// Field-name lookup shorthand (tree-sitter wants the byte length).
inline TSNode field(TSNode node, const char* name) {
    return ts_node_child_by_field_name(
        node, name, static_cast<uint32_t>(std::strlen(name)));
}

// Identifier-shaped node types across the grammars we register params from.
inline bool is_identifier_type(std::string_view t) {
    return t == "identifier" || t == "simple_identifier" ||
           t == "field_identifier" || t == "property_identifier" ||
           t == "shorthand_property_identifier" || t == "variable_name" ||
           t == "name" || t == "dotted_name";
}

inline int line_of(TSNode node) {
    return static_cast<int>(ts_node_start_point(node).row) + 1;
}
inline int col_of(TSNode node) {
    return static_cast<int>(ts_node_start_point(node).column) + 1;
}

// Case-insensitive ASCII prefix match (local; the analyzer keeps its own).
inline bool iprefix(std::string_view name, std::string_view prefix) {
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
inline std::string_view level_of_log_callee(std::string_view callee) {
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
inline int log_level_rank(std::string_view level) {
    if (level == "error") return 4;
    if (level == "warn") return 3;
    if (level == "info") return 2;
    if (level == "debug") return 1;
    return 0;  // "print"
}

inline void note_log_level(CatchSiteInfo& site, std::string_view level) {
    if (site.log_level.empty() ||
        log_level_rank(level) > log_level_rank(site.log_level)) {
        site.log_level = std::string(level);
    }
}

inline bool is_log_callee(std::string_view callee) {
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
inline bool is_log_qualifier(std::string_view qualifier) {
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
inline bool header_names_a_normal_condition(std::string_view header) {
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
inline CalleeParts split_callee(std::string_view callee) {
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
inline bool is_throw_node(std::string_view t) {
    return t == "throw_statement" || t == "throw_expression" ||
           t == "raise_statement" || t == "throw";
}

inline bool is_call_node(std::string_view t) {
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
inline bool is_assignment_node(std::string_view t) {
    return t == "assignment_expression" || t == "assignment" ||
           t == "augmented_assignment_expression" ||
           t == "variable_declarator" || t == "yield_expression" ||
           t == "yield";
}


}  // namespace lci::parser
