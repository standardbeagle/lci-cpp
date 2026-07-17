"""Select the discovery sweep's symbol cohorts by objective, measured axes.

The sweep asks whether a semantic index beats grep at finding things. Which
symbols it asks about decides the answer, so the sample must not be chosen by
intuition about which symbols "feel hard" -- that intuition IS the hypothesis
under test, and encoding it in the sample would make any result circular.

Every axis here is therefore computed, from two sources that are both
independent of the tool under benchmark:

  * grep_hit_count -- real grep(1) over the corpus. This is the noise a human
    with grep actually wades through.
  * true_reference_count, fan_in, impl_count, test/production split -- D1's
    gopls oracle (gopls_oracle.py). gopls is the Go language server; it has no
    relationship to LCI.

The primary axis is COLLISION NOISE = grep_hit_count / true_reference_count.
It is the *mechanical* reason grep should struggle: a method named Type with
351 whole-word grep hits and 29 real references buries the answer 12:1 in
lookalikes, while a uniquely-named symbol sits at ~1 and grep should win
outright. Symbols at ~1 are CONTROL cells and are mandatory -- a sweep that
cannot report "no difference here" is not an experiment.

Cost shape. gopls is expensive (seconds per symbol); grep is not (~10ms).
So selection is two-phase: enumerate + grep every candidate cheaply, then
oracle-enrich only a seeded stratified sample. The cheap grep count decides
which candidates get *looked at*; the measured noise ratio decides which
cohort they land in. A candidate drawn from the noisiest grep stratum whose
measured noise turns out to be ~1 is classified control, and that is the
point: the proxy shapes cost, never the conclusion.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gopls_oracle import (  # noqa: E402
    PINNED_GOPLS_VERSION,
    GoplsOracle,
    SymbolAnchor,
    corpus_commit,
    resolve_gopls_binary,
)

SCHEMA_VERSION = 1

# The committed seed. Changing it reshuffles the sample and invalidates every
# downstream comparison, so it is a constant in source, never a flag default.
COHORT_SEED = "d2-pocketbase-cohorts-v1"

# Default sample size. Rationale (breadth over depth, per the parent epic):
# each cell costs a gopls answer key now and a sweep run per tool later, so
# depth is expensive and buys little -- the product of this epic is D5's
# correlation of separation against difficulty, and a correlation needs range
# across the axes far more than it needs many samples at one point. 48 spreads
# 12 candidates across each of 4 grep-hit strata, which on the pinned corpus
# populates all three noise buckets with control cells to spare, while keeping
# the whole selection to ~10 minutes of gopls.
DEFAULT_POOL_SIZE = 48
STRATA = 4

# --- Axis thresholds --------------------------------------------------------
# Measured against the pinned corpus rather than guessed. Observed ratios:
# BaseApp.vacuum 3/3 = 1.0 (unique, control); apis.wwwRedirect 3/2 = 1.5
# (unique, control); Record.Set 529/119 = 4.4 (shadowed 6x, medium);
# GeoPointField.Type 351/29 = 12.1 (shadowed 14x, high).
CONTROL_MAX_NOISE = 2.0
HIGH_MIN_NOISE = 8.0

FAN_IN_LOW_MAX = 2
FAN_IN_HIGH_MIN = 8
IMPL_MANY_MIN = 2
TEST_DOMINANT_MIN_SHARE = 0.5

_FUNC_RE = re.compile(r"^func\s+(?:\((?P<recv>[^)]*)\)\s*)?(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\(")
_TYPE_RE = re.compile(r"^type\s+(?P<name>[A-Za-z_]\w*)\s")

_SKIP_DIRS = {"vendor", ".git", "testdata"}


class SelectionError(RuntimeError):
    """Raised instead of degrading. A bad sample is worse than no sample."""


class Candidate:
    __slots__ = ("name", "path", "line", "column", "kind")

    def __init__(self, name, path, line, column, kind):
        self.name = name
        self.path = path
        self.line = line
        self.column = column
        self.kind = kind

    def slug(self):
        flat = re.sub(r"[^A-Za-z0-9]+", "_", "%s_%s" % (self.path, self.name)).strip("_")
        return "%s_L%dC%d" % (flat, self.line, self.column)

    def __repr__(self):
        return "Candidate(%s at %s:%d:%d)" % (self.name, self.path, self.line, self.column)


def is_test_file(path):
    """Go's own rule. Matching "test" anywhere would misfile testdata/ code."""
    return os.path.basename(path).endswith("_test.go")


