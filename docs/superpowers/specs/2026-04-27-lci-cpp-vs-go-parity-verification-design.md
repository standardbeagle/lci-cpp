# LCI C++ vs Go Parity Verification — Design

**Date:** 2026-04-27
**Status:** Approved design, ready for plan
**Scope:** Systematic side-by-side verification of `lci-cpp` against the Go `lci`
implementation, with module-level comparison where it makes sense.

## Goals

1. Detect any divergence between the C++ port and the Go reference: CLI output,
   MCP tool JSON, indexed data, and core algorithms (trigrams, scoring, walker,
   linker, semantic annotations, regex analyzer).
2. Run automatically under `ctest -L parity` with hard failures and full diff
   dumps for triage.
3. Use real codebases plus synthetic fixtures, with strictness tiered to
   tolerate ranked/timed/id fields without losing signal on stable fields.
4. Module comparison is opportunistic — only where algorithms must match.
   Skip places where C++ has different data types or has merged/split
   responsibilities; trust end-to-end and probe-level diffs to surface
   regressions.

## Non-goals

- Performance comparison (covered by existing `scripts/benchmark-compare.sh`).
- Exhaustive Go ↔ C++ public API symbol diff. Heavy, low-signal.
- Replacing existing unit/integration tests inside `tests/`.
- Cross-platform parity (Linux only for now; macOS/Windows out of scope).

## Layout

```
lci-cpp/tests/parity/
  MODULE_MAP.md              # Go pkg → C++ dir, probe coverage status
  README.md                  # how to run, how to triage failures
  diff_engine/
    canonicalize.{h,cpp}     # JSON key-sort, number normalize, path rewrite
    field_tier.{h,cpp}       # JSONPath → tier classifier
    diff.{h,cpp}             # tiered comparator, unified-diff emitter
  runner/
    parity_runner.cpp        # spawn both binaries, capture, diff, dump
    descriptor.{h,cpp}       # parse .parity.json descriptors
    modes/
      cli.cpp                # fork+exec, capture stdio
      mcp.cpp                # long-lived JSON-RPC stdio session
      http.cpp               # ephemeral server + libcurl
      index.cpp              # post-index `debug export --json`
  descriptors/
    cli/                     # one .parity.json per CLI test
    mcp/                     # one per MCP tool × input
    http/                    # one per HTTP endpoint (optional, Phase 6)
    index/                   # debug-export diffs per corpus
    probes/                  # algorithmic probes (§Algorithmic targets)
  corpora/
    synthetic/               # tiny hand-built repos checked in
    prep_real.sh             # symlink real repos into corpora/
  CMakeLists.txt             # one ctest per descriptor, label "parity"

build/parity-failures/<test_id>/
  desc.json                  # copy of descriptor
  go.raw, cpp.raw            # uninterpreted captured output
  go.canon.json, cpp.canon.json
  diff.txt                   # unified diff of canon
  report.txt                 # per-tier reason breakdown
```

## Descriptor format

One JSON shape drives every test type. Runner mode-agnostic.

```json
{
  "id": "cli/search/json-basic",
  "mode": "cli",
  "corpus": "lci-go-repo",
  "go_binary": "${LCI_GO}",
  "cpp_binary": "${LCI_CPP}",
  "invocation": {
    "args": ["search", "--json", "MasterIndex"],
    "stdin": null,
    "env": {"LCI_NO_DAEMON": "1"},
    "cwd": "${CORPUS}"
  },
  "capture": ["stdout", "exit"],
  "parse": "json",
  "tiers": {
    "stable":   ["results[].file", "results[].line", "results[].symbol", "total"],
    "ranked":   ["results[].score"],
    "timed":    ["elapsed_ms"],
    "ids":      ["request_id"],
    "ignore":   ["server_pid", "version"]
  },
  "tolerances": {
    "score_abs": 0.01,
    "timed_max_ms": 60000
  },
  "expect_exit": 0
}
```

### Tier semantics

- **stable** — canonicalize → byte-exact deep-equal. Any drift = fail.
- **ranked** — multiset match by stable composite key (e.g., `file+line+symbol`).
  Score within `score_abs`. Order may differ within ranks of equal score.
- **timed** — type-check + range-check `[0, timed_max_ms]`. Value otherwise free.
- **ids** — regex/format match only. Value otherwise free.
- **ignore** — strip before diff.
- **unspecified field** — defaults to **stable** (fail-closed).

