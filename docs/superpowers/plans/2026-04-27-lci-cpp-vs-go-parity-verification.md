# LCI C++ vs Go Parity Verification — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a side-by-side parity harness under `tests/parity/` that drives both the Go `lci` reference binary and the new C++ `lci` port through identical inputs, then diffs outputs with field-tier strictness, with hard-fail and full diff dump on mismatch.

**Architecture:** A small C++ runner (`parity_runner`) takes a JSON descriptor, spawns both binaries (CLI / MCP / HTTP / index export modes), captures output, runs it through a tiered diff engine (canonicalize → tier-classify → compare), and writes failure dumps to `build/parity-failures/`. CTest globs descriptors and creates one test per descriptor under label `parity`.

**Tech Stack:** C++20, GoogleTest, nlohmann_json, CLI11, cpp-httplib (already in vcpkg), POSIX `fork`/`exec` for subprocess control.

**Spec:** `docs/superpowers/specs/2026-04-27-lci-cpp-vs-go-parity-verification-design.md`

---

## File Structure

**Created (under `tests/parity/`):**

```
tests/parity/
  CMakeLists.txt                       # builds parity_runner, globs descriptors
  README.md                            # how to run, how to triage
  MODULE_MAP.md                        # Go pkg → C++ dir + probe coverage

  diff_engine/
    canonicalize.h / canonicalize.cpp  # JSON key-sort, num normalize, path rewrite
    field_tier.h   / field_tier.cpp    # JSONPath-lite → tier classifier
    diff.h         / diff.cpp          # tiered comparator, unified-diff emitter

  runner/
    descriptor.h   / descriptor.cpp    # parse .parity.json
    parity_runner.cpp                  # main() — selects mode, drives diff
    modes/
      cli.h    / cli.cpp               # fork+exec, capture stdio
      mcp.h    / mcp.cpp               # long-lived JSON-RPC stdio session
      http.h   / http.cpp              # ephemeral server + httplib client
      index.h  / index.cpp             # `lci debug export --json` post-index

  descriptors/
    cli/*.parity.json
    mcp/*.parity.json
    index/*.parity.json
    probes/*.parity.json
    http/*.parity.json                 # Phase 6, optional

  corpora/
    prep_real.sh                       # symlink real repos
    synthetic/                         # checked-in fixtures
      empty/                           # empty repo
      single-file/main.go
      multi-lang/{a.go,b.py,c.cpp,d.rs}
      unicode-names/...
      nested-deep/.../leaf.go
      large-file/big.cpp               # >1 MB

  unit_tests/
    canonicalize_test.cpp
    field_tier_test.cpp
    diff_test.cpp
    descriptor_test.cpp
```

**Modified:**

- `tests/CMakeLists.txt` — `add_subdirectory(parity)`
- `CMakeLists.txt` (root) — add `LCI_GO_PATH` cache variable for Go binary discovery

---

## Conventions used in this plan

- **Test framework:** GoogleTest (already wired in `tests/CMakeLists.txt`).
- **JSON library:** `nlohmann::json` (already a dependency).
- **Commit messages:** Conventional Commits, e.g. `feat(parity): add canonicalize`.
- **Build:** `cmake --preset debug && cmake --build build/debug -j$(nproc)` for fast iteration.
- **Run a single unit test:** `ctest --test-dir build/debug -R parity_canonicalize_test --output-on-failure`.

---

## Phase 0 — Bootstrap

### Task 0.1: Create directory skeleton and parity CMakeLists

**Files:**
- Create: `tests/parity/CMakeLists.txt`
- Create: `tests/parity/README.md`
- Modify: `tests/CMakeLists.txt` (append one line)
- Modify: `CMakeLists.txt` (root, add LCI_GO_PATH cache var)

- [ ] **Step 1: Create the parity CMakeLists with placeholder body**

`tests/parity/CMakeLists.txt`:

```cmake
# tests/parity/CMakeLists.txt
# Parity harness: drives both Go and C++ lci binaries side-by-side.

# Locate Go reference binary
if(NOT DEFINED LCI_GO_PATH)
    find_program(LCI_GO_PATH NAMES lci-linux-amd64 lci HINTS
        /home/beagle/work/core/lci
        /usr/local/bin
        ENV PATH)
endif()

if(NOT LCI_GO_PATH)
    message(WARNING "LCI_GO_PATH not set and Go lci binary not found — parity tests will be skipped")
    return()
endif()
message(STATUS "Parity: using Go binary at ${LCI_GO_PATH}")

# ---- Diff engine library (pure functions, unit-testable) ----
add_library(lci_parity_diff STATIC
    diff_engine/canonicalize.cpp
    diff_engine/field_tier.cpp
    diff_engine/diff.cpp
)
target_include_directories(lci_parity_diff PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(lci_parity_diff PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(lci_parity_diff PUBLIC cxx_std_20)

# ---- Runner library (subprocess + mode handlers) ----
add_library(lci_parity_runner_lib STATIC
    runner/descriptor.cpp
    runner/modes/cli.cpp
    runner/modes/mcp.cpp
    runner/modes/http.cpp
    runner/modes/index.cpp
)
target_include_directories(lci_parity_runner_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(lci_parity_runner_lib PUBLIC
    lci_parity_diff
    nlohmann_json::nlohmann_json
    httplib::httplib
)

# ---- Runner executable ----
add_executable(parity_runner runner/parity_runner.cpp)
target_link_libraries(parity_runner PRIVATE
    lci_parity_runner_lib
    CLI11::CLI11
)

# ---- Unit tests for the diff engine ----
add_executable(parity_unit_tests
    unit_tests/canonicalize_test.cpp
    unit_tests/field_tier_test.cpp
    unit_tests/diff_test.cpp
    unit_tests/descriptor_test.cpp
)
target_link_libraries(parity_unit_tests PRIVATE
    lci_parity_diff
    lci_parity_runner_lib
    GTest::gtest_main
)
add_test(NAME parity_unit_tests COMMAND parity_unit_tests)
set_tests_properties(parity_unit_tests PROPERTIES LABELS "parity")

# ---- Per-descriptor parity tests ----
file(GLOB_RECURSE PARITY_DESCS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/descriptors/*.parity.json)

foreach(D ${PARITY_DESCS})
    file(RELATIVE_PATH NAME ${CMAKE_CURRENT_SOURCE_DIR}/descriptors ${D})
    string(REPLACE "/" "." TID ${NAME})
    string(REPLACE ".parity.json" "" TID ${TID})
    add_test(NAME parity.${TID}
             COMMAND parity_runner ${D}
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(parity.${TID} PROPERTIES
        LABELS "parity"
        ENVIRONMENT "LCI_GO=${LCI_GO_PATH};LCI_CPP=$<TARGET_FILE:lci>;PARITY_CORPORA=${CMAKE_CURRENT_SOURCE_DIR}/corpora;PARITY_FAILURES=${CMAKE_BINARY_DIR}/parity-failures"
        TIMEOUT 120)
endforeach()
```

`tests/parity/README.md`:

```markdown
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
```

`tests/CMakeLists.txt` (append at end):

```cmake
add_subdirectory(parity)
```

`CMakeLists.txt` (root) — add near top after `LCI_UNIVERSAL_BINARY` option:

```cmake
set(LCI_GO_PATH "" CACHE FILEPATH "Path to Go reference lci binary for parity tests")
```

- [ ] **Step 2: Configure and verify zero parity tests are registered**

```bash
cmake --preset debug
cmake --build build/debug --target lci -j$(nproc)
ctest --test-dir build/debug -L parity -N
```

Expected: `Total Tests: 0` (or only `parity_unit_tests` once it's wired but no source files yet — that's fine for the next step). Build target `lci` builds. No errors in CMake configure.

- [ ] **Step 3: Create empty source stubs so the build doesn't break**

Create empty files (CMake `add_library`/`add_executable` need source files to exist):

```bash
mkdir -p tests/parity/diff_engine tests/parity/runner/modes tests/parity/unit_tests \
    tests/parity/descriptors tests/parity/corpora/synthetic
touch tests/parity/diff_engine/{canonicalize,field_tier,diff}.cpp \
      tests/parity/diff_engine/{canonicalize,field_tier,diff}.h \
      tests/parity/runner/{descriptor.cpp,descriptor.h,parity_runner.cpp} \
      tests/parity/runner/modes/{cli,mcp,http,index}.cpp \
      tests/parity/runner/modes/{cli,mcp,http,index}.h \
      tests/parity/unit_tests/{canonicalize,field_tier,diff,descriptor}_test.cpp
```

Add `int main(int, char**) { return 0; }` to `parity_runner.cpp` so it links:

```cpp
// tests/parity/runner/parity_runner.cpp
int main(int /*argc*/, char** /*argv*/) { return 0; }
```

- [ ] **Step 4: Build and verify CMake reconfigure works**

```bash
cmake --build build/debug -j$(nproc)
```

Expected: builds `parity_runner` and `parity_unit_tests` successfully. The unit tests will fail to link if there's no `main()` from `gtest_main` and no test cases — that's fine because `gtest_main` provides `main`. Empty test executable should run and exit 0:

```bash
./build/debug/tests/parity/parity_unit_tests
echo "exit=$?"
```

Expected: `exit=0` (gtest reports "Ran 0 tests").

- [ ] **Step 5: Commit**

```bash
git add tests/parity CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(parity): scaffold parity harness directory and CMake wiring"
```

---

### Task 0.2: Corpus prep script and synthetic fixtures

**Files:**
- Create: `tests/parity/corpora/prep_real.sh`
- Create: `tests/parity/corpora/synthetic/empty/.gitkeep`
- Create: `tests/parity/corpora/synthetic/single-file/main.go`
- Create: `tests/parity/corpora/synthetic/multi-lang/{a.go,b.py,c.cpp,d.rs}`
- Create: `tests/parity/corpora/synthetic/unicode-names/héllo.go`
- Create: `tests/parity/corpora/synthetic/nested-deep/a/b/c/d/leaf.go`
- Create: `tests/parity/corpora/synthetic/.gitignore` (ignore real-repo symlinks)

- [ ] **Step 1: Write the prep script**

`tests/parity/corpora/prep_real.sh`:

```bash
#!/usr/bin/env bash
# Symlinks the three real-repo corpora into tests/parity/corpora/.
# Idempotent: re-running is a no-op if the symlinks already point correctly.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

link_if_needed() {
    local target="$1" link="$2"
    if [[ ! -e "$target" ]]; then
        echo "WARN: $target missing — skipping $link"
        return 0
    fi
    if [[ -L "$link" && "$(readlink "$link")" == "$target" ]]; then
        return 0
    fi
    rm -f "$link"
    ln -s "$target" "$link"
    echo "linked: $link -> $target"
}

link_if_needed "/home/beagle/work/core/lci"      "$HERE/lci-go-repo"
link_if_needed "/home/beagle/work/core/lci-cpp"  "$HERE/lci-cpp-repo"
link_if_needed "/home/beagle/work/core/lci-test" "$HERE/lci-test"

echo "OK"
```

Make it executable:

```bash
chmod +x tests/parity/corpora/prep_real.sh
```

- [ ] **Step 2: Add synthetic fixtures**

`tests/parity/corpora/synthetic/empty/.gitkeep` — empty file.

`tests/parity/corpora/synthetic/single-file/main.go`:

```go
package main

import "fmt"

func main() {
    fmt.Println("hello")
}
```

`tests/parity/corpora/synthetic/multi-lang/a.go`:

```go
package multi

func Add(a, b int) int { return a + b }
```

