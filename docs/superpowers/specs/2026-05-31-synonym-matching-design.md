# Synonym Matching for LCI C++ Search — Design

**Date:** 2026-05-31
**Status:** Approved design, pre-implementation
**Author:** brainstormed with Andy Brummer

## Summary

Add synonym matching to LCI search so a query for one word finds code using an
equivalent word: `login`↔`signin`, `add`/`insert`, `update`/`upsert`,
`delete`/`remove`/`erase`. Synonyms are **bidirectional equivalence groups**.

The feature spans both search surfaces:

- **Symbol search** (`def` / `symbols` / `inspect` / `refs`, via `SemanticScorer`):
  a new ranked match tier, alongside exact/stemming/abbreviation/fuzzy.
- **Text search** (the engine's pattern path, via `expand_pattern_semantic`):
  query terms expand to their synonyms; results union.

Synonym sets come from a **built-in curated table** (minimal dev-verb set), with a
`.lci.kdl` `synonyms` block that can add, override, clear, and clear-all.

This is net-new. There is no Go parity reference — Go's `phrase_matcher.go` (mirrored
by the existing C++ `abbreviation_table`) carries only directional abbreviations, not
synonyms. Synonyms are a distinct relation and live in their own table and detector.

## Non-goals

- No thesaurus-scale word lists. The built-in set is the small, high-confidence
  dev-verb set in §2; breadth is the user's job via KDL.
- No runtime mutation of the table. It is built once at config load and frozen.
- No change to the existing abbreviation feature. Abbreviation (directional,
  short→long) stays exactly as-is and byte-parity-tested.
- Synonym weight is a fixed default, not KDL-configurable (YAGNI).

## 1. Data model — `SynonymTable`

New focused unit: `include/lci/semantic/synonym_table.h` + `src/semantic/synonym_table.cpp`.

```cpp
namespace lci {

/// Immutable bidirectional synonym groups. Built once at config load, then
/// shared read-only by const reference (no read-path lock — Karpathy rule 3).
/// Groups are DISJOINT equivalence classes: every word belongs to at most one
/// group. All words are lowercased at build time.
class SynonymTable {
  public:
    /// Built-in curated dev-verb groups (§2).
    static SynonymTable build_default();

    /// Applies KDL operations (§5) to the built-in baseline (or to empty when
    /// clear-all is present) and returns the frozen result. Returns an
    /// lci::Error on validation failure (fail-fast, Karpathy rule 6), using the
    /// project's Result<T> type (include/lci/error.h).
    static Result<SynonymTable> build_from_ops(std::span<const SynonymOp> ops);

    /// Members of `word`'s group, EXCLUDING `word` itself. Empty if `word`
    /// is in no group. `word` is matched case-insensitively. No allocation:
    /// heterogeneous string_view lookup into owning storage.
    std::span<const std::string> synonyms_of(std::string_view word) const;

    /// True if `a` and `b` are distinct words in the same group.
    bool in_same_group(std::string_view a, std::string_view b) const;

    bool empty() const { return groups_.empty(); }

  private:
    std::vector<std::vector<std::string>> groups_;          // owning, lowercased
    absl::flat_hash_map<std::string, uint32_t> word_to_group_;  // word -> group index
};

}  // namespace lci
```

`word_to_group_` uses Abseil's transparent (heterogeneous) hashing so
`synonyms_of(std::string_view)` and `in_same_group` probe without constructing a
temporary `std::string`. Callers lowercase their query word before lookup (the
symbol path already has `query_lower`/`target_lower`; the text path lowercases the
extracted term).

Disjointness is an invariant enforced at build time: if the same word would land in
two groups, `build_from_ops` returns an error (KDL path) or the default builder is
written to be conflict-free by construction (built-in path).

## 2. Built-in table (minimal dev-verb set)

Disjoint equivalence groups. Initial set (tunable during implementation; keep
each word in exactly one group):

```
{add, insert, append, push}
{delete, remove, erase, destroy, drop}
{update, modify, edit, upsert}
{get, fetch, retrieve, load, read}
{set, store, save, write, put}
{login, signin, authenticate}
{logout, signout}
{create, make, new, build}
{find, search, lookup, query}
{start, begin, init}
{stop, end, halt, terminate}
{check, validate, verify}
{parse, decode, deserialize}
{encode, serialize, marshal}
{connect, open}
{disconnect, close}
{enable, activate}
{disable, deactivate}
```

