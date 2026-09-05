// Column contract for every CLI-facing result row.
//
// THE ONE column base, stated here and referenced everywhere else:
//
//   `column` is a 0-based byte offset into the matched line, exactly as
//   emitted by the search engine (MasterIndex::search computes
//   `col = match_start - line_start`, see src/indexing/master_index_search.cpp).
//   The value kColumnUnknown (-1) means "match position not recorded".
//   0 is a REAL position — the first byte of the line.
//
// Every component that consumes or rewrites a row's `column` (grep row
// filters, AST content filters, JSON emitters) MUST use this base. The
// historical disagreement — the -E row filter rewriting to 1-based while
// literal rows stayed 0-based — made `-w` drop real word hits and made
// --strings-only/--comments-only misclassify by one byte, and made --json
// columns differ between -E and literal rows.
//
// `rg --json` submatch offsets are 0-based byte offsets too, so rows
// honoring this contract diff cleanly against ripgrep's JSON oracle.

#pragma once

namespace lci {
namespace cli {

/// Sentinel row column: "match position not recorded". Distinct from every
/// real position because column 0 is the first byte of the line.
inline constexpr int kColumnUnknown = -1;

/// A row column honors the contract iff it is the unknown sentinel or a
/// valid 0-based byte offset.
inline constexpr bool column_honors_contract(int column) {
    return column == kColumnUnknown || column >= 0;
}

}  // namespace cli
}  // namespace lci