`tests/parity/corpora/synthetic/multi-lang/b.py`:

```python
def add(a: int, b: int) -> int:
    return a + b
```

`tests/parity/corpora/synthetic/multi-lang/c.cpp`:

```cpp
int add(int a, int b) { return a + b; }
```

`tests/parity/corpora/synthetic/multi-lang/d.rs`:

```rust
pub fn add(a: i32, b: i32) -> i32 { a + b }
```

`tests/parity/corpora/synthetic/unicode-names/héllo.go`:

```go
package héllo

func Greet() string { return "héllo" }
```

`tests/parity/corpora/synthetic/nested-deep/a/b/c/d/leaf.go`:

```go
package leaf

func Leaf() {}
```

`tests/parity/corpora/synthetic/.gitignore`:

```
# Real-repo symlinks created by prep_real.sh — not committed
/lci-go-repo
/lci-cpp-repo
/lci-test
```

- [ ] **Step 3: Run prep and verify symlinks work**

```bash
./tests/parity/corpora/prep_real.sh
ls -l tests/parity/corpora/
```

Expected: three symlinks (`lci-go-repo`, `lci-cpp-repo`, `lci-test`) plus `synthetic/` dir.

- [ ] **Step 4: Commit**

```bash
git add tests/parity/corpora
git commit -m "feat(parity): add corpus prep script and synthetic fixtures"
```

---

## Phase 1 — Diff engine (pure logic, TDD)

### Task 1.1: Canonicalize — JSON key-sort, number normalize, path rewrite

**Files:**
- Create: `tests/parity/diff_engine/canonicalize.h`
- Create: `tests/parity/diff_engine/canonicalize.cpp`
- Create: `tests/parity/unit_tests/canonicalize_test.cpp`

- [ ] **Step 1: Write the failing tests**

`tests/parity/unit_tests/canonicalize_test.cpp`:

```cpp
#include "diff_engine/canonicalize.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using lci::parity::canonicalize_json;
using lci::parity::CanonicalizeOptions;
using nlohmann::json;

TEST(CanonicalizeJson, SortsObjectKeysRecursively) {
    auto in = json::parse(R"({"b":1,"a":{"y":2,"x":1}})");
    auto out = canonicalize_json(in, {});
    // The dump string is the canonical form
    EXPECT_EQ(out.dump(), R"({"a":{"x":1,"y":2},"b":1})");
}

TEST(CanonicalizeJson, NormalizesFloatsToSixSignificantDigits) {
    auto in = json::parse(R"({"score":0.123456789})");
    auto out = canonicalize_json(in, {});
    // 0.123457 — %.6g
    EXPECT_EQ(out["score"].get<std::string>(), "0.123457");
}

TEST(CanonicalizeJson, PreservesNumbersOnPreservedPaths) {
    auto in = json::parse(R"({"results":[{"score":0.987654}]})");
    CanonicalizeOptions opts;
    opts.preserve_number_paths = {"results[].score"};
    auto out = canonicalize_json(in, opts);
    // Still a number — diff engine ranked tier will apply tolerance.
    EXPECT_TRUE(out["results"][0]["score"].is_number());
    EXPECT_NEAR(out["results"][0]["score"].get<double>(), 0.987654, 1e-9);
}

TEST(CanonicalizeJson, KeepsIntsUnchanged) {
    auto in = json::parse(R"({"n":42})");
    auto out = canonicalize_json(in, {});
    EXPECT_EQ(out["n"].get<int64_t>(), 42);
}

TEST(CanonicalizeJson, StripsIgnoredJsonPaths) {
    auto in = json::parse(R"({"a":1,"server_pid":1234,"version":"x"})");
    CanonicalizeOptions opts;
    opts.ignore_paths = {"server_pid", "version"};
    auto out = canonicalize_json(in, opts);
    EXPECT_FALSE(out.contains("server_pid"));
    EXPECT_FALSE(out.contains("version"));
    EXPECT_EQ(out["a"].get<int>(), 1);
}

TEST(CanonicalizeJson, RewritesAbsoluteCorpusPathsToToken) {
    auto in = json::parse(R"({"results":[{"file":"/tmp/corpus-abc/src/a.go"}]})");
    CanonicalizeOptions opts;
    opts.corpus_prefix = "/tmp/corpus-abc";
    auto out = canonicalize_json(in, opts);
    EXPECT_EQ(out["results"][0]["file"].get<std::string>(),
              "${CORPUS}/src/a.go");
}

TEST(CanonicalizeText, TrimsTrailingWhitespacePerLine) {
    using lci::parity::canonicalize_text;
    EXPECT_EQ(canonicalize_text("a   \nb\t \nc"), "a\nb\nc");
}
```

- [ ] **Step 2: Run tests — they should fail to compile (header missing)**

```bash
cmake --build build/debug --target parity_unit_tests -j$(nproc) 2>&1 | head -20
```

Expected: compile errors mentioning `canonicalize.h` not found or undefined symbols.

- [ ] **Step 3: Write the header**

`tests/parity/diff_engine/canonicalize.h`:

```cpp
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace lci::parity {

struct CanonicalizeOptions {
    // JSONPath-lite expressions to strip before comparison.
    // Supports literal field paths like "results[].file" or "server_pid".
    std::vector<std::string> ignore_paths;

    // Paths whose numeric values must be preserved as numbers (not
    // stringified). Typically the union of ranked + timed paths from
    // the descriptor's tier map, so the diff engine can apply tolerances.
    std::vector<std::string> preserve_number_paths;

    // Absolute corpus prefix to rewrite to "${CORPUS}".
    // Empty string disables rewriting.
    std::string corpus_prefix;
};

// Canonicalize a JSON value:
//   - Object keys recursively sorted (handled implicitly by nlohmann::json::dump
//     with no flags — but we walk and re-emit explicitly to be deterministic).
//   - Floats normalized to "%.6g" string form (stored as JSON string).
//   - String values inside objects/arrays get corpus-prefix rewrite if non-empty.
//   - JSONPath-lite ignore_paths are stripped.
nlohmann::json canonicalize_json(const nlohmann::json& in,
                                 const CanonicalizeOptions& opts);

// Canonicalize plain text: trim trailing whitespace per line. Preserve the
// trailing newline policy of the input (no newline added or removed at EOF).
std::string canonicalize_text(std::string_view in);

} // namespace lci::parity
```

- [ ] **Step 4: Write the minimal implementation**

`tests/parity/diff_engine/canonicalize.cpp`:

```cpp
#include "diff_engine/canonicalize.h"

#include <cstdio>
#include <sstream>

namespace lci::parity {

namespace {

// Returns true if `path` matches any of the given JSONPath-lite expressions.
// Patterns supported:
//   "field"               — exact top-level field name
//   "a.b"                 — nested field
//   "results[].file"      — array element field (matches any index)
bool path_matches(const std::vector<std::string>& patterns,
                  const std::string& path) {
    for (const auto& p : patterns) {
        if (p == path) return true;
    }
    return false;
}

void strip_paths_recursive(nlohmann::json& node,
                           const std::vector<std::string>& patterns,
                           std::string current_path) {
    if (node.is_object()) {
        std::vector<std::string> to_remove;
        for (auto it = node.begin(); it != node.end(); ++it) {
            std::string child_path =
                current_path.empty() ? it.key() : current_path + "." + it.key();
            if (path_matches(patterns, child_path)) {
                to_remove.push_back(it.key());
            } else {
                strip_paths_recursive(it.value(), patterns, child_path);
            }
        }
        for (const auto& k : to_remove) node.erase(k);
    } else if (node.is_array()) {
        std::string array_path = current_path + "[]";
        for (auto& elem : node) {
            strip_paths_recursive(elem, patterns, array_path);
        }
    }
}

void rewrite_paths_recursive(nlohmann::json& node,
                             const std::string& corpus_prefix) {
    if (corpus_prefix.empty()) return;
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            rewrite_paths_recursive(it.value(), corpus_prefix);
        }
    } else if (node.is_array()) {
        for (auto& elem : node) rewrite_paths_recursive(elem, corpus_prefix);
    } else if (node.is_string()) {
        std::string s = node.get<std::string>();
        if (s.size() >= corpus_prefix.size() &&
            s.compare(0, corpus_prefix.size(), corpus_prefix) == 0) {
            node = std::string("${CORPUS}") + s.substr(corpus_prefix.size());
        }
    }
}

bool path_in(const std::vector<std::string>& patterns, const std::string& p) {
    for (const auto& q : patterns) if (q == p) return true;
    return false;
}

void normalize_floats_recursive(nlohmann::json& node,
                                const std::vector<std::string>& preserve,
                                std::string current_path) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            std::string child = current_path.empty()
                                ? it.key() : current_path + "." + it.key();
            normalize_floats_recursive(it.value(), preserve, child);
        }
    } else if (node.is_array()) {
        std::string array_path = current_path + "[]";
        for (auto& elem : node) {
            normalize_floats_recursive(elem, preserve, array_path);
        }
    } else if (node.is_number_float()) {
        // Skip preserved-number paths so ranked/timed tiers can apply
        // numeric tolerance later.
        if (path_in(preserve, current_path)) return;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", node.get<double>());
        node = std::string(buf);
    }
}

nlohmann::json sort_keys_recursive(const nlohmann::json& in) {
    if (in.is_object()) {
        // Walk keys in sorted order via nlohmann's ordered map view —
        // nlohmann::json already sorts keys when re-emitting via dump if we
        // construct a fresh object insertion-ordered with sorted keys.
        std::vector<std::string> keys;
        for (auto it = in.begin(); it != in.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        nlohmann::json out = nlohmann::json::object();
        for (const auto& k : keys) {
            out[k] = sort_keys_recursive(in.at(k));
        }
        return out;
    }
    if (in.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& elem : in) out.push_back(sort_keys_recursive(elem));
        return out;
    }
    return in;
}

} // namespace

nlohmann::json canonicalize_json(const nlohmann::json& in,
                                 const CanonicalizeOptions& opts) {
    nlohmann::json out = in;
    strip_paths_recursive(out, opts.ignore_paths, "");
    rewrite_paths_recursive(out, opts.corpus_prefix);
    normalize_floats_recursive(out, opts.preserve_number_paths, "");
    out = sort_keys_recursive(out);
    return out;
}

std::string canonicalize_text(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    size_t pos = 0;
    while (pos < in.size()) {
        size_t nl = in.find('\n', pos);
        std::string_view line =
            (nl == std::string_view::npos) ? in.substr(pos)
                                           : in.substr(pos, nl - pos);
        // trim trailing whitespace (space, tab, CR)
        size_t end = line.size();
        while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                           line[end - 1] == '\r')) {
            --end;
        }
        out.append(line.substr(0, end));
        if (nl == std::string_view::npos) break;
        out.push_back('\n');
        pos = nl + 1;
    }
    return out;
}

} // namespace lci::parity
```

- [ ] **Step 5: Run tests — should pass**

```bash
cmake --build build/debug --target parity_unit_tests -j$(nproc)
./build/debug/tests/parity/parity_unit_tests --gtest_filter='Canonicalize*'
```

Expected: 6 tests pass.

- [ ] **Step 6: Commit**

```bash
git add tests/parity/diff_engine/canonicalize.{h,cpp} \
        tests/parity/unit_tests/canonicalize_test.cpp
git commit -m "feat(parity): add JSON canonicalize (key-sort, num normalize, path rewrite)"
```

---

### Task 1.2: Field tier classifier