Disjointness note: words like `read`/`write`/`load`/`save` are assigned to a single
group each as listed above. If implementation surfaces a word that genuinely wants
two senses, prefer dropping it from one group over splitting the invariant — the
table stays disjoint.

## 3. Symbol scorer wiring

### 3.1 Match type and weight (`include/lci/semantic/score_types.h`)

- Add `MatchType::Synonym` to the enum (between `Abbreviation` and `NameSplit`
  is fine; ordering of the enum is not score-significant) and a
  `to_string` arm returning `"synonym"`.
- Add `double synonym_weight{0.6};` to `ScoreLayers`. Rationale: a synonym match
  (`login`↔`signin`) is a strong semantic equivalence — ranked **above stemming
  (0.55)** and **below fuzzy (0.70)**. Fixed default, not configurable.

### 3.2 `SynonymMatchDetector` (`semantic_scorer.{h,cpp}`)

Mirrors `AbbreviationMatchDetector` (`src/semantic/semantic_scorer.cpp:341`):

```cpp
class SynonymMatchDetector final : public MatchDetector {
  public:
    SynonymMatchDetector(const NameSplitter& splitter, const SynonymTable& table);
    DetectResult detect(std::string_view query, std::string_view target_name,
                        std::string_view query_lower, std::string_view target_lower,
                        const ScoreLayers& config) const override;
  private:
    const NameSplitter& splitter_;
    const SynonymTable& table_;
};
```

`detect` logic:

1. Split `query_lower` and `target_lower` into words via `splitter_` (same splitter
   the abbreviation/name-split detectors already use; splits are cached).
2. For each query word `qw` and target word `tw`, record a match when
   `table_.in_same_group(qw, tw)` (which already excludes `qw == tw`).
3. If any matches: `score = config.synonym_weight * (matched / denom)`, with `denom`
   computed the same way `AbbreviationMatchDetector` does (based on query-word and
   target-word counts) so scoring is consistent across the semantic tiers.
4. Justification: `"Synonym match: <qw> ↔ <tw>"` (joined for multiple); `details`
   map mirrors the abbreviation detector's keys (`query`, `targetName`, `matches`).

Register the detector in the scorer's pass chain next to the abbreviation pass.
Per-word "best match wins" accumulation is unchanged — synonym is just another
candidate tier. Because the matrix is query-words × target-words, both directions
fall out for free (`erase` query vs `delete` target and the reverse).

### 3.3 Wiring

`SemanticScorer` gains a `const SynonymTable&` (passed at construction, owned by
`Config`). Default-constructed scorers in tests use `SynonymTable::build_default()`.

## 4. Text search expansion

Extend `expand_pattern_semantic` (`src/search/engine.cpp:143`, called from
`src/mcp/handlers_core.cpp:688`). The function gains access to the active
`SynonymTable` (passed in, or via the engine holding a `const SynonymTable&`).

Behavior (always-on):

1. Keep existing output (original pattern first, then >2-char split words).
2. For each retained word, append its `synonyms_of(word)` members (deduped against
   `seen`, preserving original-first score priority).
3. **Bound:** cap the total expanded pattern count at `kMaxSynonymExpansion = 16`.
   Group sizes are ~3–5, so single-word queries stay well under; the cap only bites
   on multi-word queries. The cap is a named constant documented in the function
   header — no silent unbounded growth (Karpathy rule 6).

Case handling (confirmed): synonym-expanded terms are matched **case-insensitively**
regardless of the base query's `-i` flag, so expanded `signin` matches code `signIn`.
Synonyms are word-concepts, not literal strings. The multi-pattern search path
(`SearchEngine::search(patterns, options)`, `engine.cpp:357`) currently applies one
shared `options` to every pattern; this design adds a **per-pattern case-insensitive
override** so the originally-typed pattern keeps the user's case flag while
synonym-injected patterns force case-insensitive. The expanded set is tagged so the
engine knows which patterns are synonym-injected. Results union and dedup through the
existing multi-pattern merge.

## 5. KDL configuration and merge

A `synonyms` block in `.lci.kdl`, parsed by the existing hand-written KDL parser in
`src/config/config.cpp`.

```kdl
synonyms {
    clear-all                                   // optional, must be the FIRST child
    group "delete" "remove" "erase" "destroy"   // define / add a group
    group "get" "fetch" "retrieve"              // shares "get" with built-in → overrides it
    clear "update"                              // drop the built-in group containing "update"
}
```