### Canonicalize rules

- JSON: recursive object key sort.
- Numbers: floats → `%.6g`; ints unchanged.
- Strings: trim trailing whitespace per line.
- Paths: rewrite absolute corpus prefix to `${CORPUS}` (handles `/tmp/...` vs
  symlinks vs absolute paths).

## Diff engine pipeline

```
raw_go, raw_cpp
  ↓ parse (json | text | exit-only)
  ↓ canonicalize (key-sort, num-normalize, path-rewrite, strip ignore[])
  ↓ tier classify (JSONPath match against tiers{})
  ↓ compare per tier
       stable: deep-equal      → mismatch list
       ranked: multiset-by-key → missing/extra/score-drift list
       timed:  type+range      → fail if NaN/<0/>max
       ids:    regex match     → fail if format mismatch
  ↓
PASS  |  FAIL{reasons[], unified_diff, dump_paths}
```

## Runner

```
parity_runner <descriptor.json>
  resolves ${LCI_GO}, ${LCI_CPP}, ${CORPUS}
  mode=cli:   fork+exec, capture stdout/stderr/exit
  mode=mcp:   open 2 long-lived stdio sessions, JSON-RPC initialize, then call
  mode=http:  start each server in ephemeral runtime dir, libcurl over Unix
              socket, shutdown
  mode=index: invoke `lci debug export --json` post-index, diff
  invokes diff engine
  exit 0 = pass, 1 = mismatch, 2 = infra error (binary missing, timeout, crash)
```

### Server isolation per HTTP/MCP test

- Per-test runtime dir `${TMP}/parity-<pid>-<test_id>/`
- Override socket path env so Go and C++ servers don't collide.
- Tear down: `lci shutdown` → `rm -rf` runtime dir.
- Timeout 60s per phase; SIGKILL on timeout, mark infra error (not mismatch).

## ctest wiring

```cmake
file(GLOB_RECURSE PARITY_DESCS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/descriptors/*.parity.json)

foreach(D ${PARITY_DESCS})
  file(RELATIVE_PATH NAME
       ${CMAKE_CURRENT_SOURCE_DIR}/descriptors ${D})
  string(REPLACE "/" "." TID ${NAME})
  string(REPLACE ".parity.json" "" TID ${TID})
  add_test(NAME parity.${TID}
           COMMAND parity_runner ${D}
           WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(parity.${TID} PROPERTIES
    LABELS "parity"
    ENVIRONMENT
      "LCI_GO=${LCI_GO_PATH};LCI_CPP=$<TARGET_FILE:lci>"
    TIMEOUT 120)
endforeach()
```

Run: `ctest -L parity -j$(nproc) --output-on-failure`

## Corpora

Four corpora, set up by `corpora/prep_real.sh` (idempotent):

| Key | Source | Purpose |
|-----|--------|---------|
| `synthetic` | `corpora/synthetic/` (checked in) | Tiny, deterministic, fast |
| `lci-go-repo` | symlink `/home/beagle/work/core/lci` | Real Go code, multi-language minimal |
| `lci-cpp-repo` | symlink `/home/beagle/work/core/lci-cpp` | Heavy tree-sitter, C++ code |
| `lci-test` | symlink `/home/beagle/work/core/lci-test` | LongBench, repobench-eval fixtures |

Synthetic fixtures cover: empty repo, single file, multi-language mix,
unicode names, deeply nested dirs, symlinks, large file (>1 MB),
binary-disguised-as-text, near-duplicates (trigram edge cases).

## Module mapping (lightweight)

`tests/parity/MODULE_MAP.md` is a one-page orientation table. Columns:

| Go pkg | C++ home | Mapping kind | Has algo probe? | Notes |

Mapping kinds: `mapped` (1↔1), `merged` (Go N→C++ 1), `split` (Go 1→C++ N),
`removed` (intentionally dropped).

No per-package symbol extraction. No type-alias dictionary. No `diff_api.cpp`.
Module comparison is opportunistic; the table exists for orientation and to
track which targets have algorithmic probes (next section).

## Algorithmic parity targets

Algorithms expected to produce identical output regardless of internal types.
Each target = one descriptor with `mode: cli` driving a `lci debug …` probe.

