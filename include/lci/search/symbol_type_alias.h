#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/types.h>

namespace lci {

/// Canonicalization of user-supplied symbol-type names.
///
/// The MCP `search` tool advertises short aliases (func, var, cls, …) in its
/// `symbol_types` description, but the search path used to compare the
/// caller's strings verbatim against SymbolType names. Every advertised alias
/// therefore matched nothing and the caller got an empty result set with a
/// hint suggesting a shorter pattern — a documented input silently producing
/// a wrong answer.
///
/// This is the ONE place the alias table lives. The `list`/`explore` symbol
/// filters carry their own parallel spelling table; they are meant to adopt
/// this header rather than grow a second copy.
///
/// Divergence from the tool description, deliberate: the description claims
/// `trait -> interface`. SymbolType has its OWN Trait variant, so honoring
/// that mapping would make `symbol_types=trait` never match a Rust trait —
/// the exact silent-zero failure this table exists to remove. `trait`
/// canonicalizes to `trait`. The description is the thing that needs
/// correcting.
///
/// Header-only on purpose: src/CMakeLists.txt lists sources explicitly and is
/// outside this change's file scope, so a new translation unit could not be
/// registered. The tables are function-local statics, built once.

/// Every canonical symbol-type name, in SymbolType declaration order.
/// Used to build the "valid types are ..." half of an error message.
inline const std::vector<std::string_view>& canonical_symbol_type_names() {
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> v;
        v.reserve(kSymbolTypeCount);
        for (int i = 0; i < kSymbolTypeCount; ++i) {
            v.push_back(to_string(static_cast<SymbolType>(i)));
        }
        return v;
    }();
    return names;
}

/// Comma-separated canonical names, for error messages.
inline const std::string& canonical_symbol_type_list() {
    static const std::string joined = [] {
        std::string s;
        for (auto name : canonical_symbol_type_names()) {
            if (!s.empty()) s.append(", ");
            s.append(name);
        }
        return s;
    }();
    return joined;
}

namespace detail {

/// alias -> canonical. Canonical names map to themselves and are seeded from
/// to_string(), so the table can never drift from the SymbolType enum.
inline const absl::flat_hash_map<std::string_view, std::string_view>&
symbol_type_alias_table() {
    static const auto table = [] {
        absl::flat_hash_map<std::string_view, std::string_view> m;
        for (auto name : canonical_symbol_type_names()) m.emplace(name, name);
        // Aliases advertised by the MCP `search` tool description.
        m.emplace("func", "function");
        m.emplace("fn", "function");    // Rust
        m.emplace("def", "function");   // Python
        m.emplace("var", "variable");
        m.emplace("const", "constant");
        m.emplace("cls", "class");
        m.emplace("meth", "method");
        m.emplace("iface", "interface");
        return m;
    }();
    return table;
}

}  // namespace detail

/// Returns the canonical SymbolType name for `name`, matching case-
/// insensitively against both the canonical names and the advertised
/// aliases. Returns an empty view when `name` is not a known type or alias --
/// callers must treat that as an error, never as "matches nothing".
///
/// The returned view points into the static table, not into `name`.
inline std::string_view canonical_symbol_type(std::string_view name) {
    if (name.empty() || name.size() > 32) return {};
    // Symbol-type lists are short and validated once per query, not per file;
    // the fixed buffer keeps it allocation-free (Karpathy rule 2).
    char buf[32];
    for (size_t i = 0; i < name.size(); ++i) {
        buf[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(name[i])));
    }
    const auto& table = detail::symbol_type_alias_table();
    auto it = table.find(std::string_view(buf, name.size()));
    if (it == table.end()) return {};
    return it->second;
}

}  // namespace lci