**Files:**
- Create: `tests/parity/diff_engine/field_tier.h`
- Create: `tests/parity/diff_engine/field_tier.cpp`
- Create: `tests/parity/unit_tests/field_tier_test.cpp`

- [ ] **Step 1: Write the failing tests**

`tests/parity/unit_tests/field_tier_test.cpp`:

```cpp
#include "diff_engine/field_tier.h"
#include <gtest/gtest.h>

using lci::parity::FieldTier;
using lci::parity::TierMap;
using lci::parity::classify_path;

TEST(FieldTier, ClassifiesStableByDefault) {
    TierMap m;
    EXPECT_EQ(classify_path(m, "anything"), FieldTier::Stable);
}

TEST(FieldTier, ExplicitMappingsTakePrecedence) {
    TierMap m;
    m.ranked = {"results[].score"};
    m.timed  = {"elapsed_ms"};
    m.ids    = {"request_id"};
    m.ignore = {"server_pid"};
    m.stable = {"results[].file"};
    EXPECT_EQ(classify_path(m, "results[].score"), FieldTier::Ranked);
    EXPECT_EQ(classify_path(m, "elapsed_ms"),      FieldTier::Timed);
    EXPECT_EQ(classify_path(m, "request_id"),      FieldTier::Id);
    EXPECT_EQ(classify_path(m, "server_pid"),      FieldTier::Ignore);
    EXPECT_EQ(classify_path(m, "results[].file"),  FieldTier::Stable);
}

TEST(FieldTier, ArrayWildcardMatchesAnyIndex) {
    // Internally we strip indexes to "[]" before lookup, so callers
    // should pass paths in array-wildcard form already. The classifier
    // just does literal lookup over the maps.
    TierMap m;
    m.ranked = {"results[].score"};
    EXPECT_EQ(classify_path(m, "results[].score"), FieldTier::Ranked);
    EXPECT_EQ(classify_path(m, "results[3].score"), FieldTier::Stable)
        << "Caller must normalize indexes before classifying";
}
```

- [ ] **Step 2: Run tests — fail to compile**

```bash
cmake --build build/debug --target parity_unit_tests 2>&1 | head -10
```

Expected: missing header.

- [ ] **Step 3: Write header + impl**

`tests/parity/diff_engine/field_tier.h`:

```cpp
#pragma once

#include <string>
#include <vector>

namespace lci::parity {

enum class FieldTier { Stable, Ranked, Timed, Id, Ignore };

struct TierMap {
    std::vector<std::string> stable;
    std::vector<std::string> ranked;
    std::vector<std::string> timed;
    std::vector<std::string> ids;
    std::vector<std::string> ignore;
};

// Returns the tier for a given JSONPath-lite path. Default = Stable
// (fail-closed). Caller is responsible for normalizing array indexes
// (e.g. "results[3].file" -> "results[].file") before calling.
FieldTier classify_path(const TierMap& m, const std::string& path);

// Helper: rewrite "[N]" to "[]" for any integer N in the path.
std::string normalize_indexes(const std::string& path);

} // namespace lci::parity
```

`tests/parity/diff_engine/field_tier.cpp`:

```cpp
#include "diff_engine/field_tier.h"

#include <algorithm>
#include <cctype>

namespace lci::parity {

namespace {
bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

FieldTier classify_path(const TierMap& m, const std::string& path) {
    // Order: explicit ignore → ids → timed → ranked → stable → default Stable
    if (contains(m.ignore, path)) return FieldTier::Ignore;
    if (contains(m.ids, path))    return FieldTier::Id;
    if (contains(m.timed, path))  return FieldTier::Timed;
    if (contains(m.ranked, path)) return FieldTier::Ranked;
    if (contains(m.stable, path)) return FieldTier::Stable;
    return FieldTier::Stable;
}

std::string normalize_indexes(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '[') {
            size_t j = i + 1;
            while (j < path.size() && std::isdigit(static_cast<unsigned char>(path[j]))) {
                ++j;
            }
            if (j < path.size() && path[j] == ']' && j > i + 1) {
                out += "[]";
                i = j + 1;
                continue;
            }
        }
        out.push_back(path[i]);
        ++i;
    }
    return out;
}

} // namespace lci::parity
```

- [ ] **Step 4: Run tests — pass**

```bash
cmake --build build/debug --target parity_unit_tests -j$(nproc)
./build/debug/tests/parity/parity_unit_tests --gtest_filter='FieldTier*'
```

Expected: 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tests/parity/diff_engine/field_tier.{h,cpp} \
        tests/parity/unit_tests/field_tier_test.cpp
git commit -m "feat(parity): add field tier classifier"
```

---

### Task 1.3: Tiered diff comparator

**Files:**
- Create: `tests/parity/diff_engine/diff.h`
- Create: `tests/parity/diff_engine/diff.cpp`
- Create: `tests/parity/unit_tests/diff_test.cpp`

- [ ] **Step 1: Write the failing tests**

`tests/parity/unit_tests/diff_test.cpp`:

```cpp
#include "diff_engine/diff.h"
#include <gtest/gtest.h>

using lci::parity::compare;
using lci::parity::DiffOptions;
using lci::parity::DiffResult;
using lci::parity::TierMap;
using nlohmann::json;

TEST(Diff, EqualStableJsonPasses) {
    auto a = json::parse(R"({"file":"x","line":1})");
    auto b = a;
    auto r = compare(a, b, {});
    EXPECT_TRUE(r.passed);
    EXPECT_TRUE(r.reasons.empty());
}

TEST(Diff, StableMismatchFails) {
    auto a = json::parse(R"({"file":"x","line":1})");
    auto b = json::parse(R"({"file":"x","line":2})");
    auto r = compare(a, b, {});
    EXPECT_FALSE(r.passed);
    ASSERT_FALSE(r.reasons.empty());
    EXPECT_NE(r.reasons[0].find("line"), std::string::npos);
}

TEST(Diff, TimedFieldWithinRangePasses) {
    auto a = json::parse(R"({"elapsed_ms":42})");
    auto b = json::parse(R"({"elapsed_ms":99})");
    DiffOptions opts;
    opts.tiers.timed = {"elapsed_ms"};
    opts.timed_max_ms = 60000;
    auto r = compare(a, b, opts);
    EXPECT_TRUE(r.passed) << (r.reasons.empty() ? "" : r.reasons[0]);
}

TEST(Diff, TimedFieldOutOfRangeFails) {
    auto a = json::parse(R"({"elapsed_ms":42})");
    auto b = json::parse(R"({"elapsed_ms":120000})");
    DiffOptions opts;
    opts.tiers.timed = {"elapsed_ms"};
    opts.timed_max_ms = 60000;
    auto r = compare(a, b, opts);
    EXPECT_FALSE(r.passed);
}

TEST(Diff, IgnoredFieldDoesNotAffectResult) {
    auto a = json::parse(R"({"server_pid":1,"x":42})");
    auto b = json::parse(R"({"server_pid":2,"x":42})");
    DiffOptions opts;
    opts.tiers.ignore = {"server_pid"};
    auto r = compare(a, b, opts);
    EXPECT_TRUE(r.passed);
}

TEST(Diff, RankedFieldScoreWithinTolerance) {
    auto a = json::parse(R"({"results":[{"file":"x","line":1,"score":0.91}]})");
    auto b = json::parse(R"({"results":[{"file":"x","line":1,"score":0.91005}]})");
    DiffOptions opts;
    opts.tiers.stable = {"results[].file","results[].line"};
    opts.tiers.ranked = {"results[].score"};
    opts.score_abs    = 0.01;
    auto r = compare(a, b, opts);
    EXPECT_TRUE(r.passed) << (r.reasons.empty() ? "" : r.reasons[0]);
}

TEST(Diff, RankedFieldScoreBeyondToleranceFails) {
    auto a = json::parse(R"({"results":[{"file":"x","line":1,"score":0.91}]})");
    auto b = json::parse(R"({"results":[{"file":"x","line":1,"score":0.5}]})");
    DiffOptions opts;
    opts.tiers.stable = {"results[].file","results[].line"};
    opts.tiers.ranked = {"results[].score"};
    opts.score_abs    = 0.01;
    auto r = compare(a, b, opts);
    EXPECT_FALSE(r.passed);
}

TEST(Diff, IdFieldFormatMatchPasses) {
    auto a = json::parse(R"({"request_id":"abc-123"})");
    auto b = json::parse(R"({"request_id":"xyz-987"})");
    DiffOptions opts;
    opts.tiers.ids = {"request_id"};
    opts.id_pattern = R"(^[a-z]+-[0-9]+$)";
    auto r = compare(a, b, opts);
    EXPECT_TRUE(r.passed);
}

TEST(Diff, IdFieldFormatMismatchFails) {
    auto a = json::parse(R"({"request_id":"abc-123"})");
    auto b = json::parse(R"({"request_id":"NOT_AN_ID"})");
    DiffOptions opts;
    opts.tiers.ids = {"request_id"};
    opts.id_pattern = R"(^[a-z]+-[0-9]+$)";
    auto r = compare(a, b, opts);
    EXPECT_FALSE(r.passed);
}
```

- [ ] **Step 2: Run tests — fail to compile**

```bash
cmake --build build/debug --target parity_unit_tests 2>&1 | head -10
```

- [ ] **Step 3: Write header + impl**

`tests/parity/diff_engine/diff.h`:

```cpp
#pragma once

#include "diff_engine/field_tier.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace lci::parity {

struct DiffOptions {
    TierMap     tiers;
    double      score_abs   = 0.01;     // ranked tolerance
    long long   timed_max_ms = 60000;   // timed range
    std::string id_pattern;             // regex; empty = type-only check
};

struct DiffResult {
    bool passed = true;
    std::vector<std::string> reasons;   // human-readable per-field reasons
    std::string unified_diff;           // unified diff of canonicalized JSON
};

// Compare two canonicalized JSON values per the tier map and tolerances.
// Inputs MUST already be canonicalized (sort_keys, num-normalize, etc.).
DiffResult compare(const nlohmann::json& go,
                   const nlohmann::json& cpp,
                   const DiffOptions& opts);

} // namespace lci::parity
```

`tests/parity/diff_engine/diff.cpp`:

```cpp
#include "diff_engine/diff.h"

#include <regex>
#include <sstream>

namespace lci::parity {

namespace {

void walk(const nlohmann::json& a, const nlohmann::json& b,
          const DiffOptions& opts, std::string path,
          DiffResult& r) {
    auto tier = classify_path(opts.tiers, normalize_indexes(path));

    // Ignore tier: skip outright.
    if (tier == FieldTier::Ignore) return;

    // Both null: equal.
    if (a.is_null() && b.is_null()) return;

    // Type mismatch always fails (unless ignored).
    if (a.type() != b.type()) {
        r.passed = false;
        r.reasons.push_back(path + ": type mismatch (" +
                            std::string(a.type_name()) + " vs " +
                            std::string(b.type_name()) + ")");
        return;
    }

    if (a.is_object()) {
        // Union of keys
        std::vector<std::string> keys;
        for (auto it = a.begin(); it != a.end(); ++it) keys.push_back(it.key());
        for (auto it = b.begin(); it != b.end(); ++it) {
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
                keys.push_back(it.key());
            }
        }
        for (const auto& k : keys) {
            std::string child = path.empty() ? k : path + "." + k;
            const auto& av = a.contains(k) ? a.at(k) : nlohmann::json();
            const auto& bv = b.contains(k) ? b.at(k) : nlohmann::json();
            walk(av, bv, opts, child, r);
        }
        return;
    }

