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

## 8. A committed declaration ABOUT a gitignored artefact is unverified until a test compares the two — and the check must fail, not skip, when the artefact is present but wrong

Rules 1-7 govern a validator versus the transformer it validates. This is the same
independence problem one level up: the committed banks *describe* the corpus they were
built on, the corpus itself is gitignored, and nothing compared the description to the
thing. All 54 committed banks declared `forge_version` 2 while every local
`.work/exploration/*/seed-7/manifest.json` said 1, and the anchors were demonstrably
authored against the v1 layout — four bank tests failed on a re-forged v2 pocketbase
tree and passed on v1. The label had been bumped with the forge script; the artefact it
described had not. No test held the two side by side, so the split survived 54 banks and
blocked two downstream baselines before a review found it.

Three requirements for any committed artefact that declares a property of a derived,
gitignored one:

1. **Assert the declaration against the artefact in the test suite**, not at baseline
   launch. A version/shape gate that only runs when the long benchmark starts converts a
   test-time failure into a run-time abort hours later.
2. **Pin the artefact's identity in a committed, non-content registry** — here
   `exploration/corpora.json` `reference_forge_version` + `reference_tree_hash` per
   corpus/seed — and check BOTH the artefact's self-pin and the external pin. A manifest
   cannot detect its own rewrite: only the outside pin catches a manifest and tree
   clobbered consistently. The unrecorded original pocketbase v1 hash (`8e0d6d03`,
   `git grep` -> empty) is the cost of not having had this registry; that tree is
   unrecoverable.
3. **Present-but-invalid is a failure; only absent is a skip.** The drift check first
   shipped reporting a present, drifted tree via `SkipTest`, which reads green on every
   run — a host whose scikit-learn tree had silently accumulated 658 stray
   `__pycache__/*.pyc` blobs (writer unknown; re-running the oracle leaves none) still
   passed. Gate the skip on absence of the artefact alone, and prove the discrimination
   by injecting a stray file into a clean tree.

A companion guard: when a discrimination test's failure case depends on two config
values differing (`FORGE_VERSION` != the pinned reference), assert that inequality in
the test. Otherwise a later re-forge makes them equal and the test passes vacuously
instead of announcing that it stopped discriminating.
<!-- written_at: 2026-09-06T16:00:00Z  source_event: task:01M1VMC1DEDYTPXRYNK1VA2CG5, comment:01M1VP25HW4D70S1FAWYD6T5EE, comment:01M1VPMW3R09KB2Q35BDC99FGZ, git:2701344, git:6ef7208, git:87f9e6c -->

## 9. In a two-arm harness, BOTH arms' raw tool output must be rendered into the graded syntax — one rendered arm is a rigged instrument

Rules 1-5 protect the oracle's independence from the thing it grades. This one protects
the ARMS' symmetry, which a correct oracle does not give you. The discovery sweep's
tool-level grader parsed `path:line` citations. The baseline's grep output was rendered
into that syntax by `_grep_answer`; the treatment's raw LCI JSON (`file_path` and `line`
as separate fields) was handed to the same parser, which returned `[]`. The treatment
scored 0.0 on every family **by construction**, and the one real cell that had been run
read as a legitimate capability loss. Fixed in `e7897f5` by
`benchmarks/repo-qa/discovery/runner/rendering.py`, one renderer per tool response shape;
the same real cell then read treatment P=1.0/R=1.0 against baseline P=0.111/R=1.0.

- **A uniform floor is an instrument-defect signature.** When one arm reports exactly
  0.0 across every family, treat it as a harness bug until disproven. A real capability
  loss is ragged; construction-zero is flat.
- **Pin a per-arm fixture captured from the real tool surface**, showing non-zero
  citations. Rule 6's "real material, not fixtures" applies per arm: an invented payload
  would have missed that `callers` emits `call_lines` as a LIST distinct from the
  caller's own definition line — the whole difference between call sites and callers.
- **An unrenderable payload is a BROKEN CELL, never an empty answer.** The grader scores
  an empty answer identically to a wrong one, so a silent `[]` fabricates a loss. Raise
  with a NAMED reason (`unrecognised_tool_shape` / `unparseable_payload` /
  `uncitable_location`) so the downstream analysis slice can attribute an output-format
  gap rather than a capability gap.
- **A renderer must honour the payload's own completeness flags** (`truncated`,
  `total_matches`, `has_more`). LCI `search` caps at 100 hits
  (`handlers_search.cpp:416`; live `Close` 100/409); rendering a capped page as complete
  converts a known cap into an apparent recall loss. Filed `01M1VSHDVWEF7MHQWHW8J5W68H`.
<!-- written_at: 2026-09-06T17:00:00Z  source_event: task:01KXQ2V3P299T64XZ6BXS2QMH1, comment:01M1VR6R67634DGA7G2Z86RCRN, comment:01M1VR77X0B7Z31KTNMWEE8A4J, comment:01M1VS46STNF46BDD0CTKP4JT8, git:4fbb0ac, git:e7897f5 -->

## 10. A claim that a tool, field, or capability is ABSENT must cite the probe — a registry, a docstring, or a prior attempt is not evidence

Attempt 1 routed the caller families through `get_context` on the written claim that
`mcp__lci__callers` does not exist. The claim came from reading `predictions.json` prose.
One `tools/list` call in attempt 2 disproved it: `callers` is live and returns
`file_path` + `call_lines`; only `mcp__lci__references` is genuinely absent. The false
absence cost a rewind and produced the wrong measurement for two families.

An absence claim is load-bearing — everything downstream is designed around the hole —
so it carries the same evidence bar as a security or performance claim: name the probe
(`tools/list`, `tools/call`, the failing import, the `git grep` that returned empty) in
the comment or the docstring that asserts it. Probing also yields the payload SHAPE that
rule 9's fixtures need, so the probe pays for itself twice. Corollary found by the same
probe: `get_context` emits callers and call trees as bare NAMES with no location, so it
cannot answer ANY citation-graded call or reference question — `callers` is the only tool
that emits caller locations with a path (product gap `01M1VR4TBD7GXAXWBQXV2MCR49`).
<!-- written_at: 2026-09-06T17:00:00Z  source_event: task:01KXQ2V3P299T64XZ6BXS2QMH1, comment:01M1VR6R67634DGA7G2Z86RCRN, comment:01M1VS3B70HPMKMRGFS6BVWV5C, git:e7897f5 -->

## 11. Whatever decides WHICH tool is called and HOW its reply is parsed belongs in the hermetically tested package, not the executor script

The discovery runner put the arm executor behind an injected callable — a good pattern
(31 hermetic tests, seconds, no clone, no model). But the real executors lived in
`scripts/discovery_sweep.py`, outside the suite, and **all three live defects were in
that untested half**: the error-payload-graded-as-an-answer, the wrong `get_context`
arguments, and the phantom-tool routing. The implementer's own attempt-1 annotation named
the gap before the reviewer found the defects in it.

Split the harness so the tested package owns the routing table and the payload renderer;
leave only the socket, the subprocess, and the agent outside it. The residual risk — a
test passing against a shape the live server no longer emits — is paid for by pinning the
fixtures from a live capture and recording the corpus and commit in the test.
<!-- written_at: 2026-09-06T17:00:00Z  source_event: task:01KXQ2V3P299T64XZ6BXS2QMH1, comment:01M1VQS5EZKAD73G32Z3AV8F93, comment:01M1VSJQ9F3EWC3MCFDD5KR1FM, git:e7897f5 -->
