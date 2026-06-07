# Spec-backed integration tests

This directory carries the C++ self-test suite: each spec runs the C++ `lci`
binary and diffs its captured output against a checked-in golden. (The Go
reference has been retired — the migration is complete — so these goldens are
the source of truth, not a Go oracle.)

## Pattern

1. Write a spec under `integration/<surface>/<name>.spec.json` describing the
   invocation (args + stdin) and the tier/ignore rules.
2. Run the C++ side from `lci_integration_tests`.
3. Compare the captured output against a checked-in golden under
   `integration/goldens/...` with `spec_diff::assert_matches`. Regenerate
   goldens with `LCI_UPDATE_GOLDENS=1`.

The reusable helper is `integration/spec_runner.{h,cpp}`.

## Goldens

Goldens live under `tests/integration/goldens/` and store the canonical form of
the C++ output. The spec runner normalizes the live output with the descriptor
rules, then compares that result with the checked-in golden through
`spec_diff::assert_matches`. This keeps the files small, stable, and portable
across machines.

## Update workflow

Rebuild the integration target, then rerun the migrated tests with
`LCI_UPDATE_GOLDENS=1` to refresh the checked-in canonical outputs:

```bash
cmake --build build/debug -j$(nproc) --target lci_integration_tests
LCI_UPDATE_GOLDENS=1 ctest --test-dir build/debug -R 'SpecMigrationTest' --output-on-failure
```

Run the same command again without `LCI_UPDATE_GOLDENS` to verify the fresh
goldens:

```bash
ctest --test-dir build/debug -R 'SpecMigrationTest' --output-on-failure
```

## Choosing migration candidates

Prefer descriptors that are already green in parity, deterministic, and narrow
enough to diagnose from a single golden. Do not close older legacy `LCI:` issues
just because a migrated spec exists; only close parity-specific Dart work when a
golden-backed replacement is proven green and clearly covers that issue.