    if (a.is_array()) {
        std::string child_path = path + "[]";
        auto child_tier = classify_path(opts.tiers, normalize_indexes(child_path));

        // Ranked array: descend per-element. Element child paths inherit
        // ranked tier via per-field classification (stable for keys, ranked
        // for score). Treat as ordered for now; future work: multiset match.
        if (a.size() != b.size()) {
            r.passed = false;
            r.reasons.push_back(path + ": array length mismatch (" +
                                std::to_string(a.size()) + " vs " +
                                std::to_string(b.size()) + ")");
            return;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            walk(a[i], b[i], opts, child_path, r);
        }
        return;
    }

    // Leaf comparison per tier.
    switch (tier) {
        case FieldTier::Stable: {
            if (a != b) {
                r.passed = false;
                r.reasons.push_back(path + ": stable mismatch (" +
                                    a.dump() + " vs " + b.dump() + ")");
            }
            return;
        }
        case FieldTier::Ranked: {
            // Score must be number; absolute diff within tolerance.
            if (!a.is_number() || !b.is_number()) {
                r.passed = false;
                r.reasons.push_back(path + ": ranked tier expects number");
                return;
            }
            double da = a.get<double>(), db = b.get<double>();
            if (std::abs(da - db) > opts.score_abs) {
                r.passed = false;
                std::ostringstream os;
                os << path << ": ranked drift |" << da << " - " << db
                   << "| > " << opts.score_abs;
                r.reasons.push_back(os.str());
            }
            return;
        }
        case FieldTier::Timed: {
            for (const auto& v : {a, b}) {
                if (!v.is_number()) {
                    r.passed = false;
                    r.reasons.push_back(path + ": timed tier expects number");
                    return;
                }
                double dv = v.get<double>();
                if (dv < 0 || dv > static_cast<double>(opts.timed_max_ms)) {
                    r.passed = false;
                    r.reasons.push_back(path + ": timed out of range");
                    return;
                }
            }
            return;
        }
        case FieldTier::Id: {
            if (!a.is_string() || !b.is_string()) {
                r.passed = false;
                r.reasons.push_back(path + ": id tier expects string");
                return;
            }
            if (!opts.id_pattern.empty()) {
                std::regex re(opts.id_pattern);
                if (!std::regex_match(a.get<std::string>(), re) ||
                    !std::regex_match(b.get<std::string>(), re)) {
                    r.passed = false;
                    r.reasons.push_back(path + ": id format mismatch");
                }
            }
            return;
        }
        case FieldTier::Ignore:
            return;
    }
}

std::string make_unified_diff(const std::string& a, const std::string& b) {
    // Minimal unified diff: split lines, find first divergence, emit
    // a small window. Real implementation could use libdiff; this keeps
    // dependencies minimal.
    if (a == b) return "";
    std::ostringstream os;
    os << "--- go\n+++ cpp\n";
    std::vector<std::string> al, bl;
    std::istringstream as(a), bs(b);
    std::string line;
    while (std::getline(as, line)) al.push_back(line);
    while (std::getline(bs, line)) bl.push_back(line);
    size_t n = std::max(al.size(), bl.size());
    for (size_t i = 0; i < n; ++i) {
        const std::string& la = i < al.size() ? al[i] : "";
        const std::string& lb = i < bl.size() ? bl[i] : "";
        if (la == lb) {
            os << " " << la << "\n";
        } else {
            os << "-" << la << "\n+" << lb << "\n";
        }
    }
    return os.str();
}

} // namespace

DiffResult compare(const nlohmann::json& go,
                   const nlohmann::json& cpp,
                   const DiffOptions& opts) {
    DiffResult r;
    walk(go, cpp, opts, "", r);
    if (!r.passed) {
        r.unified_diff = make_unified_diff(go.dump(2), cpp.dump(2));
    }
    return r;
}

} // namespace lci::parity
```

- [ ] **Step 4: Run tests — pass**

```bash
cmake --build build/debug --target parity_unit_tests -j$(nproc)
./build/debug/tests/parity/parity_unit_tests --gtest_filter='Diff*'
```

Expected: 9 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tests/parity/diff_engine/diff.{h,cpp} \
        tests/parity/unit_tests/diff_test.cpp
git commit -m "feat(parity): add tiered diff comparator (stable/ranked/timed/id/ignore)"
```

---

### Task 1.4: Descriptor parser

**Files:**
- Create: `tests/parity/runner/descriptor.h`
- Create: `tests/parity/runner/descriptor.cpp`
- Create: `tests/parity/unit_tests/descriptor_test.cpp`

- [ ] **Step 1: Write the failing tests**

`tests/parity/unit_tests/descriptor_test.cpp`:

```cpp
#include "runner/descriptor.h"
#include <gtest/gtest.h>

using lci::parity::Descriptor;
using lci::parity::Mode;
using lci::parity::ParseStyle;
using lci::parity::parse_descriptor;

static const char* kSampleCli = R"({
  "id": "cli/search/json-basic",
  "mode": "cli",
  "corpus": "lci-go-repo",
  "go_binary": "${LCI_GO}",
  "cpp_binary": "${LCI_CPP}",
  "invocation": {
    "args": ["search", "--json", "MasterIndex"],
    "env": {"LCI_NO_DAEMON": "1"},
    "cwd": "${CORPUS}"
  },
  "capture": ["stdout","exit"],
  "parse": "json",
  "tiers": {
    "stable": ["results[].file","results[].line"],
    "ranked": ["results[].score"],
    "timed":  ["elapsed_ms"],
    "ids":    ["request_id"],
    "ignore": ["server_pid","version"]
  },
  "tolerances": {"score_abs": 0.01, "timed_max_ms": 60000},
  "expect_exit": 0
})";

TEST(Descriptor, ParsesAllFields) {
    auto d = parse_descriptor(kSampleCli);
    EXPECT_EQ(d.id, "cli/search/json-basic");
    EXPECT_EQ(d.mode, Mode::Cli);
    EXPECT_EQ(d.corpus, "lci-go-repo");
    EXPECT_EQ(d.invocation.args.size(), 3u);
    EXPECT_EQ(d.invocation.args[1], "--json");
    EXPECT_EQ(d.invocation.env.at("LCI_NO_DAEMON"), "1");
    EXPECT_EQ(d.parse, ParseStyle::Json);
    EXPECT_DOUBLE_EQ(d.tolerances.score_abs, 0.01);
    EXPECT_EQ(d.tolerances.timed_max_ms, 60000);
    EXPECT_EQ(d.expect_exit, 0);
}

TEST(Descriptor, RejectsUnknownMode) {
    std::string bad = R"({"id":"x","mode":"wat","corpus":"c","invocation":{"args":[]}})";
    EXPECT_THROW(parse_descriptor(bad), std::runtime_error);
}

TEST(Descriptor, MissingIdFieldThrows) {
    std::string bad = R"({"mode":"cli","corpus":"c","invocation":{"args":[]}})";
    EXPECT_THROW(parse_descriptor(bad), std::runtime_error);
}
```

- [ ] **Step 2: Run tests — fail**

```bash
cmake --build build/debug --target parity_unit_tests 2>&1 | head -10
```

- [ ] **Step 3: Write header + impl**

`tests/parity/runner/descriptor.h`:

```cpp
#pragma once

#include "diff_engine/diff.h"
#include <map>
#include <string>
#include <vector>

namespace lci::parity {

enum class Mode { Cli, Mcp, Http, Index };
enum class ParseStyle { Json, Text, ExitOnly };

struct Invocation {
    std::vector<std::string> args;
    std::string stdin_data;
    std::map<std::string, std::string> env;
    std::string cwd;        // may contain "${CORPUS}"
};

struct Descriptor {
    std::string id;
    Mode        mode;
    std::string corpus;     // key into corpora/
    std::string go_binary;  // "${LCI_GO}" placeholder allowed
    std::string cpp_binary;
    Invocation  invocation;
    std::vector<std::string> capture;  // "stdout", "stderr", "exit"
    ParseStyle  parse;
    TierMap     tiers;
    struct Tolerances {
        double    score_abs    = 0.01;
        long long timed_max_ms = 60000;
    } tolerances;
    int         expect_exit = 0;
    std::string id_pattern;  // for ids tier (optional)
};

// Throws std::runtime_error on schema or type errors. The input is the raw
// JSON text of one .parity.json file.
Descriptor parse_descriptor(const std::string& json_text);

} // namespace lci::parity
```

`tests/parity/runner/descriptor.cpp`:

```cpp
#include "runner/descriptor.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace lci::parity {

namespace {

Mode parse_mode(const std::string& s) {
    if (s == "cli")   return Mode::Cli;
    if (s == "mcp")   return Mode::Mcp;
    if (s == "http")  return Mode::Http;
    if (s == "index") return Mode::Index;
    throw std::runtime_error("invalid mode: " + s);
}

ParseStyle parse_style(const std::string& s) {
    if (s == "json")        return ParseStyle::Json;
    if (s == "text")        return ParseStyle::Text;
    if (s == "exit-only")   return ParseStyle::ExitOnly;
    throw std::runtime_error("invalid parse style: " + s);
}

std::vector<std::string> str_array(const nlohmann::json& j) {
    std::vector<std::string> out;
    for (const auto& e : j) out.push_back(e.get<std::string>());
    return out;
}

void load_tiers(const nlohmann::json& j, TierMap& m) {
    if (!j.is_object()) return;
    if (j.contains("stable")) m.stable = str_array(j.at("stable"));
    if (j.contains("ranked")) m.ranked = str_array(j.at("ranked"));
    if (j.contains("timed"))  m.timed  = str_array(j.at("timed"));
    if (j.contains("ids"))    m.ids    = str_array(j.at("ids"));
    if (j.contains("ignore")) m.ignore = str_array(j.at("ignore"));
}

void require(const nlohmann::json& j, const std::string& key) {
    if (!j.contains(key)) {
        throw std::runtime_error("descriptor missing required field: " + key);
    }
}

} // namespace

Descriptor parse_descriptor(const std::string& json_text) {
    auto j = nlohmann::json::parse(json_text);
    require(j, "id");
    require(j, "mode");
    require(j, "corpus");
    require(j, "invocation");

    Descriptor d;
    d.id     = j.at("id").get<std::string>();
    d.mode   = parse_mode(j.at("mode").get<std::string>());
    d.corpus = j.at("corpus").get<std::string>();
    d.go_binary  = j.value("go_binary",  std::string("${LCI_GO}"));
    d.cpp_binary = j.value("cpp_binary", std::string("${LCI_CPP}"));

    const auto& inv = j.at("invocation");
    if (inv.contains("args"))  d.invocation.args = str_array(inv.at("args"));
    if (inv.contains("stdin") && inv.at("stdin").is_string()) {
        d.invocation.stdin_data = inv.at("stdin").get<std::string>();
    }
    if (inv.contains("env")) {
        for (auto it = inv.at("env").begin(); it != inv.at("env").end(); ++it) {
            d.invocation.env[it.key()] = it.value().get<std::string>();
        }
    }
    d.invocation.cwd = inv.value("cwd", std::string("${CORPUS}"));

    if (j.contains("capture")) d.capture = str_array(j.at("capture"));
    else d.capture = {"stdout", "exit"};

    d.parse = parse_style(j.value("parse", std::string("json")));

    if (j.contains("tiers")) load_tiers(j.at("tiers"), d.tiers);

    if (j.contains("tolerances")) {
        const auto& t = j.at("tolerances");
        d.tolerances.score_abs    = t.value("score_abs",    0.01);
        d.tolerances.timed_max_ms = t.value("timed_max_ms", 60000LL);
    }
    d.expect_exit = j.value("expect_exit", 0);
    d.id_pattern  = j.value("id_pattern",  std::string());
    return d;
}

} // namespace lci::parity
```

