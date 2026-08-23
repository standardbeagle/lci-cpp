#pragma once

// Linter-style suppression directives for analysis findings.
//
// The vocabulary every linter a developer has used shares (eslint, clang-tidy,
// rubocop, pylint): a directive inside any comment, optionally naming rules.
//
//   // lci-disable-next-line empty-catch
//   catch (e) { }                          // lci-disable-line
//   /* lci-disable partial-write-risk, broad-catch */
//   ...
//   /* lci-enable */
//
// Rule names are the finding signal names the report prints
// (`empty-catch`, `log-and-swallow`, ...). A directive with no rule list
// disables every rule. Rules are separated by commas or spaces; an unknown
// rule name is inert rather than an error, so a directive written for a
// future rule does not break today's run.
//
// Detection is a text scan for the `lci-` marker, not a comment parse: it
// runs once per file, only when the analysis pass is attached, and costs one
// pass over the bytes.

#include <string>
#include <string_view>
#include <vector>

namespace lci {

struct FindingSuppressions {
    struct Entry {
        int first_line{};  // inclusive, 1-based
        int last_line{};   // inclusive; INT_MAX for an unterminated block
        std::vector<std::string> rules;  // empty = every rule
    };
    std::vector<Entry> entries;

    bool empty() const { return entries.empty(); }
    /// True when a finding of `rule` on `line` is suppressed.
    bool suppresses(int line, std::string_view rule) const;
};

/// Scans `source` for directives. Lines are 1-based to match finding lines.
FindingSuppressions parse_finding_suppressions(std::string_view source);

}  // namespace lci
