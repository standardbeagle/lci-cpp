# Fuzz targets

libFuzzer targets over LCI's highest-complexity, untrusted-input surfaces.
Clang-only (libFuzzer + ASan/UBSan). The whole `fuzz` build is instrumented
uniformly — mixing an ASan fuzz TU with a non-ASan abseil aborts on the first
`flat_hash_map` (abseil container poisoning), so `lci_lib` and every vendored
dep are built with `-fsanitize=address,undefined,fuzzer-no-link` via the preset;
each fuzz executable only adds the libFuzzer driver (`-fsanitize=fuzzer`).

## Targets

| Target | Surface |
|---|---|
| `fuzz_trigram_extract` | ASCII/Unicode trigram extraction |
| `fuzz_search_input` | search query validation + execution |
| `fuzz_symbol_extract` | tree-sitter symbol extraction across 13 languages |
| `fuzz_mcp_dispatch` | full MCP stdio/JSON-RPC boundary → all 14 tools + arg guard |
| `fuzz_find_files` | glob / substring / path / Levenshtein-fuzzy path matcher |
| `fuzz_get_context` | object-ID base-63 codec + param normalization + call hierarchy |

## Build

```sh
# 'clang'/'clang++' must be on PATH; override if only versioned clang exists:
cmake --preset fuzz -DCMAKE_C_COMPILER=clang-17 -DCMAKE_CXX_COMPILER=clang++-17
cmake --build build/fuzz -j --target fuzz_mcp_dispatch   # or any target
```

## Run

```sh
# Ad-hoc fuzzing (seed corpus is committed under tests/fuzz/corpus/<target>/):
./build/fuzz/tests/fuzz_get_context tests/fuzz/corpus/fuzz_get_context

# Reproduce a saved crash:
./build/fuzz/tests/fuzz_get_context crash-<hash>

# Bounded smoke runs as ctest (LABELS=fuzz), what CI would gate on:
ctest --test-dir build/fuzz -L fuzz --output-on-failure
```

Smoke tests write new inputs to `build/fuzz/tests/fuzz_corpus/<target>/`
(throwaway); the committed `tests/fuzz/corpus/<target>/` seeds are read-only.

## Corpus

Committed seeds are minimal regression anchors, including reproducers for bugs
these targets already found (e.g. `fuzz_get_context/crash_invalid_utf8_0x8a` —
the non-UTF-8 byte that threw in the JSON serializer, now fixed via
`dump_json_lossy`). Add a seed when a target finds a real crash.