- [ ] **Step 4: Run tests — pass**

```bash
cmake --build build/debug --target parity_unit_tests -j$(nproc)
./build/debug/tests/parity/parity_unit_tests --gtest_filter='Descriptor*'
```

Expected: 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tests/parity/runner/descriptor.{h,cpp} \
        tests/parity/unit_tests/descriptor_test.cpp
git commit -m "feat(parity): add descriptor parser"
```

---

### Task 1.5: CLI mode — fork+exec subprocess capture

**Files:**
- Create: `tests/parity/runner/modes/cli.h`
- Create: `tests/parity/runner/modes/cli.cpp`

(No unit test here — will be exercised by Task 1.7 smoke descriptor end-to-end.)

- [ ] **Step 1: Write header**

`tests/parity/runner/modes/cli.h`:

```cpp
#pragma once

#include "runner/descriptor.h"
#include <string>
#include <map>

namespace lci::parity {

struct CapturedOutput {
    std::string stdout_data;
    std::string stderr_data;
    int         exit_code = -1;
    bool        timed_out = false;
};

// Runs the binary at `binary_path` with the descriptor's invocation,
// substituting placeholder env vars. corpus_path is the resolved
// absolute path to the corpus directory (replaces ${CORPUS} in cwd/args).
// timeout_seconds = SIGKILL after this many seconds.
CapturedOutput run_cli(const std::string& binary_path,
                      const Invocation&  inv,
                      const std::string& corpus_path,
                      int                timeout_seconds = 60);

} // namespace lci::parity
```

- [ ] **Step 2: Write impl**

`tests/parity/runner/modes/cli.cpp`:

```cpp
#include "runner/modes/cli.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace lci::parity {