| Target | Probe (both sides expose) | Why must match |
|--------|---|---|
| Trigram extraction | `lci debug trigrams <input>` → sorted trigram list | Same input → same set, else search recall diverges |
| Score components | `lci debug score <query> <doc-id>` → BM25/IDF/weights | Ranking parity |
| File walker filters | `lci debug walk <dir>` → included paths | Include/exclude rules |
| Symbol linker | `lci debug link <file>` → name→def edges | Refs/defs accuracy |
| Semantic annotations | `lci debug annotate <symbol>` → labels | Tool output parity |
| Regex analyzer | `lci debug regex-analyze <pattern>` → literal/prefix/anchor flags | Search planner pre-filter |
| ID codec | `lci debug encode-id <symbol>` / `decode-id <id>` | Cross-binary id stability if exposed |
| Git scope | `lci git-analyze --scope wip --json` (existing) | Already E2E-covered |
| Index export | `lci debug export --json` (existing) | Symbols/refs/file/trigram-count parity |

If a probe is missing on one side → file as `MODULE_MAP.md` backlog row, skip
that target until probe is added on both sides.

## Phasing and acceptance

### Phase 0 — Bootstrap
- Build C++ release; resolve Go binary path.
- `corpora/prep_real.sh` symlinks all four corpora.
- Skeleton dirs + CMakeLists.
- **Acceptance:** `ctest -L parity` runs, finds zero tests, exits 0.

### Phase 1 — Diff engine + runner (CLI mode)
- Canonicalize, tier classifier, diff implemented.
- `parity_runner` supports `mode: cli`.
- One smoke descriptor: `lci --version` (text, exit-only).
- **Acceptance:** smoke passes; injecting a stdout corruption fails with dump
  in `build/parity-failures/`.

### Phase 2 — CLI parity, broad shallow
- Descriptors for: search (6 variants), grep, def, refs, tree, list, symbols,
  inspect, browse, config show/validate, debug info/validate, git-analyze.
- Across all 4 corpora.
- **Acceptance:** all green or diff dumps drive a tracked bug list.

### Phase 3 — MCP parity
- Runner gains `mode: mcp` (long-lived stdio session, initialize + call).
- All 17 tools × ≥3 inputs each.
- **Acceptance:** all green or allowlisted.

### Phase 4 — Index/data parity
- `mode: index`: index corpus on both, `debug export --json`, diff.
- Per corpus: file table, symbol table, ref table, trigram count, version.
- **Acceptance:** structural diff clean.

### Phase 5 — Algorithmic probes
- For each target with probe both sides: descriptor + run.
- For each missing probe: backlog row in `MODULE_MAP.md`.
- **Acceptance:** all present-probe targets green.

### Phase 6 — HTTP parity (optional)
- `mode: http`, 15 endpoints, ephemeral server lifecycle.
- **Acceptance:** all green.

### Done criteria
- Phase 0–5 green on `synthetic` + `lci-go-repo` + `lci-cpp-repo`.
- `MODULE_MAP.md` lists all algorithmic targets with probe status.
- Failure dump triage workflow documented in `tests/parity/README.md`.
- CI: `ctest -L parity` part of standard test run.

## Risks and mitigations

| Risk | Mitigation |
|------|------------|
| Probes don't exist on Go side | File backlog row; skip target until added on both sides. Don't block other phases. |
| Trigram extraction order non-deterministic | Tier as `ranked`, key by trigram string |
| HTTP/MCP socket collisions | Per-test ephemeral runtime dir + env override |
| MCP server warmup timing affects diffs | `initialize` handshake completes before any timing capture |
| `${CORPUS}` path drift across hosts | Path-rewrite step in canonicalize normalizes absolute corpus prefix |
| Score drift due to float arithmetic order | `score_abs` tolerance + ranked tier; if drift exceeds tolerance, treat as bug |
| Real-repo corpora change under us | Symlinks; tests should not depend on commit-specific symbol counts; use search predicates that select stable patterns |
| `debug export` schema differs across versions | Version pinned in descriptor; bump descriptor when schema bumps |

## Future work (out of scope for this design)

- macOS/Windows parity runs.
- Performance regression detection bolted into the same harness.
- API surface diff (deferred unless E2E + probes leave gaps).
- Fuzz-style query generation against both binaries.
