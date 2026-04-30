# Known Parity Failures

Baseline last updated 2026-04-30.

- Go reference: `lci version 0.4.1`
- C++ port: `lci 0.1.0`

Run: `ctest --test-dir build/debug -L parity --output-on-failure`

Result: **64 / 64 parity descriptors passing.** There are no active parity
failures in the current suite.

## Status by category

| Category | Failing | Total | Status |
|---|---:|---:|---|
| `cli.*` | 0 | 28 | Green |
| `http.*` | 0 | 12 | Green |
| `index.*` | 0 | 3 | Green |
| `mcp.*` | 0 | 17 | Green |
| `probes.*` | 0 | 3 | Green |
| **Total** | **0** | **64** | **Green** |

## No active failing test IDs

All descriptors currently pass.

## Intentional divergences still documented

These are not active failures, but they remain intentional descriptor or
contract decisions and should stay documented:

1. `index.*` compares `debug export` at exit-code level only because Go and C++
   export fundamentally different payloads.
2. `cli.search.case-insensitive` is intentionally stabilized as a lighter probe
   because the Go reference is flaky on the synthetic multi-language corpus.
3. `http.git-analyze` ignores `report.summary.top_recommendation` because the
   underlying recommendation copy diverges even when the analyzed refs and
   summary counts align.

## Next strengthening work

1. Replace weakened parity probes with stronger happy-path coverage where the
   product surface is ready.
2. Promote stable parity surfaces into C++-owned goldens where that gives
   clearer ownership than side-by-side Go comparison.
3. Keep `MODULE_MAP.md` aligned with the current parity decisions so this file
   can remain a short status page instead of a historical backlog dump.
