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

## 5. The rule covers any soundness check that MIRRORS its extractor — not just forge/validator pairs

Rule 1's "shared code object = shared blind spot" understates the failure: two
*separately written* walkers over the same grammar inherit the same blind spot when
one is authored to mirror the other. `src/cli/grep_filters.cpp` holds exactly that
pair — `regex_literal_seeds` extracts trigram seeds from a pattern, and
`regex_every_match_has_seed` decides whether the trigram fast path is lossless for
that pattern. They share no code, but the coverage check was written to mirror the
extractor's walk, so both treated `\x41`, `\x{41}`, `\pL` and `\Q..\E` argument bytes
as literal text. The extractor banked a seed no match carries; the check certified it;
`lci search -E '\x41bc'` returned a silent empty — a certified-absence defect that
per-branch analysis in the member slice missed and no member test caught. Only the
whole-epic review found it (fixed in place, `9c6ff20`: unsure -> full scan for
`search`, loud error for `grep`, pinned in `AlternationSeedTest.UnseededBranchForcesFallback`).

Applies to any index-narrowing prefilter followed by an exact filter (trigram seeds,
bloom, n-gram). Two additions to rule 2's discrimination test:
- Enumerate the **escape and metacharacter classes** the walker must handle, and pin
  one case per class. A slice whose acceptance criteria name only the shapes the
  author thought of will ship the shapes they did not.
- Pin **both directions**: an unseeded shape forces the fallback, a seeded shape keeps
  the index. A one-directional test lets the check decay into a blanket downgrade.
<!-- written_at: 2026-09-05T23:30:00Z  source_event: task:01M1NCSJ31EQ7WZ9GEEB8CDCY5, comment:01M1SWJGAX1RRNBG64E34M7ZDW (systemicObservations, costGate verdict fix-now), git:9c6ff20 -->

## 6. Verify a leak gate by re-injecting REAL committed answer-key material, and enumerate the needle's GRANULARITIES

Rule 2 demands a discrimination test; rule 5 adds "enumerate the escape classes". The
third axis a mirror-blind walker misses is **granularity of the same needle**. The
edit-bank leak check matched an answer-key patch path only as its full contiguous
token run, so a prompt naming `apis/record_crud.go` was caught while one naming just
`record_crud.go` or `next-app-loader` passed — and the basename hands the agent the
target as surely as the full path does. The synthetic-fixture tests that shipped with
the check never exercised a partial-path form; the miss surfaced only when the
whole-epic review re-injected three REAL committed patches (`pb-api-1`, `skl-log-1`,
`nx-retry-2`) into their REAL prompts. Path-segment needles landed in `4e248e8`
(single-token segments like `apis`, `src`, `sklearn` stay skipped — they cannot
discriminate); the target-SYMBOL granularity is still ungated and filed as
`01M1VJCZ109H2KVN8Y3T9T5P86`.

For any prompt/answer-key linter in `benchmarks/repo-qa` (exploration, edits, future
banks):
- Enumerate the granularities at which the secret is still a give-away — full path,
  each multi-token path segment, basename, target symbol, distinctive content line —
  and pin one discrimination case per granularity.
- Prove the gate on **real committed material**, not fixtures. A fixture set is
  authored from the same mental model as the checker, so it inherits the checker's
  blind spot; the committed bank is not.
<!-- written_at: 2026-09-06T15:00:00Z  source_event: task:01KXPDP6VBM33EP5088AY2WQTP, comment:01M1VJEGARVAW30VERNZMPX704 (systemicObservations, costGate frequency=24, verdict fix-now+file), git:4e248e8 -->

## 7. A foundation/mechanism slice must smoke against EVERY real corpus it will serve, not one

A slice that builds the shared mechanism other slices consume is tested, by default,
against whatever fixture its author invented — and a synthetic fixture has no
symlinks, no dotted directory names, and no upstream package naming. Both of the
edit-bank foundation's residual defects were found by the THIRD consuming member,
after two corpora had already passed:

- `shutil.copytree` follows symlinks by default, so a corpus holding a link to a
  target absent on this host made materialization raise and the gate stamp
  `TOOL_FAILURE` for every task in that corpus (`4646883` RED / `cd52c95` GREEN).
- The `oracle_patch.path` segment class allowed only `[a-z0-9-]`, rejecting
  `patches/next.js/<id>.json` — corpus dirs take their upstream package name
  verbatim, so every next.js answer key was unvalidatable (`fc0ede5` RED /
  `cc4ce56` GREEN).

Both were mechanism gaps owned by the foundation member, discovered and paid for by a
downstream member whose own subject was answer keys, not plumbing.

Rule: a foundation slice's acceptance includes a smoke run of the mechanism against
every real corpus in `benchmarks/repo-qa/.work/exploration/` it is expected to serve —
one materialization plus one validate per corpus — before it is declared done. The
corpora differ in exactly the ways a hand-built fixture does not.
<!-- written_at: 2026-09-06T15:00:00Z  source_event: task:01M1T0WJFF6JM6TA5HEB3MJVTT (foundation), task:01M1T0WJRHKB1HTRZE9DV7CC8T (next.js member, found both), git:cd52c95, git:cc4ce56 -->
