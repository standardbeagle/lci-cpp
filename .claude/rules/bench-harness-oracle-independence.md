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
- **The rule is not about linters — it holds for EVERY emitter/consumer pair in this
  bench.** Any consumer of a `bench.py` / sweep row (token totals, timing, result rows)
  builds its fixture by copying one committed real row and mutating it. D5 shipped an
  invented `tokens.cache` nested object; `bench.py:154-158` emits FLAT
  `cache_read`/`cache_write`, so `_token_total`'s "sum every numeric key" rule absorbed
  ~85% cache traffic into a figure the decision document labelled billable — "5.2x fewer
  tokens" was really 2.7x. The fixture passed because it was authored from the same model
  as the code. Third recurrence of this shape in `benchmarks/repo-qa` since 2026-08
  (rules 5, 6, 7).
- **An aggregator over an upstream emitter's fields ENUMERATES what counts and FAILS on
  the rest.** "Sum every numeric key" has no contract: it silently adopts every field the
  emitter adds next, and the drift lands in a quoted cost claim rather than in a test.
  Landed as an explicit allowlist (`input`/`output`/`reasoning`) plus `ValueError` on any
  unrecognised counter (`5bf46bc` RED / `174a4d6` GREEN). Keep a test that REJECTS the
  previously-invented shape, or the next author re-invents it.
- **NAME is not the contract; TYPE, required-ness, and enum membership are separate
  granularities of the same needle.** A "the example cites real parameters" gate that
  checks key membership only (`set(args) <= properties`) certifies an invocation the real
  tool rejects. Two of fifteen variant-C worked invocations (13%) were type-wrong while
  the gate was green — `context.refs` given as a comma string against an array of
  `{f,l}` objects, `git_analysis.focus` as a string against an array — so the C arm would
  have shipped into E2.3 measuring wording plus an uncallable claim. An example or
  invocation embedded in bench material is validated with the REAL schema validator
  against the LIVE schema (`jsonschema.validate` is importable under `/usr/bin/python3`),
  never a hand-rolled key check, and the paired discrimination case is type-wrong, not
  name-wrong (`430334f` RED / `df522f6` GREEN).
- **An extractor that can silently truncate its subject must fail on truncation.** The
  same gate pulled the example out of prose with a non-nesting brace regex, so for a
  nested example it validated the INNER object and reported green on a fragment. Decode
  embedded JSON with the JSON decoder (`raw_decode` from the first brace), never a brace
  regex; a partial parse is an error, not a smaller input.
<!-- written_at: 2026-09-06T20:10:00Z  source_event: task:01KXSHA3PEAEHXDQ7XZVXW3186, comment:01M1W40JEAEJZQCF3GN6XG79TD (systemicObservations, costGate frequency=2, verdict fix-now), comment:01M1W4D8JFVZ2MNBNABD5AKSEP, git:430334f, git:df522f6 -->
<!-- written_at: 2026-09-06T19:00:00Z  source_event: task:01KXQ2V3PW91QTTQ1NTZXH7PR6, comment:01M1W02T0C1W20W449Q8XXRNV8 (systemicObservations, costGate frequency=3, verdict file), comment:01M1W0DFH9TRFTSZKK7418ZAGE, git:174a4d6 -->
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
Same shape, one step further: **a test whose subject is served by a permissive
stand-in cannot fail at all.** E2.2's "name and schema are constant across variants"
test rendered every arm through the mock and diffed `(name, inputSchema)` tuples — but
`scripts/mock_lci_mcp.py:171` serves an empty permissive `inputSchema` for every tool,
so the tuples were equal by construction and the gate proved nothing about the arms.
The committed note beside the arms CLAIMED the mock served the manifest schema. Before
asserting an invariant, check what actually supplies the value: if a stub, mock, or
default supplies it uniformly, the assertion is vacuous and must either move to the real
supplier or be deleted. And a prose note explaining WHY an experiment holds something
constant is a load-bearing claim about code that may sit outside `fileScope` — verify it
against the file that ships it (here the mock, one directory away; corrected in
`df522f6`, real-schema mock filed as `01M1W40XF2E0HKT05SS51A5PJ2`).
<!-- written_at: 2026-09-06T20:10:00Z  source_event: task:01KXSHA3PEAEHXDQ7XZVXW3186, comment:01M1W402AFKB7SPSJJRNKYQ04B (advisory), comment:01M1W4D8JFVZ2MNBNABD5AKSEP, git:df522f6 -->

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

