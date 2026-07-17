# Symbol cohorts — the discovery sweep's sample

`cohorts.json` is the committed sample D3/D4 run and D5 correlates against.
It is generated, never edited by hand:

```
python3 scripts/select_symbol_cohorts.py \
  --corpus real_projects/go/pocketbase \
  --out discovery/cohorts/cohorts.json
```

Rerunning against the pinned corpus (`d438c6a`) with the committed seed
(`d2-pocketbase-cohorts-v1`) reproduces the file byte-for-byte.

## Why the sample is generated

The sweep asks whether a semantic index beats grep at finding things. *Which*
symbols it asks about decides the answer. A hand-picked list of "symbols that
feel hard" would encode the very intuition this epic exists to test, and any
LCI win it reported would be circular. So every axis is computed, and the
selector refuses to emit a sample that could only produce one answer.

## Independence

Both inputs are independent of the tool under benchmark:

| Metric | Source |
|---|---|
| `grep_hit_count` | real `grep -rnw` over the corpus |
| `true_reference_count`, `fan_in`, `impl_count`, test/production split | D1's gopls oracle (`gopls_oracle.py`, gopls v0.23.0) |

LCI is never imported, invoked, or consulted — a test asserts this. The tool
under test does not get to help choose its own exam.

## Axes

The primary axis is **collision noise** = `grep_hit_count / true_reference_count`.
It is the mechanical reason grep should struggle: `fileblob.Close` buries 7 real
references under 269 whole-word hits (38.4), while `getFileName` sits at 0.88 and
grep should win outright.

| Axis | Buckets |
|---|---|
| collision noise | `control` (≤2.0, unique) / `medium` / `high` (≥8.0) |
| fan-in | `low` (≤2) / `high` (≥8) |
| shadowing | `unique` (1 declaration) / `multi` |
| interface implementors | `none` (0) / `many` (≥2) |
| reference kind | `test_dominant` (≥50% in `_test.go`) / `production_dominant` |

Cohorts overlap by design: each symbol takes one bucket per axis, so D5 can
slice separation by any axis independently. Every symbol carries all its
metrics inline in `symbols[]` — that correlation is the product of the epic.

Thresholds are measured, not guessed. Observed on the pinned corpus:
`vacuum` 3/3 = 1.0, `wwwRedirect` 3/2 = 1.5, `Record.Set` 529/119 = 4.4,
`GeoPointField.Type` 351/29 = 12.1.

## Why N = 48

Breadth over depth, per the parent epic's method. Each cell costs a gopls
answer key now and a sweep run per tool later, so depth is expensive and buys
little: the epic's product is D5's correlation of separation against
difficulty, and a correlation needs *range* across the axes far more than it
needs many samples at one point.

48 draws 12 candidates from each of 4 grep-hit strata over 1887 candidates.
On the pinned corpus that populates all three noise buckets — 26 control /
16 medium / 6 high — with every secondary axis represented, in ~70s of gopls.

## Control cells are mandatory

26 of the 48 cells are controls: low-noise, uniquely-named symbols where grep
should do fine. `validate_cohorts` **fails** if the control or high-noise
cohort is empty. A sweep with no controls can only report that LCI wins, which
is a rigged experiment — the epic's honesty constraint depends on being able to
report a null result.

## Cost shape (and why it doesn't bias the sample)

gopls costs seconds per symbol; grep costs milliseconds. So grep hits stratify
the *candidate pool* and gopls only measures the seeded sample. The cheap proxy
decides who gets looked at; the measured ratio alone decides the cohort. A
candidate drawn from the noisiest grep stratum whose real noise turns out to be
~1 is classified `control` — the proxy shapes cost, never the conclusion.

## Population limits

Candidates are production `func` declarations (functions and methods). Excluded:

- **`_test.go` declarations** — the sweep measures how tools find production symbols.
- **type declarations** — gopls `call_hierarchy` refuses a type, so fan-in is
  undefined for them. Lifting this needs the D1 oracle to express "fan-in
  undefined" rather than error.
- **`init` / `main`** — compiler-invoked and never referenced from source, so
  their noise ratio measures a language artifact, not collision.

All three still count toward `def_count`: a type declaration collides with a
same-named method in grep's output even though it cannot be a cell itself.
