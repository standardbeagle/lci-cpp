# Index suite — known divergences

The integration index suite mirrors the parity descriptors under
`tests/parity/descriptors/index/`. Two of the three index descriptors are
**not migrated** to deterministic spec.json + golden pairs because their
inputs are live git checkouts whose counts (`file_count`, `symbol_count`,
…) change with every commit, and goldens cannot track that without
constant churn.

## Decision C — payload-level divergence

All three index parity descriptors already declare Decision C: every
schema field lives in the `ignore` tier (Go and C++ `debug export`
schemas are disjoint by design — see the `_rationale` field on each
parity descriptor and `MODULE_MAP.md > Known Divergences`). After
ignore-tier scrubbing the canonical comparison value is `{}` regardless
of the corpus, which means the integration suite reduces to checking
"exit code is 0" for these cases.

## Not migrated

### `index/lci-cpp-repo`
- **Source:** `tests/parity/descriptors/index/lci-cpp-repo.parity.json`
- **Corpus:** `tests/parity/corpora/lci-cpp-repo` (this very repo,
  re-indexed during the test)
- **Why no integration golden:** the corpus is the host repo's working
  tree. `file_count` and `symbol_count` change on every commit, and a
  golden pinned to a commit-snapshot would force a golden update on
  every PR that touches `src/`, `include/`, or `tests/`. The parity
  oracle handles this by ignoring the entire payload (Decision C); an
  integration test would either replicate that empty `{}` golden — same
  semantic value as the synthetic-multilang case — or chase the live
  counts.
- **Coverage:** the parity oracle (`ctest -L parity -R parity\.index`)
  still exercises this descriptor end-to-end with the Go reference
  binary; it is the authoritative cross-port check for index mode on
  this corpus. Removing it from integration does not reduce coverage.

### `index/lci-go-repo`
- **Source:** `tests/parity/descriptors/index/lci-go-repo.parity.json`
- **Corpus:** `tests/parity/corpora/lci-go-repo` (the sibling Go
  reference implementation's checkout)
- **Why no integration golden:** identical reasoning to
  `index/lci-cpp-repo`. The corpus is a live checkout maintained
  outside this repo (`/home/beagle/work/core/lci`), so its file count
  is doubly volatile — it shifts whenever the Go port lands a commit,
  with no signal to this repo's tree that a golden refresh is due.
- **Coverage:** same as above — the parity oracle keeps the
  cross-binary check; integration intentionally skips.

## Migrated

### `index/synthetic-multilang`
- **Source:** `tests/parity/descriptors/index/synthetic-multilang.parity.json`
- **Integration spec:** `tests/integration/index/synthetic-multilang.spec.json`
- **Golden:** `tests/integration/goldens/index/synthetic-multilang.json` (`{}`)
- **Why migrated:** the corpus is a fixed checked-in fixture under
  `tests/parity/corpora/synthetic/multi-lang/`, so the C++ `debug
  export` output is reproducible from one CI run to the next — the
  exit-code-level Decision C check is meaningful here.

## Future tightening (out of scope for migration 5/8)

For `index/synthetic-multilang` it would be possible to promote
`file_count` and `symbol_count` from `ignore` to `stable` since the
synthetic corpus is fixed. That requires also threading a running
server (the standalone `lci debug export` writes zeros when no server
is listening, see `runner/modes/index.cpp`), so it is deferred to a
follow-up task and not part of the mass-migrate.