def _walk_go_files(corpus_root):
    """Every non-vendored .go file, in a deterministic order.

    os.walk's order follows the filesystem, which differs across machines and
    would make the "identical cohorts on rerun" guarantee a lie.
    """
    for dirpath, dirnames, filenames in os.walk(corpus_root):
        dirnames[:] = sorted(d for d in dirnames if d not in _SKIP_DIRS)
        for filename in sorted(filenames):
            if filename.endswith(".go"):
                full = os.path.join(dirpath, filename)
                yield full, os.path.relpath(full, corpus_root).replace(os.sep, "/")


def _declarations_in(full_path, rel_path):
    with open(full_path, encoding="utf8", errors="replace") as handle:
        for lineno, line in enumerate(handle, 1):
            for regex, kind in ((_FUNC_RE, "func"), (_TYPE_RE, "type")):
                match = regex.match(line)
                if not match:
                    continue
                name = match.group("name")
                # gopls resolves a cursor position, not a name, so the column
                # must land on the identifier itself. Search from the end of
                # the receiver group so `func (t *T) T()` anchors the method,
                # not the receiver type.
                start = match.end("recv") if match.groupdict().get("recv") else match.start("name")
                column = line.index(name, max(start - 1, 0)) + 1
                yield Candidate(name, rel_path, lineno, column, kind)
                break


def enumerate_declarations(corpus_root):
    """Candidate symbols: production declarations only, deterministically ordered.

    Test-file declarations are excluded as candidates -- the sweep measures how
    tools find production symbols -- but they still count toward shadowing (see
    declaration_counts).
    """
    found = []
    for full, rel in _walk_go_files(corpus_root):
        if is_test_file(rel):
            continue
        found.extend(_declarations_in(full, rel))
    return sorted(found, key=lambda c: c.slug())


def declaration_counts(corpus_root):
    """How many times each identifier is declared corpus-wide, tests included.

    Shadowing is a property of the name across the whole corpus: a name also
    declared in a _test.go is still shadowed, and still confuses a grep user.
    """
    counts = {}
    for full, rel in _walk_go_files(corpus_root):
        for candidate in _declarations_in(full, rel):
            counts[candidate.name] = counts.get(candidate.name, 0) + 1
    return counts


def grep_hit_count(name, corpus_root):
    """Whole-word grep hits for `name` across the corpus's .go files.

    Real grep(1), deliberately: this number is the sweep's grep baseline, so
    computing it any other way would measure something no grep user experiences.
    Counts matching LINES, which is what a grep user scrolls through.
    """
    proc = subprocess.run(
        ["grep", "-rnw", "--include=*.go", "-c", "--", name, "."],
        cwd=corpus_root,
        capture_output=True,
        text=True,
    )
    # grep exits 1 on "no matches", which is not an error here. Anything above
    # that is (unreadable tree, bad pattern) and must not read as zero noise.
    if proc.returncode not in (0, 1):
        raise SelectionError(
            "grep failed for %r in %s: %s" % (name, corpus_root, proc.stderr.strip())
        )
    total = 0
    for line in proc.stdout.strip().split("\n"):
        if not line or ":" not in line:
            continue
        path, _, count = line.rpartition(":")
        if any(part in _SKIP_DIRS for part in path.split("/")):
            continue
        total += int(count)
    return total


def noise_ratio(grep_hits, true_references):
    """grep_hit_count / true_reference_count."""
    if true_references <= 0:
        # The oracle raises rather than return an empty reference set, so zero
        # here means something upstream degraded. Clamping the denominator to 1
        # would turn that break into a plausible-looking, maximally-noisy cell.
        raise SelectionError(
            "true_reference_count is %r; refusing to synthesize a noise ratio "
            "from a broken oracle answer" % (true_references,)
        )
    return grep_hits / float(true_references)


def classify_noise(ratio, def_count):
    """Bucket a symbol on the primary axis.

    A shadowed identifier is never a control however quiet this particular
    declaration is: control means "grep should trivially win", and a name with
    several declarations does not offer grep a clean shot at any of them.
    """
    if ratio <= CONTROL_MAX_NOISE and def_count == 1:
        return "control"
    if ratio >= HIGH_MIN_NOISE:
        return "high"
    return "medium"