namespace {

std::string substitute(const std::string& s, const std::string& corpus_path) {
    std::string out = s;
    auto replace = [&](const std::string& token, const std::string& with) {
        size_t pos = 0;
        while ((pos = out.find(token, pos)) != std::string::npos) {
            out.replace(pos, token.size(), with);
            pos += with.size();
        }
    };
    replace("${CORPUS}", corpus_path);
    return out;
}

std::string read_all(int fd) {
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

} // namespace

CapturedOutput run_cli(const std::string& binary_path,
                       const Invocation&  inv,
                       const std::string& corpus_path,
                       int                timeout_seconds) {
    int out_pipe[2], err_pipe[2], in_pipe[2];
    if (pipe(out_pipe) || pipe(err_pipe) || pipe(in_pipe)) {
        throw std::runtime_error(std::string("pipe failed: ") + strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error(std::string("fork failed: ") + strerror(errno));
    }

    if (pid == 0) {
        // child
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        for (const auto& [k, v] : inv.env) {
            setenv(k.c_str(), v.c_str(), 1);
        }
        if (!inv.cwd.empty()) {
            std::string cwd = substitute(inv.cwd, corpus_path);
            if (chdir(cwd.c_str()) != 0) {
                _exit(127);
            }
        }
        std::vector<std::string> sub_args;
        sub_args.reserve(inv.args.size());
        for (const auto& a : inv.args) sub_args.push_back(substitute(a, corpus_path));

        std::vector<char*> argv;
        argv.reserve(sub_args.size() + 2);
        std::string prog = binary_path;
        argv.push_back(prog.data());
        for (auto& a : sub_args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(prog.c_str(), argv.data());
        _exit(127);
    }

    // parent
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (!inv.stdin_data.empty()) {
        ssize_t total = 0;
        while (total < (ssize_t)inv.stdin_data.size()) {
            ssize_t n = ::write(in_pipe[1],
                                inv.stdin_data.data() + total,
                                inv.stdin_data.size() - total);
            if (n < 0) { if (errno == EINTR) continue; break; }
            total += n;
        }
    }
    close(in_pipe[1]);

    CapturedOutput cap;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_seconds);
    int status = 0;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            cap.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    cap.stdout_data = read_all(out_pipe[0]);
    cap.stderr_data = read_all(err_pipe[0]);
    close(out_pipe[0]);
    close(err_pipe[0]);

    if (WIFEXITED(status))   cap.exit_code = WEXITSTATUS(status);
    else                     cap.exit_code = -1;
    return cap;
}

} // namespace lci::parity
```

- [ ] **Step 3: Build to confirm it compiles**

```bash
cmake --build build/debug --target lci_parity_runner_lib -j$(nproc)
```

Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add tests/parity/runner/modes/cli.{h,cpp}
git commit -m "feat(parity): add CLI subprocess mode (fork+exec, capture, timeout)"
```

---

### Task 1.6: parity_runner main + CLI mode glue

**Files:**
- Modify: `tests/parity/runner/parity_runner.cpp`
- Modify: `tests/parity/runner/modes/{mcp,http,index}.cpp` (add stubs)

- [ ] **Step 1: Write the main runner**

`tests/parity/runner/parity_runner.cpp`:

```cpp
#include "diff_engine/canonicalize.h"
#include "diff_engine/diff.h"
#include "runner/descriptor.h"
#include "runner/modes/cli.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using namespace lci::parity;

namespace {

std::string env_or(const char* name, const std::string& dflt = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : dflt;
}

std::string substitute_binary(const std::string& spec) {
    if (spec == "${LCI_GO}")  return env_or("LCI_GO");
    if (spec == "${LCI_CPP}") return env_or("LCI_CPP");
    return spec;
}

std::string resolve_corpus(const std::string& key) {
    std::string base = env_or("PARITY_CORPORA");
    if (base.empty()) {
        throw std::runtime_error("PARITY_CORPORA env not set");
    }
    return (fs::path(base) / "synthetic" / key).string().c_str() == nullptr
            ? std::string()
            : (fs::is_directory(fs::path(base) / "synthetic" / key)
                ? (fs::path(base) / "synthetic" / key).string()
                : (fs::path(base) / key).string());
}

void write_dump(const fs::path& dump_dir,
                const Descriptor& d,
                const std::string& go_raw,
                const std::string& cpp_raw,
                const nlohmann::json& go_canon,
                const nlohmann::json& cpp_canon,
                const DiffResult& dr) {
    fs::create_directories(dump_dir);
    {
        std::ofstream f(dump_dir / "desc.json");
        f << nlohmann::json{{"id", d.id}, {"corpus", d.corpus},
                            {"mode", "cli"}}.dump(2);
    }
    std::ofstream(dump_dir / "go.raw")  << go_raw;
    std::ofstream(dump_dir / "cpp.raw") << cpp_raw;
    std::ofstream(dump_dir / "go.canon.json")  << go_canon.dump(2);
    std::ofstream(dump_dir / "cpp.canon.json") << cpp_canon.dump(2);
    std::ofstream(dump_dir / "diff.txt") << dr.unified_diff;
    std::ofstream rep(dump_dir / "report.txt");
    rep << "test_id: " << d.id << "\n";
    rep << "passed: " << (dr.passed ? "true" : "false") << "\n\n";
    for (const auto& reason : dr.reasons) rep << "- " << reason << "\n";
}

int run_cli_descriptor(const Descriptor& d) {
    std::string corpus_path = resolve_corpus(d.corpus);
    std::string go_bin  = substitute_binary(d.go_binary);
    std::string cpp_bin = substitute_binary(d.cpp_binary);

    if (go_bin.empty() || cpp_bin.empty()) {
        std::cerr << "infra: LCI_GO or LCI_CPP not set\n";
        return 2;
    }

    auto go  = run_cli(go_bin,  d.invocation, corpus_path);
    auto cpp = run_cli(cpp_bin, d.invocation, corpus_path);

    if (go.timed_out || cpp.timed_out) {
        std::cerr << "infra: timeout\n";
        return 2;
    }
    if (d.expect_exit != go.exit_code || d.expect_exit != cpp.exit_code) {
        std::cerr << "exit-code mismatch: expected " << d.expect_exit
                  << " got go=" << go.exit_code
                  << " cpp=" << cpp.exit_code << "\n";
        return 1;
    }

    nlohmann::json go_canon, cpp_canon;
    DiffOptions opts;
    opts.tiers        = d.tiers;
    opts.score_abs    = d.tolerances.score_abs;
    opts.timed_max_ms = d.tolerances.timed_max_ms;
    opts.id_pattern   = d.id_pattern;

    DiffResult dr;
    if (d.parse == ParseStyle::Json) {
        try {
            auto go_j  = nlohmann::json::parse(go.stdout_data);
            auto cpp_j = nlohmann::json::parse(cpp.stdout_data);
            CanonicalizeOptions co;
            co.ignore_paths  = d.tiers.ignore;
            co.corpus_prefix = corpus_path;
            co.preserve_number_paths = d.tiers.ranked;
            co.preserve_number_paths.insert(co.preserve_number_paths.end(),
                                            d.tiers.timed.begin(),
                                            d.tiers.timed.end());
            go_canon  = canonicalize_json(go_j,  co);
            cpp_canon = canonicalize_json(cpp_j, co);
            dr = compare(go_canon, cpp_canon, opts);
        } catch (const std::exception& e) {
            std::cerr << "infra: json parse failed: " << e.what() << "\n";
            return 2;
        }
    } else if (d.parse == ParseStyle::Text) {
        std::string a = canonicalize_text(go.stdout_data);
        std::string b = canonicalize_text(cpp.stdout_data);
        dr.passed = (a == b);
        if (!dr.passed) {
            dr.reasons.push_back("text mismatch");
            dr.unified_diff = a + "\n--- vs ---\n" + b;
        }
        go_canon  = a;
        cpp_canon = b;
    } else {
        // ExitOnly — already checked
        dr.passed = true;
    }

    if (!dr.passed) {
        fs::path dump_dir =
            fs::path(env_or("PARITY_FAILURES", "build/parity-failures")) / d.id;
        write_dump(dump_dir, d, go.stdout_data, cpp.stdout_data,
                   go_canon, cpp_canon, dr);
        std::cerr << "FAIL " << d.id << " (" << dr.reasons.size() << " reasons)\n";
        for (const auto& r : dr.reasons) std::cerr << "  - " << r << "\n";
        std::cerr << "dump: " << dump_dir << "\n";
        return 1;
    }
    std::cout << "PASS " << d.id << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"lci parity runner"};
    std::string desc_path;
    app.add_option("descriptor", desc_path, "Path to .parity.json")->required();
    CLI11_PARSE(app, argc, argv);

    std::ifstream f(desc_path);
    if (!f) { std::cerr << "cannot open " << desc_path << "\n"; return 2; }
    std::stringstream ss; ss << f.rdbuf();
    Descriptor d;
    try {
        d = parse_descriptor(ss.str());
    } catch (const std::exception& e) {
        std::cerr << "descriptor parse error: " << e.what() << "\n";
        return 2;
    }

    switch (d.mode) {
        case Mode::Cli:   return run_cli_descriptor(d);
        case Mode::Mcp:
        case Mode::Http:
        case Mode::Index:
            std::cerr << "mode not yet implemented in this phase\n";
            return 2;
    }
    return 2;
}
```

- [ ] **Step 2: Add empty stubs for unimplemented modes**

`tests/parity/runner/modes/mcp.h`, `mcp.cpp`, `http.h`, `http.cpp`, `index.h`, `index.cpp`:

```cpp
// tests/parity/runner/modes/mcp.h (and identically named for http, index)
#pragma once
namespace lci::parity { /* phase 3+ */ }
```

```cpp
// tests/parity/runner/modes/mcp.cpp (and identically named for http, index)
// Stub for later phases.
```

- [ ] **Step 3: Build the runner**

```bash
cmake --build build/debug --target parity_runner -j$(nproc)
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add tests/parity/runner
git commit -m "feat(parity): wire parity_runner main + CLI dispatch"
```

---

### Task 1.7: First end-to-end smoke descriptor — `lci --version`

**Files:**
- Create: `tests/parity/descriptors/cli/version.parity.json`

- [ ] **Step 1: Write the smoke descriptor**

`tests/parity/descriptors/cli/version.parity.json`:

```json
{
  "id": "cli/version",
  "mode": "cli",
  "corpus": "synthetic/empty",
  "go_binary": "${LCI_GO}",
  "cpp_binary": "${LCI_CPP}",
  "invocation": {
    "args": ["--version"],
    "cwd": "${CORPUS}"
  },
  "capture": ["stdout", "exit"],
  "parse": "exit-only",
  "tiers": {},
  "tolerances": {"score_abs": 0.0, "timed_max_ms": 60000},
  "expect_exit": 0
}
```

(Note: `parse: exit-only` skips body diffing because Go/C++ will report different version strings — we just want to confirm both run. The full text-mode version compare comes once we pin both versions.)

- [ ] **Step 2: Build, prep corpora, run**

```bash
cmake --build build/debug -j$(nproc)
./tests/parity/corpora/prep_real.sh
ctest --test-dir build/debug -L parity -R 'parity\.cli\.version' --output-on-failure
```

Expected: `parity.cli.version` PASSES (both binaries exit 0).

- [ ] **Step 3: Inject a deliberate failure to verify dump path**

Temporarily change `expect_exit` to `99` in the descriptor and rerun:

```bash
sed -i 's/"expect_exit": 0/"expect_exit": 99/' \
    tests/parity/descriptors/cli/version.parity.json
ctest --test-dir build/debug -L parity -R 'parity\.cli\.version' --output-on-failure
ls -la build/debug/parity-failures/cli/version/
```

Expected: test FAILS; dump dir exists with `desc.json`, `report.txt`. Revert:

```bash
sed -i 's/"expect_exit": 99/"expect_exit": 0/' \
    tests/parity/descriptors/cli/version.parity.json
```

- [ ] **Step 4: Commit smoke descriptor**

```bash
git add tests/parity/descriptors/cli/version.parity.json
git commit -m "test(parity): add cli --version smoke descriptor"
```

---

## Phase 2 — CLI parity descriptors (broad shallow)

Goal: cover every CLI command from MIGRATION.md across the four corpora. Each descriptor is small and self-contained. We add them in batches and run the full suite after each batch.

**Strategy:** Write descriptors against `synthetic/multi-lang` first (deterministic, fast). Then add `lci-go-repo` and `lci-cpp-repo` variants for commands that don't depend on absolute symbol counts (use predicate-style queries: `--regex`, `--kind`, etc.).

### Task 2.1: Search command variants

**Files:**
- Create: `tests/parity/descriptors/cli/search/{basic,json,compact,case-insensitive,regex,grep}.parity.json` (6 files)

- [ ] **Step 1: Write the 6 descriptors**

For each, follow this template (adjusted args). Example `tests/parity/descriptors/cli/search/json.parity.json`:

```json
{
  "id": "cli/search/json",
  "mode": "cli",
  "corpus": "multi-lang",
  "invocation": {
    "args": ["search", "--json", "add"],
    "cwd": "${CORPUS}"
  },
  "parse": "json",
  "tiers": {
    "stable": ["results[].file","results[].line","results[].symbol","total"],
    "ranked": ["results[].score"],
    "timed":  ["elapsed_ms"],
    "ignore": ["server_pid","version","request_id"]
  },
  "tolerances": {"score_abs": 0.05, "timed_max_ms": 60000},
  "expect_exit": 0
}
```

Variants:

| File | Args | Notes |
|------|------|-------|
| `basic.parity.json` | `["search", "add"]` | text output; parse: text |
| `json.parity.json` | `["search", "--json", "add"]` | JSON output |
| `compact.parity.json` | `["search", "--compact-search", "add"]` | compact text |
| `case-insensitive.parity.json` | `["search", "-i", "ADD"]` | text |
| `regex.parity.json` | `["search", "--regex", "func.*Add"]` | text |
| `grep.parity.json` | `["grep", "add"]` | grep mode |

For text-output variants set `"parse": "text"` and `"tiers": {}`.

- [ ] **Step 2: Run the search batch**

```bash
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug -L parity -R 'parity\.cli\.search' --output-on-failure
```

Expected: all green. If any fail, inspect dump in `build/debug/parity-failures/cli/search/<name>/`. Document each genuine divergence as a tracked bug; do not silence by widening tolerances.

- [ ] **Step 3: Commit search batch**

```bash
git add tests/parity/descriptors/cli/search
git commit -m "test(parity): add CLI search-variant descriptors"
```

---

### Task 2.2: Symbol commands (def, refs, tree, list, symbols, inspect, browse)

**Files:** seven descriptors under `tests/parity/descriptors/cli/symbols/`.

- [ ] **Step 1: Write the seven descriptors**

`tests/parity/descriptors/cli/symbols/def.parity.json`:

```json
{
  "id": "cli/symbols/def",
  "mode": "cli",
  "corpus": "multi-lang",
  "invocation": {"args": ["def", "Add"], "cwd": "${CORPUS}"},
  "parse": "text",
  "tiers": {},
  "tolerances": {"score_abs": 0.0, "timed_max_ms": 60000},
  "expect_exit": 0
}
```

Use the same template, varying `id`/`args`:

| File | Args |
|------|------|
| `def.parity.json` | `["def","Add"]` |
| `refs.parity.json` | `["refs","add"]` |
| `tree.parity.json` | `["tree","Add"]` |
| `list.parity.json` | `["list"]` |
| `symbols.parity.json` | `["symbols","--kind","function"]` |
| `inspect.parity.json` | `["inspect","Add"]` |
| `browse.parity.json` | `["browse","a.go"]` |

- [ ] **Step 2: Run the symbols batch**

```bash
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug -L parity -R 'parity\.cli\.symbols' --output-on-failure
```

Expected: green or actionable diff dumps.

- [ ] **Step 3: Commit**

```bash
git add tests/parity/descriptors/cli/symbols
git commit -m "test(parity): add CLI symbol-command descriptors"
```

---

### Task 2.3: Config, debug, git-analyze descriptors

**Files:** `tests/parity/descriptors/cli/config/{show,validate}.parity.json`, `tests/parity/descriptors/cli/debug/{info,validate}.parity.json`, `tests/parity/descriptors/cli/git/git-analyze.parity.json`.

- [ ] **Step 1: Write descriptors**

`tests/parity/descriptors/cli/config/show.parity.json`:

```json
{
  "id": "cli/config/show",
  "mode": "cli",
  "corpus": "multi-lang",
  "invocation": {"args": ["config","show"], "cwd": "${CORPUS}"},
  "parse": "text",
  "tiers": {},
  "tolerances": {"score_abs": 0.0, "timed_max_ms": 60000},
  "expect_exit": 0
}
```

Repeat the same pattern for the other four (vary `id` and `args`):

| Path | id | args |
|------|----|----|
| `config/validate.parity.json` | `cli/config/validate` | `["config","validate"]` |
| `debug/info.parity.json` | `cli/debug/info` | `["debug","info"]` |
| `debug/validate.parity.json` | `cli/debug/validate` | `["debug","validate"]` |
| `git/git-analyze.parity.json` | `cli/git/git-analyze` | `["git-analyze","--json","--scope","wip"]` (use `parse: json`, `corpus: lci-go-repo`) |

- [ ] **Step 2: Run and commit**

```bash
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug -L parity --output-on-failure
git add tests/parity/descriptors/cli/{config,debug,git}
git commit -m "test(parity): add CLI config/debug/git-analyze descriptors"
```

Expected: all `parity.cli.*` green or recognized divergences logged.

---

## Phase 3 — MCP parity

### Task 3.1: MCP mode (long-lived JSON-RPC stdio session)

**Files:**
- Modify: `tests/parity/runner/modes/mcp.h`
- Modify: `tests/parity/runner/modes/mcp.cpp`
- Modify: `tests/parity/runner/parity_runner.cpp` (add `Mode::Mcp` dispatch)

- [ ] **Step 1: Write the MCP session helper**

`tests/parity/runner/modes/mcp.h`:

```cpp
#pragma once

#include "runner/descriptor.h"
#include "runner/modes/cli.h"
#include <string>

namespace lci::parity {

// Drives one MCP descriptor: spawns the binary with `mcp` subcommand,
// performs JSON-RPC initialize, sends the descriptor's tool call,
// captures the response, terminates cleanly. The descriptor's
// invocation.stdin_data is the JSON-RPC request body for the call;
// invocation.args[0] is expected to be "mcp".
CapturedOutput run_mcp(const std::string& binary_path,
                       const Descriptor&  d,
                       const std::string& corpus_path,
                       int timeout_seconds = 60);

} // namespace lci::parity
```

`tests/parity/runner/modes/mcp.cpp`:

```cpp
#include "runner/modes/mcp.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace lci::parity {

namespace {

const char* kInitializeFrame =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
    "\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"parity_runner\",\"version\":\"1.0\"}}}\n";

ssize_t write_all(int fd, const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = ::write(fd, data.data() + total, data.size() - total);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

std::string read_one_line(int fd, std::chrono::steady_clock::time_point deadline) {
    std::string out;
    char c;
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::read(fd, &c, 1);
        if (n == 1) {
            if (c == '\n') return out;
            out.push_back(c);
        } else if (n == 0) {
            return out;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return out;
        }
    }
    return out;
}

} // namespace

CapturedOutput run_mcp(const std::string& binary_path,
                       const Descriptor&  d,
                       const std::string& corpus_path,
                       int timeout_seconds) {
    int out_pipe[2], in_pipe[2], err_pipe[2];
    if (pipe(out_pipe) || pipe(in_pipe) || pipe(err_pipe)) {
        throw std::runtime_error(std::string("pipe failed: ") + strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        for (const auto& [k, v] : d.invocation.env) setenv(k.c_str(), v.c_str(), 1);
        if (!corpus_path.empty()) chdir(corpus_path.c_str());

        std::vector<std::string> args = d.invocation.args;
        std::vector<char*> argv;
        std::string prog = binary_path;
        argv.push_back(prog.data());
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(prog.c_str(), argv.data());
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_seconds);

    write_all(in_pipe[1], kInitializeFrame);
    std::string init_resp = read_one_line(out_pipe[0], deadline);
    (void)init_resp; // we don't validate beyond non-empty

    // Send the descriptor's stdin payload (the tool-call JSON-RPC).
    if (!d.invocation.stdin_data.empty()) {
        std::string body = d.invocation.stdin_data;
        if (body.back() != '\n') body.push_back('\n');
        write_all(in_pipe[1], body);
    }
    std::string call_resp = read_one_line(out_pipe[0], deadline);

    close(in_pipe[1]);
    ::kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);

    CapturedOutput cap;
    cap.stdout_data = call_resp;
    cap.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
    cap.timed_out = std::chrono::steady_clock::now() > deadline;

    char buf[4096];
    while (true) {
        ssize_t n = ::read(err_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        cap.stderr_data.append(buf, static_cast<size_t>(n));
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    return cap;
}

} // namespace lci::parity
```

- [ ] **Step 2: Wire MCP dispatch in main**

In `tests/parity/runner/parity_runner.cpp`, replace the `Mode::Mcp` arm in `main()` with a call to a new helper:

```cpp
// Add near run_cli_descriptor:
int run_mcp_descriptor(const Descriptor& d) {
    std::string corpus_path = resolve_corpus(d.corpus);
    std::string go_bin  = substitute_binary(d.go_binary);
    std::string cpp_bin = substitute_binary(d.cpp_binary);
    auto go  = run_mcp(go_bin,  d, corpus_path);
    auto cpp = run_mcp(cpp_bin, d, corpus_path);
    if (go.timed_out || cpp.timed_out) { std::cerr << "infra: mcp timeout\n"; return 2; }

    DiffOptions opts;
    opts.tiers        = d.tiers;
    opts.score_abs    = d.tolerances.score_abs;
    opts.timed_max_ms = d.tolerances.timed_max_ms;
    opts.id_pattern   = d.id_pattern;

    nlohmann::json go_canon, cpp_canon;
    DiffResult dr;
    try {
        auto gj = nlohmann::json::parse(go.stdout_data);
        auto cj = nlohmann::json::parse(cpp.stdout_data);
        CanonicalizeOptions co;
        co.ignore_paths  = d.tiers.ignore;
        co.corpus_prefix = corpus_path;
        co.preserve_number_paths = d.tiers.ranked;
        co.preserve_number_paths.insert(co.preserve_number_paths.end(),
                                        d.tiers.timed.begin(),
                                        d.tiers.timed.end());
        go_canon  = canonicalize_json(gj, co);
        cpp_canon = canonicalize_json(cj, co);
        dr = compare(go_canon, cpp_canon, opts);
    } catch (const std::exception& e) {
        std::cerr << "infra: mcp parse failed: " << e.what()
                  << "\ngo: "  << go.stdout_data
                  << "\ncpp: " << cpp.stdout_data << "\n";
        return 2;
    }
    if (!dr.passed) {
        fs::path dump_dir =
            fs::path(env_or("PARITY_FAILURES", "build/parity-failures")) / d.id;
        write_dump(dump_dir, d, go.stdout_data, cpp.stdout_data,
                   go_canon, cpp_canon, dr);
        std::cerr << "FAIL " << d.id << "\n";
        for (const auto& r : dr.reasons) std::cerr << "  - " << r << "\n";
        return 1;
    }
    std::cout << "PASS " << d.id << "\n";
    return 0;
}
```

Replace the `Mode::Mcp` arm in `main()` to call `run_mcp_descriptor(d)`. Add `#include "runner/modes/mcp.h"`.

- [ ] **Step 3: Build**

```bash
cmake --build build/debug -j$(nproc)
```

Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add tests/parity/runner/modes/mcp.{h,cpp} \
        tests/parity/runner/parity_runner.cpp
git commit -m "feat(parity): add MCP stdio JSON-RPC mode"
```

---

### Task 3.2: MCP descriptors per tool

**Files:** one descriptor per MCP tool under `tests/parity/descriptors/mcp/<tool>/<input>.parity.json`. Tools list (from MIGRATION.md): `info`, `search`, `get_context`, `semantic_annotations`, `side_effects`, `code_insight`, `find_files`, `list_symbols`, `inspect_symbol`, `browse_file`, `index_stats`, `debug_info`, `git_analysis`, `context_manifest`, `search_definitions`, `grep`, `tree` (17 tools).

- [ ] **Step 1: Write a template + first three descriptors**

Template — `tests/parity/descriptors/mcp/search/basic.parity.json`:

```json
{
  "id": "mcp/search/basic",
  "mode": "mcp",
  "corpus": "multi-lang",
  "invocation": {
    "args": ["mcp"],
    "stdin": "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"search\",\"arguments\":{\"query\":\"add\"}}}",
    "cwd": "${CORPUS}"
  },
  "parse": "json",
  "tiers": {
    "stable": ["result.content[].type","result.content[].text"],
    "ignore": ["id","jsonrpc","result.meta.elapsed_ms","result.meta.server_pid"]
  },
  "tolerances": {"score_abs": 0.05, "timed_max_ms": 60000},
  "expect_exit": 0
}
```

Write `mcp/info/basic.parity.json`, `mcp/index_stats/basic.parity.json`, `mcp/list_symbols/all.parity.json` next, varying `name`/`arguments`.

- [ ] **Step 2: Run the first MCP batch**

```bash
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug -L parity -R 'parity\.mcp\.' --output-on-failure
```

Expected: descriptors run; mismatches dumped. Exact tier shapes will need tuning per tool — adjust `tiers.stable`/`ignore` based on dump output, then re-run.

- [ ] **Step 3: Iterate over remaining tools**

Add one descriptor per remaining tool (≥ 1 input each, ideally 3 inputs each). After each batch, run and tune tiers. Commit per tool.

```bash
# Per tool:
git add tests/parity/descriptors/mcp/<tool>
git commit -m "test(parity): add MCP <tool> descriptors"
```

- [ ] **Step 4: Final batch run**

```bash
ctest --test-dir build/debug -L parity -R 'parity\.mcp\.' --output-on-failure
```

Expected: all green or recognized as bugs (file in MODULE_MAP.md backlog, not silenced).

---

## Phase 4 — Index/data parity

### Task 4.1: Index mode (`debug export --json`)

**Files:**
- Modify: `tests/parity/runner/modes/index.{h,cpp}`
- Modify: `tests/parity/runner/parity_runner.cpp` (Mode::Index dispatch)

- [ ] **Step 1: Implement index mode**

`tests/parity/runner/modes/index.h`:

```cpp
#pragma once

#include "runner/descriptor.h"
#include "runner/modes/cli.h"

namespace lci::parity {

// Indexes the corpus on the given binary, then runs `lci debug export --json`
// in the same dir. Returns the exported JSON.
CapturedOutput run_index_export(const std::string& binary_path,
                                const Descriptor&  d,
                                const std::string& corpus_path);

} // namespace lci::parity
```

`tests/parity/runner/modes/index.cpp`:

```cpp
#include "runner/modes/index.h"

namespace lci::parity {

CapturedOutput run_index_export(const std::string& binary_path,
                                const Descriptor&  d,
                                const std::string& corpus_path) {
    Invocation inv;
    inv.args = {"debug", "export", "--json"};
    inv.cwd  = corpus_path;
    inv.env  = d.invocation.env;
    return run_cli(binary_path, inv, corpus_path);
}

} // namespace lci::parity
```

- [ ] **Step 2: Wire `Mode::Index` dispatch in main**

Add `run_index_descriptor(const Descriptor& d)` analogous to `run_mcp_descriptor`, calling `run_index_export` instead. The diff path is identical to MCP/CLI-JSON.

- [ ] **Step 3: Build and commit**

```bash
cmake --build build/debug -j$(nproc)
git add tests/parity/runner/modes/index.{h,cpp} tests/parity/runner/parity_runner.cpp
git commit -m "feat(parity): add index/data export mode"
```

---

### Task 4.2: Index descriptors per corpus

**Files:** `tests/parity/descriptors/index/{synthetic-multilang,lci-go-repo,lci-cpp-repo}.parity.json`.

- [ ] **Step 1: Write descriptors**

`tests/parity/descriptors/index/synthetic-multilang.parity.json`:

```json
{
  "id": "index/synthetic-multilang",
  "mode": "index",
  "corpus": "multi-lang",
  "invocation": {"args": [], "cwd": "${CORPUS}"},
  "parse": "json",
  "tiers": {
    "stable": [
      "files[].path",
      "files[].language",
      "symbols[].name",
      "symbols[].kind",
      "symbols[].file",
      "symbols[].line",
      "refs[].symbol",
      "refs[].file",
      "refs[].line",
      "trigram_count"
    ],
    "ignore": ["index_version","build_time","host","schema_version"]
  },
  "tolerances": {"score_abs": 0.0, "timed_max_ms": 60000},
  "expect_exit": 0
}
```

Repeat for `lci-go-repo` and `lci-cpp-repo` corpora (just change `id` and `corpus`).

- [ ] **Step 2: Run, triage, commit**

```bash
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug -L parity -R 'parity\.index\.' --output-on-failure
```

Expected: synthetic green; real-repo dumps may surface schema differences. File any divergence as a tracked bug.

```bash
git add tests/parity/descriptors/index
git commit -m "test(parity): add index export descriptors per corpus"
```

---

## Phase 5 — Algorithmic probes

### Task 5.1: Module map document

**Files:**
- Create: `tests/parity/MODULE_MAP.md`

- [ ] **Step 1: Author the map**

`tests/parity/MODULE_MAP.md`:

```markdown
# Module Map — Go LCI ↔ C++ LCI

Lightweight orientation table. Module comparison is opportunistic; only
algorithms that must match get a parity probe.

| Go pkg | C++ home | Mapping | Has algo probe? | Notes |
|--------|----------|---------|-----------------|-------|
| `internal/indexing` | `src/indexing` | mapped | covered by `index/*` descriptors | |
| `internal/parser`   | `src/parser`   | mapped | indirectly via index export | |
| `internal/search`   | `src/search`   | mapped | covered by `cli/search/*` | |
| `internal/semantic` | `src/semantic` | mapped | needs `semantic_annotate` probe | TODO |
| `internal/symbollinker` | `src/symbollinker` | mapped | indirectly via `cli/refs` | |
| `internal/regex_analyzer` | `src/regex_analyzer` | mapped | needs `regex-analyze` probe | TODO |
| `internal/encoding` | `src/string_pool.cpp` + `src/alloc` | merged | n/a | data-layout only |
| `internal/idcodec`  | (TBD)         | unknown | needs `encode-id`/`decode-id` probe if exposed | TODO investigate |
| `internal/cache`    | `src/alloc`   | merged   | n/a | |
| `internal/display`  | `src/cli`     | merged   | covered by `cli/*` text-mode | |
| `internal/git`      | `src/git`     | mapped   | covered by `cli/git/*` | |
| `internal/server`   | `src/server`  | mapped   | covered by Phase 6 HTTP descriptors | |
| `internal/mcp`      | `src/mcp`     | mapped   | covered by `mcp/*` | |
| `internal/metrics`  | (none)        | removed  | n/a | observability folded into structured logging |
| `internal/security` | (n/a)         | removed  | n/a | input sanitization moved into request-time validators |
| `internal/idcodec`  | see above    |          |          |          |

## Backlog: probes to add
- [ ] `lci debug trigrams <input>` — both sides
- [ ] `lci debug score <query> <doc-id>` — both sides
- [ ] `lci debug walk <dir>` — both sides
- [ ] `lci debug link <file>` — both sides
- [ ] `lci debug annotate <symbol>` — both sides
- [ ] `lci debug regex-analyze <pattern>` — both sides
- [ ] `lci debug encode-id <symbol>` / `decode-id <id>` — both sides if exposed
```

- [ ] **Step 2: Commit**

```bash
git add tests/parity/MODULE_MAP.md
git commit -m "docs(parity): add module map and probe coverage backlog"
```

---

### Task 5.2: Algorithmic probe descriptors (only those with probes both sides)

**Files:** `tests/parity/descriptors/probes/<target>.parity.json` for each target where `lci debug <target>` exists in **both** binaries. Skip the rest, leave them as backlog rows.

- [ ] **Step 1: Survey both binaries for available probes**

```bash
${LCI_GO}  debug --help 2>&1 | tee /tmp/go_debug_help.txt
${LCI_CPP} debug --help 2>&1 | tee /tmp/cpp_debug_help.txt
diff /tmp/go_debug_help.txt /tmp/cpp_debug_help.txt
```

- [ ] **Step 2: Write descriptors only for shared probes**

For each shared probe, follow this template (e.g., `tests/parity/descriptors/probes/trigrams.parity.json`):

```json
{
  "id": "probes/trigrams",
  "mode": "cli",
  "corpus": "multi-lang",
  "invocation": {"args": ["debug","trigrams","add"], "cwd": "${CORPUS}"},
  "parse": "json",
  "tiers": {"stable": ["trigrams[]"]},
  "tolerances": {"score_abs": 0.0, "timed_max_ms": 60000},
  "expect_exit": 0
}
```

For any probe missing on one side: open a tracking item under `MODULE_MAP.md` "Backlog" with the exact missing-side binary noted. Do NOT add a descriptor that would skew the result.

- [ ] **Step 3: Run, triage, commit per probe**

```bash
ctest --test-dir build/debug -L parity -R 'parity\.probes\.' --output-on-failure
git add tests/parity/descriptors/probes
git commit -m "test(parity): add algorithmic probe descriptors"
```

---

## Phase 6 — HTTP parity (optional)

### Task 6.1: HTTP mode (ephemeral server lifecycle)

**Files:**
- Modify: `tests/parity/runner/modes/http.{h,cpp}`
- Modify: `tests/parity/runner/parity_runner.cpp`

- [ ] **Step 1: Implement HTTP mode**

`tests/parity/runner/modes/http.h`:

```cpp
#pragma once

#include "runner/descriptor.h"
#include "runner/modes/cli.h"

namespace lci::parity {

// Starts the binary in `lci server` mode in an ephemeral runtime dir,
// makes the descriptor's HTTP request over the Unix socket, captures
// the body, then issues `lci shutdown`.
CapturedOutput run_http(const std::string& binary_path,
                        const Descriptor&  d,
                        const std::string& corpus_path);

} // namespace lci::parity
```

`tests/parity/runner/modes/http.cpp`:

```cpp
#include "runner/modes/http.h"

#include <httplib.h>
#include <filesystem>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <cstring>
#include <cerrno>

namespace fs = std::filesystem;

namespace lci::parity {

namespace {

std::string tmp_runtime_dir(const std::string& test_id) {
    std::string base = "/tmp/parity-" +
        std::to_string(::getpid()) + "-" + test_id;
    std::replace(base.begin(), base.end(), '/', '_');
    fs::create_directories(base);
    return base;
}

pid_t spawn_server(const std::string& binary_path,
                   const std::string& corpus_path,
                   const std::string& runtime_dir) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // child
        setenv("LCI_RUNTIME_DIR", runtime_dir.c_str(), 1);
        chdir(corpus_path.c_str());
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
        execlp(binary_path.c_str(), binary_path.c_str(), "server",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    return pid;
}

bool wait_for_socket(const fs::path& sock, std::chrono::milliseconds budget) {
    auto end = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < end) {
        if (fs::exists(sock)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

CapturedOutput run_http(const std::string& binary_path,
                        const Descriptor&  d,
                        const std::string& corpus_path) {
    std::string rt = tmp_runtime_dir(d.id);
    pid_t srv = spawn_server(binary_path, corpus_path, rt);

    // Find the socket file — convention: <runtime>/lci.sock.
    fs::path sock = fs::path(rt) / "lci.sock";
    if (!wait_for_socket(sock, std::chrono::seconds(10))) {
        ::kill(srv, SIGTERM); waitpid(srv, nullptr, 0);
        CapturedOutput cap; cap.timed_out = true; return cap;
    }

    // Build URL path from descriptor invocation.args[0] = "GET /path" or
    // invocation.stdin_data = JSON body for POST.
    httplib::Client cli(("unix:" + sock.string()).c_str());
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);

    CapturedOutput cap;
    if (!d.invocation.stdin_data.empty()) {
        auto res = cli.Post(d.invocation.args[0].c_str(),
                            d.invocation.stdin_data, "application/json");
        if (res) { cap.stdout_data = res->body; cap.exit_code = res->status; }
    } else {
        auto res = cli.Get(d.invocation.args[0].c_str());
        if (res) { cap.stdout_data = res->body; cap.exit_code = res->status; }
    }

    // Issue shutdown via the same socket.
    cli.Post("/shutdown", "", "application/json");
    waitpid(srv, nullptr, 0);
    fs::remove_all(rt);
    return cap;
}

} // namespace lci::parity
```

- [ ] **Step 2: Wire `Mode::Http` dispatch in main**

`run_http_descriptor(d)` analogous to MCP — parse JSON body, canonicalize, diff.

- [ ] **Step 3: Build and commit**

```bash
cmake --build build/debug -j$(nproc)
git add tests/parity/runner/modes/http.{h,cpp} tests/parity/runner/parity_runner.cpp
git commit -m "feat(parity): add HTTP mode with ephemeral server lifecycle"
```

---

### Task 6.2: HTTP descriptors per endpoint

**Files:** `tests/parity/descriptors/http/<endpoint>.parity.json` for each of the 15 endpoints.

- [ ] **Step 1: Write descriptors**

Endpoints from MIGRATION.md: `/ping`, `/status`, `/search` (POST), `/stats`, `/list-symbols`, `/definition`, `/references`, `/tree`, `/fileinfo`, `/browse-file`, `/inspect-symbol`, `/reindex`, `/git-analyze`, `/shutdown`. Use the descriptor convention: `args[0]` = path, `stdin_data` = body JSON for POST. Example `tests/parity/descriptors/http/status.parity.json`:

```json
{
  "id": "http/status",
  "mode": "http",
  "corpus": "multi-lang",
  "invocation": {"args": ["/status"], "cwd": "${CORPUS}"},
  "parse": "json",
  "tiers": {
    "stable": ["state","files_indexed","symbols_indexed"],
    "ignore": ["pid","start_time","version","uptime_ms"]
  },
  "tolerances": {"score_abs": 0.0, "timed_max_ms": 60000},
  "expect_exit": 200
}
```

(Note: for HTTP mode, `expect_exit` holds the HTTP status code.)

- [ ] **Step 2: Run, triage, commit**

```bash
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug -L parity -R 'parity\.http\.' --output-on-failure
git add tests/parity/descriptors/http
git commit -m "test(parity): add HTTP endpoint descriptors"
```

---

## Phase 7 — CI integration and final docs

### Task 7.1: README triage workflow

**Files:**
- Modify: `tests/parity/README.md`

- [ ] **Step 1: Expand README with full triage workflow**

Append to `tests/parity/README.md`:

```markdown
## Adding a new descriptor

1. Pick a mode: `cli`, `mcp`, `http`, or `index`.
2. Copy the closest existing descriptor from `descriptors/<mode>/`.
3. Update `id`, `args`, `corpus`, and `tiers`.
4. Build and run only the new test:
       cmake --build build/debug -j$(nproc)
       ctest --test-dir build/debug -L parity -R 'parity\.<id>' --output-on-failure
5. If it fails, inspect `build/debug/parity-failures/<id>/`. Adjust tiers
   only when the divergence is structural (e.g., field renamed, ordering
   non-deterministic) — never to silence a real bug.

## Triaging a parity failure

1. Open `build/debug/parity-failures/<test_id>/report.txt` for the reasons.
2. Open `diff.txt` for the unified diff of canonicalized JSON.
3. If the divergence is in a `stable` field, that's a bug — file it.
4. If it's in a `ranked` field beyond `score_abs`, investigate scoring drift.
5. If it's in `timed`, the binary is slow or hung — investigate.
6. If a divergence is intentional (e.g., new C++ field), add it to `ignore`
   in the descriptor with a comment explaining why.

## Adding a new algorithmic probe

If both Go and C++ binaries expose `lci debug <name>` with deterministic
output, add a descriptor under `descriptors/probes/<name>.parity.json` and
update the `MODULE_MAP.md` backlog (check off the row).
```

- [ ] **Step 2: Commit**

```bash
git add tests/parity/README.md
git commit -m "docs(parity): expand README with triage and authoring workflow"
```

---

### Task 7.2: CI integration

**Files:**
- Modify: `.github/workflows/<ci.yml>` if present, or `Makefile`/CI-equivalent. (Inspect first.)

- [ ] **Step 1: Find CI entry points**

```bash
ls .github/workflows/ 2>/dev/null
ls .gitlab-ci.yml Makefile ci/ 2>/dev/null
```

If none of these exist, document the CI hook step in `tests/parity/README.md` instead and skip step 2.

- [ ] **Step 2: Add a parity job step**

In the CI config, after the existing test step add:

```yaml
- name: Run parity tests
  run: |
    ./tests/parity/corpora/prep_real.sh
    ctest --test-dir build/release -L parity --output-on-failure -j$(nproc)
  env:
    LCI_GO: ${{ env.LCI_GO_PATH || '/usr/local/bin/lci' }}
```

- [ ] **Step 3: Commit**

```bash
git add .github/workflows tests/parity/README.md
git commit -m "ci(parity): wire ctest -L parity into pipeline"
```

---

## Done criteria checklist

- [ ] Phase 0–4 green on `synthetic` + `lci-go-repo` + `lci-cpp-repo`
- [ ] Phase 5 probes green for every shared probe; all unshared probes documented as backlog rows in `MODULE_MAP.md`
- [ ] Phase 6 (optional) green for all 15 endpoints
- [ ] `tests/parity/README.md` documents triage + descriptor authoring
- [ ] `ctest -L parity` part of CI

---

## Self-review notes

Spec coverage map:
- §Layout → Tasks 0.1, 1.1–1.7 produce every dir/file listed in the spec.
- §Descriptor format → Task 1.4 (parser) + every descriptor in Phase 2/3/4/5/6 uses the same shape.
- §Tier semantics → Task 1.3 implements stable/ranked/timed/ids/ignore. Defaults stable when unspecified.
- §Canonicalize rules → Task 1.1.
- §Diff engine pipeline → Task 1.3 + parity_runner orchestration in Task 1.6.
- §Runner modes → 1.5 (cli), 3.1 (mcp), 4.1 (index), 6.1 (http).
- §Server isolation → 6.1 uses ephemeral runtime dir under `/tmp/parity-<pid>-<id>`.
- §ctest wiring → Task 0.1 globs descriptors and registers tests under label `parity`.
- §Corpora → Task 0.2.
- §Module map → Task 5.1.
- §Algorithmic probes → Task 5.2.
- §Phasing/acceptance → maps 1:1 to Phases 0–6 above.
- §Risks → mitigations exposed in this plan: tier defaults fail-closed (Task 1.2), ranked tier with `score_abs` (Task 1.3), per-test runtime dir (Task 6.1), JSONPath ignore (Task 1.1), MCP initialize handshake (Task 3.1).

Type/name consistency check:
- `Descriptor`, `Mode`, `ParseStyle`, `Invocation`, `TierMap`, `FieldTier`, `DiffOptions`, `DiffResult`, `CanonicalizeOptions`, `CapturedOutput` are defined once each and used consistently across tasks 1.1–6.2.
- Helper `run_cli`/`run_mcp`/`run_index_export`/`run_http` signatures used in `parity_runner.cpp` match their declarations.
- `LCI_GO`, `LCI_CPP`, `PARITY_CORPORA`, `PARITY_FAILURES` are the four runtime env vars; set in CMake's `set_tests_properties` and read in `parity_runner.cpp`.

No placeholders found; all "TBD" markers in `MODULE_MAP.md` are intentional backlog items, not unresolved plan steps.
