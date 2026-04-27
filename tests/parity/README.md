# LCI Parity Harness

Side-by-side verification of the C++ `lci` port against the Go reference.

## Run

    cmake --preset debug
    cmake --build build/debug -j$(nproc)
    ctest --test-dir build/debug -L parity --output-on-failure -j$(nproc)

## Triage failures

When a parity test fails, the runner writes a dump to
`build/debug/parity-failures/<test_id>/`:

| File | Contents |
|------|----------|
| `desc.json` | the descriptor used |
| `go.raw`, `cpp.raw` | uninterpreted captured output from each binary |
| `go.canon.json`, `cpp.canon.json` | post-canonicalize structures |
| `diff.txt` | unified diff of canon |
| `report.txt` | per-tier reason breakdown |

See `docs/superpowers/specs/2026-04-27-lci-cpp-vs-go-parity-verification-design.md` for tier semantics.
