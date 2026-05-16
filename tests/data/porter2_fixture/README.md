# Porter2 Three-Way Byte-Equivalence Fixture

`voc.txt` and `output.txt` are the canonical Snowball English Porter2
acceptance fixture, **identical bytes** to:

- Upstream Snowball: `snowballstem/snowball/blob/master/algorithms/english/`
  (referenced by the libstemmer test suite).
- Go surgebase/porter2 v0.0.0-20150829210152:
  `github.com/surgebase/porter2/{voc.txt,output.txt}` —
  `porter2_test.go` walks these files line-by-line and fails on any
  mismatch.

Because both libstemmer (Snowball upstream) and Go surgebase/porter2 ship
this same fixture as their own acceptance test, asserting
`lci::porter2_stem(voc.txt[i]) == output.txt[i]` for all 29,417 lines
transitively proves:

```
libstemmer == Go surgebase/porter2 == lci::Stemmer
```

This is the three-way agreement required by the task description.

## Source

Copied verbatim (no edits) from
`github.com/surgebase/porter2@v0.0.0-20150829210152-56e4718818e8/`
(which itself sourced them from snowballstem.org). License: BSD-3-Clause
(same as Snowball upstream).

## Test

`tests/semantic_test.cpp :: PorterFixtureTest` reads both files and
asserts byte-equivalence for every line. Run via:

```
ctest -R semantic_test
# or
./build/tests/lci_tests --gtest_filter=PorterFixtureTest*
```

## Line count

29,417 lines per file. First line of `voc.txt` is the empty string (`""`
in the file representation) — `output.txt` has the same empty line in the
same position.