Parsed into an ordered `std::vector<SynonymOp>`:

```cpp
struct SynonymOp {
    enum class Kind { Group, Clear, ClearAll } kind;
    std::vector<std::string> words;  // Group: members; Clear: single word; ClearAll: empty
};
```

Merge, applied by `SynonymTable::build_from_ops` in document order:

1. Baseline = `build_default()`, unless a `clear-all` op is present → baseline = empty.
2. Each `clear "word"` removes the group currently containing `word` (no-op if none).
3. Each `group w1 w2 …` is added. If any listed word already belongs to an existing
   group, that existing group is **removed first** (override), then the new group is
   inserted. This preserves the disjointness invariant with KDL winning.
4. Freeze.

Validation (fail-fast, returns `lci::Error` via `Result<SynonymTable>`; Karpathy rule 6):

- A `group` with fewer than 2 words is an error (a 1-word group is meaningless).
- The same word appearing in two different `group` ops **within the same file** is
  an error (ambiguous intent).
- `clear-all` not in first position is an error (ordering must be unambiguous).
- All words lowercased on load; whitespace-only/empty words rejected.

`Config` holds the resulting `SynonymTable` and hands `const&` to the
`SemanticScorer` and `SearchEngine`.

## 6. Performance (Karpathy compliance)

- **Rule 3 (no read-path mutex):** table built once at config load, immutable,
  shared by `const&`. No locking on `synonyms_of`/`in_same_group`.
- **Rule 2 (no alloc in inner loop):** heterogeneous `string_view` lookup, no temp
  string per probe; detector reuses cached `NameSplitter` splits; expansion vector
  reserved; built-in groups built once.
- **Fast path:** a query word not in any group costs a single `flat_hash_map` miss.
  Non-synonym queries pay ~one hash probe in the symbol scorer and in expansion.
- No `shared_ptr` on the read path, no `std::regex`, no per-token `std::string`
  returns (`synonyms_of` returns a `span` into owning storage).
- Bench gate: symbol search and text search measured with and without the synonym
  pass; non-matching-query latency must not regress vs the pre-feature baseline.

## 7. Testing (real data, no mocks — Karpathy rule 5)

**Unit (`tests/semantic_test.cpp`):**
- `SynonymTable`: build_default disjointness; `synonyms_of` returns group-minus-self;
  `in_same_group` symmetry and self-exclusion; unknown word → empty.
- `SynonymMatchDetector`: `login`→`signIn` and reverse; multi-word targets;
  no false match for unrelated words; score uses `synonym_weight`.
- `expand_pattern_semantic`: includes synonyms; respects `kMaxSynonymExpansion`;
  original pattern stays first.

**Config (`tests/config_test.cpp`):**
- KDL parse of `group`/`clear`/`clear-all`.
- Merge: add new group; override built-in via shared word; `clear` removes a group;
  `clear-all` drops built-ins.
- Validation errors: 1-word group; duplicate word across groups; misplaced
  `clear-all`.

**Integration (real corpus):**
- Symbol: `def login` finds `signIn`; `sym erase` ranks `deleteRecord`.
- Text: search `delete` finds `eraseRecord`/`removeItem` case-insensitively.

**Parity:** net-new, no Go reference. Add C++-only parity descriptors with a
`_rationale` field documenting the intentional divergence (Karpathy parity discipline).

**Perf:** benchmarks per §6.

## 8. Files touched

New:
- `include/lci/semantic/synonym_table.h`
- `src/semantic/synonym_table.cpp`

Edited:
- `include/lci/semantic/score_types.h` — `MatchType::Synonym`, `synonym_weight`
- `include/lci/semantic/semantic_scorer.h` — `SynonymMatchDetector`
- `src/semantic/semantic_scorer.cpp` — detector impl + pass registration
- `src/search/engine.cpp` — synonym expansion + per-pattern case override
- `include/lci/search/search_engine.h` — `expand_pattern_semantic` signature / engine holds table
- `src/config/config.cpp` + `include/lci/config/config.h` — KDL `synonyms` parse, merge, `SynonymTable` member
- wiring: `Config` → `SemanticScorer` and `SearchEngine`
- `tests/semantic_test.cpp`, `tests/config_test.cpp`, integration descriptors

Keep `synonym_table.{h,cpp}` small and focused — it owns one concept (the table),
nothing else.
