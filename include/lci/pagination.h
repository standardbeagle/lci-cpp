#pragma once

// Shared max/offset pagination semantics for every list-shaped response
// (MCP list_symbols, HTTP /list-symbols). One implementation so the two
// surfaces cannot drift: before this header existed, MCP clamped max<=0 to
// 1 while HTTP substituted the default, and HTTP left negative offsets
// unclamped -- which made has_more report true after showing every result.
//
// Semantics:
//   max     <= 0        -> default (an explicit 0 means "unset", not "one")
//   max     >  cap      -> cap
//   offset  <  0        -> 0
//   has_more            <-> offset + shown < total

#include <nlohmann/json.hpp>

namespace lci {

struct PageWindow {
    int offset = 0;
    int max = 0;
};

/// Reads and normalizes "max"/"offset" from a request body.
inline PageWindow normalize_page(const nlohmann::json& params,
                                 int default_max = 50, int max_cap = 500) {
    PageWindow w;
    w.max = params.value("max", default_max);
    if (w.max <= 0) w.max = default_max;
    if (w.max > max_cap) w.max = max_cap;
    w.offset = params.value("offset", 0);
    if (w.offset < 0) w.offset = 0;
    return w;
}

/// True when results remain past the window that was just emitted. `shown`
/// is the number of entries actually returned, `total` the unpaginated
/// match count. Requires a normalized (non-negative) offset.
inline bool page_has_more(int total, int offset, int shown) {
    return total > offset + shown;
}

}  // namespace lci