def build_metrics_row(candidate, grep_hits, def_count, key):
    """One symbol plus every metric D5 correlates separation against."""
    counts = key["reference_counts"]
    total = counts["total"]
    ratio = noise_ratio(grep_hits, total)
    fan_in = len(key["callers"])
    impl_count = len(key["implementations"])
    return {
        "name": candidate.name,
        "path": candidate.path,
        "line": candidate.line,
        "column": candidate.column,
        "kind": candidate.kind,
        "slug": candidate.slug(),
        "grep_hit_count": grep_hits,
        "true_reference_count": total,
        "test_reference_count": counts["test"],
        "production_reference_count": counts["production"],
        "noise_ratio": round(ratio, 4),
        "noise_cohort": classify_noise(ratio, def_count),
        "fan_in": fan_in,
        "def_count": def_count,
        "impl_count": impl_count,
        "test_reference_share": round(counts["test"] / float(total), 4),
    }


def _stable_rank(seed, slug):
    """Seeded, machine-independent ordering key.

    hash() is salted per process, so it would break the reproducibility this
    module promises. sha256 of seed|slug does not.
    """
    return hashlib.sha256(("%s|%s" % (seed, slug)).encode("utf8")).hexdigest()


def stratified_pool(candidates, hits_by_name, seed, size, strata=STRATA):
    """Draw a seeded sample spread across grep-hit strata.

    Stratifying on grep hits (cheap) rather than on noise (expensive) is a cost
    decision only: it guarantees the pool spans quiet and loud names without
    paying gopls for all 1600+ candidates. The cohort a symbol ends up in is
    decided later, by its measured noise ratio.
    """
    ordered = sorted(candidates, key=lambda c: c.slug())
    if not ordered:
        return []
    by_hits = sorted(ordered, key=lambda c: (hits_by_name[c.name], c.slug()))
    per_stratum = max(1, len(by_hits) // strata)
    picked = []
    quota = _quotas(size, strata)
    for index in range(strata):
        start = index * per_stratum
        end = len(by_hits) if index == strata - 1 else (index + 1) * per_stratum
        bucket = sorted(by_hits[start:end], key=lambda c: _stable_rank(seed, c.slug()))
        picked.extend(bucket[: quota[index]])
    return sorted(picked, key=lambda c: c.slug())


def _quotas(size, strata):
    """Split `size` across strata, remainder to the earliest strata."""
    base, extra = divmod(size, strata)
    return [base + (1 if i < extra else 0) for i in range(strata)]


def assign_cohorts(rows):
    """Group measured symbols onto every declared axis.

    Cohorts intentionally overlap: a symbol is a member of one bucket per axis,
    so D5 can slice separation by any axis independently.
    """
    cohorts = {
        "noise_control": [],
        "noise_medium": [],
        "noise_high": [],
        "fan_in_low": [],
        "fan_in_high": [],
        "shadowing_unique": [],
        "shadowing_multi": [],
        "impl_none": [],
        "impl_many": [],
        "refs_test_dominant": [],
        "refs_production_dominant": [],
    }
    for row in sorted(rows, key=lambda r: r["slug"]):
        cohorts["noise_" + row["noise_cohort"]].append(row)
        if row["fan_in"] <= FAN_IN_LOW_MAX:
            cohorts["fan_in_low"].append(row)
        elif row["fan_in"] >= FAN_IN_HIGH_MIN:
            cohorts["fan_in_high"].append(row)
        cohorts["shadowing_unique" if row["def_count"] == 1 else "shadowing_multi"].append(row)
        if row["impl_count"] == 0:
            cohorts["impl_none"].append(row)
        elif row["impl_count"] >= IMPL_MANY_MIN:
            cohorts["impl_many"].append(row)
        if row["test_reference_share"] >= TEST_DOMINANT_MIN_SHARE:
            cohorts["refs_test_dominant"].append(row)
        else:
            cohorts["refs_production_dominant"].append(row)
    return cohorts


def validate_cohorts(cohorts):
    """Refuse a sample that cannot produce an honest result."""
    if not cohorts["noise_control"]:
        raise SelectionError(
            "no control cells: every selected symbol is noisy, so the sweep "
            "could only ever report that LCI wins. A benchmark that cannot "
            "report a null result is rigged; widen the pool instead."
        )
    if not cohorts["noise_high"]:
        raise SelectionError(
            "no high-noise cells: the sweep would have no case where grep is "
            "mechanically disadvantaged, so it could not detect a difference "
            "even if one exists."
        )
    return cohorts


def resolve_gopls_binary_or_none():
    try:
        return resolve_gopls_binary()
    except Exception:
        return None


def build_oracle(corpus_root, cache_dir=None):
    if cache_dir is None:
        cache_dir = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "discovery",
            "oracle",
            "cache",
        )
    return GoplsOracle.for_corpus(corpus_root=corpus_root, cache_dir=cache_dir)