## 12. An A/B arm definition is unproven until a test shows the treatment CANNOT reach the baseline's mechanism — pre-registration of PREDICTIONS does not cover it

D3 pre-registered hypotheses, thresholds and falsifiers, and D5 applied them honestly.
None of that examined whether the two arms were disjoint. `bench.py:58 workspace_config`
ADDS the LCI MCP server to the treatment agent and never disables native grep/glob, so
every agent-level "treatment" number ever produced by this harness means *LCI in addition
to grep*, not the registered *LCI instead of grep* (filed
`01M1W0147178Z2PZNZ6ZQQQBRR`). The matching tool-level defect is the mirror: the baseline
is unfiltered grep with recall 1.000 and precision 0.05-0.56 everywhere, so every
tool-level LCI "win" measures a filtering step the baseline was never given. The epic's
null result is therefore instrument-first, and the literal-search control — the one cell
that could prove an LCI loss — had no teeth at the agent level.

- **Pin arm disjointness with a test, before any cell runs.** The treatment must be
  unable to reach the baseline's mechanism, and the baseline unable to reach the
  treatment's. Assert on the produced arm config (tools enabled/disabled, binary path),
  not on the registry's prose description of the arms.
- **Pre-flight one smoke cell per arm and read the tool-call trace for the forbidden
  mechanism.** Minutes. D5 spent 60 agent cells and 3 tool sweeps on an instrument later
  shown non-disjoint. A treatment trace containing a `grep`/`glob` call is a stop.
- **Both arms must be handed the same class of post-processing.** A raw retrieval tool
  versus an index is not an A/B of capability; it is an A/B of whether a filtering step
  exists. Give the baseline its classification step or state, in the report, that the
  delta measures filtering.
- **Every arm-defining knob must reach every measurement level.** The sweep's
  `--lci-bin` reached only the tool level; the agent level silently used `config.kdl`'s
  default binary, so the two levels exercised different builds (filed
  `01M1W01483B3PBGDF58B75CA49`).
<!-- written_at: 2026-09-06T19:00:00Z  source_event: task:01KXQ2V3PW91QTTQ1NTZXH7PR6, comment:01M1VZMG8NK4DZ54NG45XCHDF8 (outOfScopeDefectsReported), comment:01M1W0MNSG8YRWW3GJA934BVMS, task:01M1W0147178Z2PZNZ6ZQQQBRR, task:01M1W01483B3PBGDF58B75CA49 -->

### 8a. The same rule applies to a snapshot of a LIVE surface — and a count derived from it is a hidden second copy

Rule 8's artefact is gitignored and derived. Its sibling is the opposite shape and fails
the same way: `comprehension/surface/tool-surface.json` is a committed snapshot of a
surface the binary serves on demand. It was pinned 2026-07-17; the server gained
`callers` on 2026-09-02 (`01ba88f`); nothing compared the two, so for four days every
bench built on it measured a 14-tool surface that no longer existed — including the
selection matrix for the very tool whose mis-selection motivates the tool-calling epic.
Nothing failed. Green throughout.

- **Any committed snapshot of a live surface carries a freshness gate that runs whenever
  the producer is present.** Present-and-different is a failure; only absent is a skip
  (rule 8.3). Cheap here: `lci mcp` answers `tools/list` in ~10 ms with no indexing, so
  the live compare adds nothing meaningful to the suite
  (`test_manifest_equals_the_live_tools_list`, proven RED against the stale manifest at
  `be36bd3` before the refresh).
- **A count hard-coded from a snapshot is a second, undeclared copy of it.** The
  comprehension A/B bank asserted a width of `28`. That constant is precisely how a
  surface addition read green: the derived invariant is `2 x live tool count`. Derive
  every such number from the snapshot; a literal that happens to equal it today is a
  latent lie.
- **Grep for readers before refreshing a snapshot, and budget for all of them.** The
  reviewer's blocker named 3 consumers; there were 5 — the toolcalling mock,
  `tool-cases/chi.json`, the format-variants bank, `live-captures.json`, and the
  hard-coded bank width — three of them owned by a different epic. The under-count cost
  the coordinator a second scope widening mid-rewind. `grep -rl <snapshot> benchmarks/`
  is the first step of the refresh, not a discovery made during it.
<!-- written_at: 2026-09-06T20:00:00Z  source_event: task:01KXSHA3NRNK2JB9T43F3X5NYY, comment:01M1W1JHMNFK283DJ9NXZM6SY3, comment:01M1W2W200F212FTW2YZFFBYD1, git:cca526d, git:be36bd3, git:01ba88f -->

