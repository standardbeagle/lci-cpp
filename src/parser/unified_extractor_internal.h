#pragma once

// Internal tree-sitter node helpers shared by the unified_extractor_*.cpp
// translation units. Not part of the public parser interface.

#include <string_view>

#include <tree_sitter/api.h>

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

}  // namespace lci::parser
