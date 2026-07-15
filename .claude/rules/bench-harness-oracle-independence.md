# Bench-Harness Oracle Independence

Lesson from the S1 exploration-corpus-forge review rewind (task
`01KXEEH7RD3D03VN6EP8ZZEP0F`, "S1 — Forge deterministic mutated exploration
corpora", branch `worktrack/exploration-corpus-forge`, review attempt 1
rewind -> attempt 2 pass, GREEN commit `fdbf4d6`, 2026-07-14/15).

## A validation oracle that shares its matching mechanism with the mutator it validates cannot catch that mutator's bugs

`exploration_corpus_forge.py`'s TypeScript import checker and its
relative-import rewriter both called the same compiled regex
(`_TS_RELATIVE`, used at both the rewrite site and the check site). Any
specifier class the rewriter's pattern didn't match was, by construction,
also invisible to the checker — the two were never independent, so a
dir_rename could silently break 56 real `next/dist/{server,shared,client}/...`
bare first-party specifiers inside `packages/next/src` at the pinned Next.js
commit, and validation still exited 0 and marked the corpus ready.

Review attempt 1 caught this precisely because it treated "does validation
pass" as insufficient evidence and asked "would this validation catch the
exact bug the rewriter can introduce" — i.e. it demanded a **discrimination
test**: inject the specific breakage the rewriter is capable of causing and
assert the checker fails on it (`test_oracle_catches_an_unrepointed_bare_specifier`,
landed in `fdbf4d6`). A checker that only proves "the happy path passes"
without a paired "the known-bad path fails" is not yet proven to be an
oracle at all.

### Reusable rule for any forge/rewriter + validator pair in this repo

1. **Independence check**: the validator's matching/parsing mechanism must
   not be the same code object (regex, parser, AST walker) as the
   transformer's. Shared regex/shared helper = shared blind spot. Give the
   validator its own specifier-matching path even if it's more verbose.
2. **Discrimination test is mandatory, not optional polish**: for every
   mutation class the transformer performs, add a test that (a) re-injects
   the untransformed/broken form and asserts the validator FAILS, and (b)
   confirms the transformed form PASSES. A validator with no proven failure
   case is unverified, regardless of how many corpora it currently passes.
3. **Bare/unslashed reference forms are a distinct class from relative
   (slashed) ones and need their own rewrite + own check**: this epic hit it
   twice in the same review round — TypeScript bare first-party specifiers
   (`next/dist/...` resolving via package alias, not a relative path) and
   Meson `subdir('name')` directives (a bare directory reference, no slash
   at all). A rewriter built only for slashed relative paths will silently
   skip both. When adding a new mutable corpus/language, explicitly enumerate
   which reference forms are slashed-relative vs. bare/aliased before writing
   the rewriter, and cover both in the validator.
4. **Alternative to hardening a self-authored checker: swap in an
   independent external oracle** (e.g. `tsc --noEmit`) when the self-authored
   checker's blind spots are hard to fully enumerate. The epic chose to
   harden the self-authored checker with its own independent specifier set
   (`_TS_ANY_SPECIFIER` + a first-party alias map, distinct from the
   rewriter's `_TS_RELATIVE` + `_rewrite_bare_first_party`); either path is
   acceptable as long as rule 1 (independence) holds afterward.

`source_event: task-01KXEEH7RD3D03VN6EP8ZZEP0F, review-panel step 01KXH30SW99FM2VWYZF7VHPBAM attempt1 (failed, rewind_to bench-unit-tests) -> attempt2 (passed), commits 0bb3d8d (RED: pin the TS oracle's blind spot) + fdbf4d6 (GREEN: repoint bare first-party + subdir specifiers), 2026-07-14T20:54 / 2026-07-15T02:52-02:54`