### 10a. The mirror: an answer key that names a tool as CORRECT must quote that tool's own contract text

Rule 10 bars an unprobed claim of absence. The same bar governs the positive claim, and
this epic paid for it: the selection bank's baseline descriptions were hand-written
prose, and three tasks were keyed to capabilities the prose invented — `context` as a
line window (live: it saves and hydrates handoff manifests, and has no line parameter),
`git_analysis` as authorship history (live: duplication, naming and complexity over
changed code), `debug_info` as a per-path parse record (live: index internals). Each
task stated a need NO live tool serves, so under the live-description arm it had no
correct answer and would have scored a model wrong for correctly finding no fit. The
three were deleted, not relabelled (`09abd89`).

- **A paraphrased contract is an invented one.** The arm that claims to measure the real
  surface is the live descriptions VERBATIM, pinned by a test. Neutral or reworded
  phrasings are variants, and a variant is what the experiment manipulates — never what
  it baselines against.
- **Require a verbatim fragment, not a review.** Each task carries
  `answer_key_evidence`: a substring of its correct tool's live description, gated by
  test. This makes invention inexpressible rather than detectable — there is no fragment
  to cite for a capability no description claims. Pair it with the discrimination case
  in both directions (rule 2): `test_evidence_gate_rejects_an_invented_capability` and
  `test_verbatim_gate_catches_a_reworded_baseline`.
- **When two tools are genuinely near-duplicates, discriminate on the live CONTRACT, not
  on payload width.** `inspect_symbol` returns all metadata including callers, so
  "everything about one symbol" fit it and `get_context` equally; the contracts differ
  only in input — object ids from a prior result versus a bare name — so that is the only
  discriminator that survives contact with the real surface.
- **Real surface redundancy will read as model confusion.** `code_insight mode=git_analyze`
  and `git_analysis` run the same `git::Analyzer` (`src/mcp/handlers_analysis.cpp:682`).
  A rationale asserting one "has no notion of what changed" is false; the answer key
  holds on description specificity alone. Record such cells as surface redundancy before
  the results slice reads them as a selection failure.
<!-- written_at: 2026-09-06T20:00:00Z  source_event: task:01KXSHA3NRNK2JB9T43F3X5NYY, comment:01M1W1JHMNFK283DJ9NXZM6SY3, comment:01M1W2W200F212FTW2YZFFBYD1, comment:01M1W384M3JE6V2CT2378GCVKN, git:09abd89, git:5b97c51 -->

### 10b. A vendor provider/model id is an absence claim in disguise — probe `opencode models`, and name the hang

Rule 10 bars an unprobed claim that something is missing; 10a bars an unquoted claim that
something is present. A model id is both at once, and it fails silently in a way a tool
name does not: an unserved id produces no "not found", it produces a plausible-looking
provider failure. Commit `869898a` asserted the weak id `opencode/deepseek-v4-flash-free`
was "registered in config.kdl". No `config.kdl` contains it and `opencode models` does not
serve it — the id is dead. The two weak cells that ran on it returned an opaque
`UnknownError` in 5.9 s and then a 300 s timeout with a ZERO-EVENT stream, and both read
as provider flakiness. The real id is `opencode-go/deepseek-v4-flash` (probed: answered in
28.4 s, `lci_callers` first).

- **Every vendor id a bench arm names is verified against `opencode models` by a
  pre-flight or a test, not against a memory, a sibling script, or a `config.kdl` alias.**
  The probe is one command and it also gives you the tier and the served-name spelling.
- **A hang-to-timeout with an EMPTY event stream is a named provider outcome
  (`provider_unserved` / `provider_timeout`), never a selection or comprehension result.**
  Zero events means the arm never ran; scoring it as a miss makes a typo look like a model
  weakness. Same denominator discipline as rule 12's non-selection outcomes.
- **A dead id spreads by copy.** After the fix here, `grep -rn <dead id> benchmarks/`
  still found it in `comprehension_ab.py` and `analyze_comprehension.py`
  (filed `01M1W646T76HF9CDAPBGYW57XP`). Grep for the id across `benchmarks/` in the same
  commit that corrects it — rule 8a's "grep for readers before refreshing" applied to ids.
