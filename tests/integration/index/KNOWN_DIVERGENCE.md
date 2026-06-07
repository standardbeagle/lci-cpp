# Index suite — golden coverage notes

`debug export` returns the running server's in-memory snapshot. The index suite
pins it with a checked-in golden only for the fixed synthetic corpus; the two
live-repo cases are **exit-code only** (no golden).

## Exit-code only (live corpora)

- **`index/lci-cpp-repo`** — corpus is this repo's working tree under
  `tests/parity/corpora/lci-cpp-repo`. `file_count` / `symbol_count` change on
  every commit, so a golden pinned to a snapshot would force a refresh on every
  PR that touches `src/`, `include/`, or `tests/`. Verifying exit code 0 is
  sufficient.
- **`index/large-repo`** — corpus is a large external checkout; same reasoning,
  and its count is volatile independent of this repo's tree.

## Golden-pinned

- **`index/synthetic-multilang`** — corpus is a fixed checked-in fixture
  (`tests/parity/corpora/synthetic/multi-lang/`), so `debug export` is
  reproducible across CI runs. Spec: `tests/integration/index/synthetic-multilang.spec.json`;
  golden: `tests/integration/goldens/index/synthetic-multilang.json`.

## Future tightening

`file_count` / `symbol_count` could be promoted from `ignore` to `stable` for
the synthetic corpus, but that requires threading a running server (standalone
`lci debug export` writes zeros with no server listening — see
`runner/modes/index.cpp`). Deferred.