def serialize(doc):
    return json.dumps(doc, indent=2, sort_keys=True) + "\n"


def select(corpus_root, oracle, seed=COHORT_SEED, pool_size=DEFAULT_POOL_SIZE,
           validate=True, progress=None):
    """Run the whole selection and return the committed document's contents."""
    corpus_root = os.path.abspath(corpus_root)
    candidates = enumerate_declarations(corpus_root)
    if not candidates:
        raise SelectionError("no Go declarations under %s" % corpus_root)

    def_counts = declaration_counts(corpus_root)
    hits_by_name = {}
    for candidate in candidates:
        if candidate.name not in hits_by_name:
            hits_by_name[candidate.name] = grep_hit_count(candidate.name, corpus_root)

    pool = stratified_pool(candidates, hits_by_name, seed=seed, size=pool_size)

    rows = []
    skipped = []
    for index, candidate in enumerate(pool, 1):
        if progress:
            progress("[%d/%d] %s %s:%d" % (index, len(pool), candidate.name,
                                           candidate.path, candidate.line))
        anchor = SymbolAnchor(candidate.name, candidate.path, candidate.line,
                              candidate.column)
        try:
            key = oracle.answer_key(anchor, max_depth=1)
        except Exception as exc:
            # A symbol gopls cannot resolve is recorded and dropped, never
            # guessed at. Dropping silently would let the sample quietly shrink
            # toward whatever gopls happens to like.
            skipped.append({"slug": candidate.slug(), "reason": str(exc)[:200]})
            continue
        rows.append(
            build_metrics_row(
                candidate,
                grep_hits=hits_by_name[candidate.name],
                def_count=def_counts[candidate.name],
                key=key,
            )
        )

    cohorts = assign_cohorts(rows)
    if validate:
        validate_cohorts(cohorts)
    return {
        "schema_version": SCHEMA_VERSION,
        "seed": seed,
        "corpus_commit": corpus_commit(corpus_root),
        "gopls_version": PINNED_GOPLS_VERSION,
        "pool_size": pool_size,
        "thresholds": {
            "control_max_noise": CONTROL_MAX_NOISE,
            "high_min_noise": HIGH_MIN_NOISE,
            "fan_in_low_max": FAN_IN_LOW_MAX,
            "fan_in_high_min": FAN_IN_HIGH_MIN,
            "impl_many_min": IMPL_MANY_MIN,
            "test_dominant_min_share": TEST_DOMINANT_MIN_SHARE,
        },
        "candidate_count": len(candidates),
        "symbols": sorted(rows, key=lambda r: r["slug"]),
        "cohorts": {name: [r["slug"] for r in members] for name, members in
                    sorted(cohorts.items())},
        "skipped": sorted(skipped, key=lambda s: s["slug"]),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--pool-size", type=int, default=DEFAULT_POOL_SIZE)
    parser.add_argument("--out", default=None)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    progress = None if args.quiet else (lambda m: sys.stderr.write(m + "\n"))
    try:
        doc = select(
            corpus_root=args.corpus,
            oracle=build_oracle(args.corpus),
            pool_size=args.pool_size,
            progress=progress,
        )
    except SelectionError as exc:
        sys.stderr.write("cohort-selection FAILED: %s\n" % exc)
        return 2
    payload = serialize(doc)
    if args.out:
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        with open(args.out, "w", encoding="utf8") as handle:
            handle.write(payload)
    else:
        sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