<!-- written_at: 2026-09-06T20:30:00Z  source_event: task:01KXSHA3Q1QCHM8T68WS6MS1NC, comment:01M1W63EGNJXC9XMVWAF0ZXXMK (probes.opencode_models, probes.weak_old_id), comment:01M1W63SVJ0A342RZVHWNWM2Z0 (systemicObservations), git:869898a, git:cc113ae -->

### 9a. A parser fixture is a VERBATIM excerpt of a recorded real stream, and it names the recording

Rule 6 says prove the gate on real material; rule 9 says pin a per-arm fixture captured
from the real tool surface. Third recurrence, so the clause gets teeth: this task's
`first_call_native` was never assigned, because the hand-built fixture event omitted
`part.type="tool"` — a field the real opencode 1.18.26 stream carries on every tool event.
The suite was green: 22 tests passing against a shape the provider does not emit. The
defect surfaced only when a live probe stream was read next to the fixture.

A fixture authored from the same mental model as the parser inherits the parser's blind
spot, exactly as rule 6's synthetic leak-check fixtures did. So:

- **Author no parser fixture by hand.** Capture a real stream, excerpt it byte-for-byte,
  and paste the excerpt. Trimming events is allowed; editing the shape of one is not.
- **Name the recording in the test** — tool + version + the command that produced it
  (here: `opencode 1.18.26`, `opencode run --format json`). A fixture with no cited source
  is unverified, and the version is what tells the next reader when to re-capture.
- **A field the parser reads and the fixture never sets is the signature.** If an
  assignment can never fire under the fixture set, the fixture set is incomplete — assert
  the positive case for every branch the parser has, not only the ones the author expected
  to take.
<!-- written_at: 2026-09-06T20:30:00Z  source_event: task:01KXSHA3Q1QCHM8T68WS6MS1NC, comment:01M1W63EGNJXC9XMVWAF0ZXXMK (fixedInPlace: first_call_native never assigned; fixture event() now carries part.type=tool), git:cc113ae; recurrence: rule 6 (leak-gate fixtures), rule 9 (per-arm rendering fixtures) -->

### 8b. A "Reproducing" recipe in a committed report is a declaration about a derived artefact — execute it and cmp against the committed output

Rule 8 governs a committed label describing a gitignored tree; a reproduce recipe is the
same shape with the artefact being the report's own numbers. E2.4's recipe passed
`rep*/shard*.jsonl` to `analyze_selection.py`, but every `--ledgers` path is treated as one
rep — so the recipe produced 12 reps with a per-shard spread and a JSON that differed from
the committed `selection-analysis.json`. Only the reviewer re-running it caught this
(corrected in `0b47e24`: shards merged into `rep1..3.jsonl`, `--exclude-ledger` carried,
`--help` no longer citing a nonexistent `--rep-glob`). This is the third recipe/string drift
in one session — the D5 `--not-run` string and the E2.2 arm note being the others — so the
class is recurrent, not incidental.

- **Run the recipe verbatim and `cmp` its output against the committed artefact** before
  the report is committed. Reading it is not executing it.
- **Any analyzer taking a list of ledgers silently accepts shards as reps.** Two analyzers
  in `benchmarks/repo-qa/scripts/` take `nargs="+"` ledger lists with no rep-identity
  check. Until a sidecar `shard` key is refused in code, the recipe is the only guard and
  must be executed.
<!-- written_at: 2026-09-06T23:55:00Z  source_event: task:01KXSHA3QKT5RDT4F08RA08H1N, comment:01M1WHW7X2KRSA1V6QJRNC7V2Y (fixedInPlace), comment:01M1WHWNM6ECEJ2TP6X1QF5CHE (systemicObservations costGate), git:0b47e24; recurrence: D5 --not-run string, E2.2 arm note -->

## 13. A resume ledger must distinguish TERMINAL outcomes from RETRYABLE ones

Skip-if-a-record-exists is the standard idempotence trick for an expensive grid, and it is
correct only if every recorded outcome is final. It never is: a provider outage, a
timeout, a quota kill and a non-zero exit are all recorded like results, so the first
transient failure freezes that cell permanently. The grid then reports a hole that reads
as missing data rather than as an unretried error, and the only recovery is deleting
records by hand.

- **The skip predicate reads the outcome, not the record's existence.** Terminal: any
  graded result. Retryable: the named non-selection outcomes of rule 12
  (`provider_error`, `provider_timeout`, `provider_quota`, `malformed_provider_stream`,
  `exit_N`).
- **Retrying is opt-in and the default stays a no-op**, so an in-flight resume is
  unchanged; last record wins. Landed here as `selection_ab.py --retry-provider-failures`.
