// Internal helpers for `lci tree` output mode flags. Exposed via this header
// so unit tests in tests/cli_test.cpp can exercise the pure formatting logic
// without going through the full CLI/server pipeline. Not part of the public
// API.
//
// Header-only (pure logic, only depends on nlohmann::json + std), parallel
// in spirit to grep_filters.h and symbol_filters.h. Implementation is
// inline because the helpers are short and only included by commands.cpp
// and the cli_test.cpp unit suite.
//
// Mirrors the layout of Go's internal/display/tree_formatter.go: a single
// `Options` struct controls all output modes, and `format_tree` dispatches
// on `Options::mode` to produce the final string.
//
// Output modes:
//
//   - Text (default)  : ASCII tree with branch chars `→ / ├─→ / └─→`,
//                       header lines, depth tag per node. Mirrors Go's
//                       formatText (tree_formatter.go:50). Box-drawing
//                       chars are unicode (├ │ └ ─).
//   - Compact         : single-line `name → name → name (+N more)`. Linear
//                       chain following the first child; siblings reported
//                       as `(+N more)`. Mirrors Go's formatCompact
//                       (tree_formatter.go:194).
//   - Agent           : indented plain text, ASCII-only -- no unicode or
//                       box-drawing chars. One node per line as
//                       `<two-space indent>* <name> ...`. Designed for
//                       coding agents and line-based tools (grep, awk).
//
// `metrics` and `show_lines` are orthogonal flags that layer on top of
// any of the three modes (Go's tree command does the same; see
// cmd/lci/main.go:1226-1233):
//
//   - `show_lines=true`  emits `[file_path:line]` after the node name when
//     the row resolved a non-empty file_path / non-zero line.
//   - `metrics=true`     emits `(complexity:N, lines:M)` after the node
//     name (or after the file annotation) when the CLI driver stamped
//     `complexity` and/or `lines_of_code` onto the node JSON via a
//     /browse-file lookup before formatting.
//
// The formatter is deliberately oblivious to where `complexity` /
// `lines_of_code` come from -- the driver in commands.cpp is responsible
// for populating them (cached per file) before calling format_tree.

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci {
namespace cli {
namespace tree_formatter {

/// Output mode for the tree formatter. `Text` is the default; `Compact`
/// and `Agent` mirror Go's `Format` field on `display.FormatterOptions`.
enum class Mode {
    Text,
    Compact,
    Agent,
};

/// Formatter options. Composable: e.g. `mode=Agent, show_lines=true,
/// metrics=true` produces agent-mode plain text with line and metric
/// annotations on each node.
struct Options {
    Mode mode{Mode::Text};
    bool show_lines{false};
    bool metrics{false};
    int max_depth{0};  // 0 = no client-side depth cutoff
};

namespace detail {

inline std::string format_node_annotations(const nlohmann::json& node,
                                           const Options& opts) {
    std::string out;
    if (opts.show_lines) {
        std::string fp = node.value("file_path", "");
        int line = node.value("line", 0);
        if (!fp.empty() && line > 0) {
            out.append(" [");
            out.append(fp);
            out.push_back(':');
            out.append(std::to_string(line));
            out.push_back(']');
        } else if (line > 0) {
            // Fall back to bare line when file_path is unknown so tests
            // running against the synthetic corpus still get a useful
            // annotation.
            out.append(" [:");
            out.append(std::to_string(line));
            out.push_back(']');
        }
    }
    if (opts.metrics) {
        int complexity = node.value("complexity", 0);
        int loc = node.value("lines_of_code", 0);
        if (complexity > 0 || loc > 0) {
            out.append(" (");
            bool first = true;
            if (complexity > 0) {
                out.append("complexity:");
                out.append(std::to_string(complexity));
                first = false;
            }
            if (loc > 0) {
                if (!first) out.append(", ");
                out.append("lines:");
                out.append(std::to_string(loc));
            }
            out.push_back(')');
        }
    }
    return out;
}

inline void collect_compact_parts(const nlohmann::json& node,
                                  std::vector<std::string>& parts) {
    if (!node.is_object()) return;
    parts.push_back(node.value("name", ""));
    if (!node.contains("children") || !node["children"].is_array()) return;
    const auto& children = node["children"];
    if (children.empty()) return;
    collect_compact_parts(children[0], parts);
    if (children.size() > 1) {
        parts.push_back("(+" + std::to_string(children.size() - 1) + " more)");
    }
}

}  // namespace detail

/// Returns the per-node decoration suffix produced by the `--show-lines`
/// and `--metrics` flags. Pure: reads `file_path`, `line`, `complexity`,
/// `lines_of_code` from `node`. Returns "" when neither flag adds anything.
inline std::string node_annotations(const nlohmann::json& node,
                                    const Options& opts) {
    return detail::format_node_annotations(node, opts);
}

/// Recursively walks `node` and appends the formatted line(s) to `out`.
/// `prefix` is the indentation/branch prefix for this line; `is_last`
/// controls the branch glyph (`└─→` vs `├─→`); `is_root` selects the
/// `→ ` root glyph in Text mode. In Agent mode all branch glyphs are
/// replaced with two-space indentation -- ASCII-only output.
inline void format_node(std::string& out, const nlohmann::json& node,
                        const std::string& prefix, bool is_last, bool is_root,
                        const Options& opts) {
    if (!node.is_object()) return;

    int depth = node.value("depth", 0);
    if (opts.max_depth > 0 && depth > opts.max_depth) return;

    std::string name = node.value("name", "");

    out.append(prefix);

    if (opts.mode == Mode::Agent) {
        // Agent mode: ASCII-only, two-space indent already encoded in
        // `prefix`. No branch glyph -- bullets confuse line-based tools.
        // Match Go's text formatter shape so coding agents see one node
        // per line: `  name [file:line] (depth=N) (complexity:N, lines:M)`
        out.append(name);
    } else {
        if (is_root) {
            out.append("\xe2\x86\x92 ");  // →
        } else if (is_last) {
            out.append("\xe2\x94\x94\xe2\x94\x80\xe2\x86\x92 ");  // └─→
        } else {
            out.append("\xe2\x94\x9c\xe2\x94\x80\xe2\x86\x92 ");  // ├─→
        }
        out.append(name);
    }

    out.append(detail::format_node_annotations(node, opts));

    // Always append depth tag in text/agent mode (matches Go's
    // formatNode at tree_formatter.go:109). Compact mode never reaches
    // this function.
    char depth_buf[32];
    std::snprintf(depth_buf, sizeof(depth_buf), " (depth=%d)", depth);
    out.append(depth_buf);

    out.push_back('\n');

    if (!node.contains("children") || !node["children"].is_array()) return;
    const auto& children = node["children"];
    int child_count = static_cast<int>(children.size());
    for (int i = 0; i < child_count; ++i) {
        bool last = (i == child_count - 1);
        std::string child_prefix;
        if (opts.mode == Mode::Agent) {
            // Two-space indent per level, ASCII only.
            child_prefix = prefix + "  ";
        } else if (is_root) {
            child_prefix = prefix + "  ";
        } else if (is_last) {
            child_prefix = prefix + "  ";
        } else {
            // Continuation bar for non-last ancestor.
            child_prefix = prefix + "\xe2\x94\x82 ";  // │
        }
        format_node(out, children[i], child_prefix, last, false, opts);
    }
}

/// Compact mode: single-line linear chain. Mirrors Go's formatCompact
/// (tree_formatter.go:194).
inline std::string format_compact(const nlohmann::json& tree) {
    if (!tree.is_object() || !tree.contains("root")) return {};
    const auto& root = tree["root"];
    if (!root.is_object()) return {};
    std::vector<std::string> parts;
    detail::collect_compact_parts(root, parts);
    if (parts.empty()) return {};
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.append(" \xe2\x86\x92 ");  // →
        out.append(parts[i]);
    }
    return out;
}

/// Text/Agent mode: header line + recursive node tree. `Mode::Agent`
/// switches to ASCII-only output and a simpler header line so the result
/// pipes cleanly into grep/awk.
inline std::string format_text(const nlohmann::json& tree,
                               const Options& opts) {
    if (!tree.is_object() || !tree.contains("root")) {
        return "No tree data available\n";
    }
    const auto& root = tree["root"];
    if (!root.is_object()) {
        return "No tree data available\n";
    }

    std::string out;
    std::string root_function = tree.value("root_function", "");
    int total_nodes = tree.value("total_nodes", 0);
    int tree_max_depth = tree.value("max_depth", 0);

    if (opts.mode == Mode::Agent) {
        // Plain ASCII header -- no decorations. Coding agents grep these
        // lines for the function name and node count.
        out.append("Function tree for '");
        out.append(root_function);
        out.append("'\n");
        out.append("Total nodes: ");
        out.append(std::to_string(total_nodes));
        out.append(", Max depth: ");
        out.append(std::to_string(tree_max_depth));
        out.append("\n\n");
    } else {
        out.append("Function tree for '");
        out.append(root_function);
        out.append("'\n");
        out.append("Total nodes: ");
        out.append(std::to_string(total_nodes));
        out.append(", Max depth: ");
        out.append(std::to_string(tree_max_depth));
        out.append("\n\n");
    }

    format_node(out, root, "", true, true, opts);
    return out;
}

/// Top-level dispatcher: picks compact / text / agent based on
/// `opts.mode`.
inline std::string format_tree(const nlohmann::json& tree,
                               const Options& opts) {
    if (opts.mode == Mode::Compact) {
        return format_compact(tree) + "\n";
    }
    return format_text(tree, opts);
}

}  // namespace tree_formatter
}  // namespace cli
}  // namespace lci