- **An in-invocation `--retries` does not satisfy this.** It cannot survive the process,
  which is the case the ledger exists for.
- Same defect still lives in the template this was copied from —
  `scripts/bench.py:253` skips any existing result file while `run_one` writes one for
  every status (filed `01M1W6858497HAM3W5QZ6QRGGV`).
<!-- written_at: 2026-09-06T20:30:00Z  source_event: task:01KXSHA3Q1QCHM8T68WS6MS1NC, comment:01M1W63EGNJXC9XMVWAF0ZXXMK (fixedInPlace: resume ledger froze provider failures), git:cc113ae, code:benchmarks/repo-qa/scripts/bench.py:253 -->

## 14. The competitor for a tool call is the agent's OWN native toolset, not a neighbouring MCP tool

Rule 12 proved the arms were non-disjoint; this is what the surviving native tools then do
to a SELECTION bench. E2.4 ran a 384-cell description grid built entirely around confusable
LCI neighbours and found no rewrite beat the shipped descriptions (A 0.791 weak / 0.711
strong; B 0.615/0.618; C 0.575/0.592; D 0.695/0.659). Every rewrite arm lost its ground to
native `glob`/`read` — strong-tier native-first calls rose 21% -> 32-34% — while neighbour
pressure stayed flat or fell. Three tools (`list_symbols`, `browse_file`, `inspect_symbol`)
sat at the floor under every arm because native `read` answers those prompts. The
confusable-neighbour axis the epic was designed around was not where the mass went. D5
reached the same conclusion from the other direction (native-first dominant in the discovery
sweep).

- **Any selection or lift bench must carry the agent's native tools as a first-class column
  before it measures neighbours.** Report native-first call share per arm alongside the
  selection rate; a description change that does not move it has not moved the thing that
  decides the call.
- **A description rewrite competes against `glob`/`read`, not against a sibling.** Write the
  variant to argue why the tool beats reading the file, and pre-register that as the
  hypothesis.
- **A live-smoke mis-selection seen once is not a defect to design an arm around.** E2.3's
  search-for-callers confusion did not reproduce once in 24 graded baseline runs, yet variant
  D existed to fix it. Pin a smoke finding with reps before it earns an arm.
<!-- written_at: 2026-09-06T23:55:00Z  source_event: task:01KXSHA3QKT5RDT4F08RA08H1N, comment:01M1WHMYBBF9XDKQ9KDRBBWMYW (lessons), comment:01M1WHW7X2KRSA1V6QJRNC7V2Y (verified.recommendations item 5), task:01KXQ2V3PW91QTTQ1NTZXH7PR6 (D5 native-first) -->

## 15. A pre-registered lift rule needs a non-degenerate-spread floor — a spread of exactly 0.0 means UNMEASURED, not CERTAIN

"Credit a lift when the gap exceeds the run-to-run spread" is the right shape and has a hole
at the floor: a baseline that scores 0 in every rep has spread exactly 0.0, so ANY non-zero
treatment result clears the bar. E2.4 hit it live — `browse_file`/strong/variant D was
credited a lift off one correct call against a 0/3 baseline. With 3 floor-pinned tasks x 2
models, 6 of the grid's cells were degenerate by construction. The same arithmetic bites the
ceiling (baseline correct in every rep).

- **Pre-register a minimum-successes (or minimum-n) gate alongside the spread rule**, so a
  baseline pinned at floor or ceiling yields `no_effect`, not a lift.
- **Denominators of 12-15 per cell are the practical ceiling for a 3-rep 16-task grid on the
  free fleet** (provider noise, rule 13). Almost every honest delta lands inside spread; a
  rule with a degenerate hole will manufacture the exceptions.
- **When a pre-registered rule misfires, report the misfire with its mechanism and
  pre-register the fix for the NEXT run.** E2.4 recorded the degenerate lift in the JSON,
  excluded it from the Markdown recommendations with the reason, and left the rule as
  registered — editing a decision rule after seeing the data is the failure this discipline
  exists to prevent.
<!-- written_at: 2026-09-06T23:55:00Z  source_event: task:01KXSHA3QKT5RDT4F08RA08H1N, comment:01M1WHMYBBF9XDKQ9KDRBBWMYW (lessons: floor-pinned-baseline hole), comment:01M1WHWNM6ECEJ2TP6X1QF5CHE (passPatterns + suggestions), follow-up:01M1WHXFH8C1HGJQT8FHWS0F16 -->
