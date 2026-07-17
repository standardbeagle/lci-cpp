#!/usr/bin/env python3
"""Build ground-truth answer keys for a pinned Go corpus, using gopls.

A discovery benchmark asks a tool "where is X defined / referenced / called
from / implemented". Grading those answers needs an authority. This module is
that authority, and gopls -- the Go team's own language server, driving the
same type checker the compiler uses -- is the only thing it consults.

Independence is the point
-------------------------
This oracle never imports, invokes, or consults the tool being benchmarked. If
gopls and that tool disagree, the disagreement IS the measurement; it is never
reconciled toward the tool. Nothing here may grow a dependency on the system
under test, however convenient.

Fail loud, never empty
----------------------
The dangerous failure is quiet: if gopls fails to load a package but still
exits 0, an empty answer key grades every candidate answer as a miss, and the
benchmark reports a spectacular regression that never happened. So every
degenerate outcome -- missing binary, version drift, non-zero exit, an empty
reference set for a symbol that demonstrably resolves -- raises with a distinct
reason code. No fallbacks, no dummy keys, no partial cache writes.

Determinism
-----------
Same corpus commit + same gopls version => byte-identical key. Every list is
sorted on an explicit total key rather than inheriting gopls' emission order,
and every path is stored relative to the corpus root, because absolute paths
baked into goldens break the moment the checkout moves.

Caching
-------
Cold gopls over ~150k LOC costs seconds per query, and a sweep wants many
symbols. Keys are cached on disk under (corpus commit, gopls version); either
one changing invalidates, both by cache path and by a provenance check inside
the payload.

Usage:
    gopls_oracle.py key --symbol UcFirst --file tools/inflector/inflector.go \
        --line 13 --column 6
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

# The exact gopls the goldens are pinned to. gopls changes its analysis and its
# output format between releases, so a different version is a different oracle
# and must never silently reuse these answers.
PINNED_GOPLS_VERSION = "golang.org/x/tools/gopls v0.23.0"

SCHEMA_VERSION = 1

REASON_GOPLS_MISSING = "GOPLS_MISSING"
REASON_VERSION_MISMATCH = "GOPLS_VERSION_MISMATCH"
REASON_EXEC_FAILED = "GOPLS_EXEC_FAILED"
REASON_EMPTY_RESULT = "EMPTY_RESULT_FOR_EXISTING_SYMBOL"
REASON_CACHE_PROVENANCE = "CACHE_PROVENANCE_MISMATCH"
REASON_CORPUS_MISSING = "CORPUS_MISSING"


class GoplsError(RuntimeError):
    """Any refusal to produce a key. Carries a machine-readable reason code."""

    def __init__(self, reason, message):
        super().__init__("[%s] %s" % (reason, message))
        self.reason = reason


class SymbolAnchor:
    """A 1-indexed cursor position identifying a symbol, as gopls locates them."""

    __slots__ = ("name", "path", "line", "column")

    def __init__(self, name, path, line, column):
        self.name = name
        self.path = path
        self.line = line
        self.column = column

    @property
    def position(self):
        return "%s:%d:%d" % (self.path, self.line, self.column)

    def slug(self):
        flat = re.sub(r"[^A-Za-z0-9]+", "_", "%s_%s" % (self.path, self.name)).strip("_")
        return "%s_L%dC%d" % (flat, self.line, self.column)

    def __repr__(self):
        return "SymbolAnchor(%s at %s)" % (self.name, self.position)


# --- gopls output grammars, transcribed from real v0.23.0 emissions ----------
#
# references / implementation:
#   /abs/path/file.go:13:6-13
_LOCATION_RE = re.compile(r"^(?P<path>.+):(?P<line>\d+):(?P<col>\d+)-(?P<endcol>\d+)$")

# call_hierarchy, caller lines only:
#   caller[0]: ranges 1240:21-28 in /abs/a.go from/to function f in /abs/b.go:1194:6-21
# The interleaved `identifier:` and `callee[N]:` lines are deliberately not
# matched here: a callee is an OUTGOING edge, and reading one as a caller would
# silently invert the call graph.
_CALLER_RE = re.compile(
    r"^caller\[(?P<idx>\d+)\]: ranges "
    r"(?P<site_line>\d+):(?P<site_col>\d+)-(?P<site_endcol>\d+) in "
    r"(?P<site_path>.+?) from/to function (?P<name>\S+) in "
    r"(?P<def_path>.+):(?P<def_line>\d+):(?P<def_col>\d+)-(?P<def_endcol>\d+)$"
)

# gopls refuses `implementation` on a non-method with a non-zero exit. That is a
# legitimately empty answer, not a broken oracle, and is the ONLY error text
# allowed to degrade to []. Everything else propagates.
_BENIGN_IMPLEMENTATION_RE = re.compile(
    r"is a function, not a method|is not a type|no objects implement"
)


def is_test_path(path):
    """Go's own rule: a test file is one whose name ends in _test.go.

    Matching on "test" appearing anywhere in the path would misfile production
    code under directories like testdata/ or contest/, skewing every
    test-vs-production ratio the benchmark reports.
    """
    return os.path.basename(path).endswith("_test.go")


def resolve_gopls_binary(candidate=None):
    """Locate gopls, or refuse loudly. Never degrades to a stub."""
    if candidate is None:
        candidate = os.environ.get("GOPLS_BIN") or shutil.which("gopls")
    if candidate is None:
        default = os.path.expanduser("~/go/bin/gopls")
        candidate = default if os.path.exists(default) else None
    if not candidate or not os.path.exists(candidate):
        raise GoplsError(
            REASON_GOPLS_MISSING,
            "gopls binary not found (looked at %r, $GOPLS_BIN, $PATH, ~/go/bin). "
            "The oracle refuses to guess ground truth without it." % (candidate,),
        )
    if not os.access(candidate, os.X_OK):
        raise GoplsError(REASON_GOPLS_MISSING, "gopls at %r is not executable" % candidate)
    return candidate


def subprocess_runner(binary):
    """Runner that shells out to the real gopls. No network: gopls reads the
    module cache offline, and the corpus is never written to."""

    def run(args, cwd):
        env = dict(os.environ)
        env["GOFLAGS"] = "-mod=mod"
        env["GOPROXY"] = "off"  # hermetic: a run must never reach the network
        proc = subprocess.run(
            [binary, *args],
            cwd=cwd,
            capture_output=True,
            text=True,
            env=env,
        )
        if proc.returncode != 0:
            raise GoplsError(
                REASON_EXEC_FAILED,
                "gopls %s exited %d: %s"
                % (" ".join(args), proc.returncode, proc.stderr.strip()),
            )
        return proc.stdout.strip()

    return run


def corpus_commit(corpus_root):
    if not os.path.isdir(corpus_root):
        raise GoplsError(REASON_CORPUS_MISSING, "corpus root %r absent" % corpus_root)
    proc = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=corpus_root,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise GoplsError(
            REASON_CORPUS_MISSING,
            "cannot read corpus commit at %r: %s" % (corpus_root, proc.stderr.strip()),
        )
    return proc.stdout.strip()


def serialize(key):
    """The one canonical encoding. Byte-identical for equal keys."""
    return json.dumps(key, indent=2, sort_keys=True) + "\n"


class GoplsOracle:
    def __init__(self, corpus_root, corpus_commit, cache_dir, expected_version, runner):
        self.corpus_root = os.path.abspath(corpus_root)
        self.corpus_commit = corpus_commit
        self.cache_dir = cache_dir
        self.expected_version = expected_version
        self.runner = runner

    @classmethod
    def for_corpus(cls, corpus_root, cache_dir, expected_version=PINNED_GOPLS_VERSION):
        """Build against the real binary, verifying provenance up front."""
        binary = resolve_gopls_binary()
        oracle = cls(
            corpus_root=corpus_root,
            corpus_commit=corpus_commit(corpus_root),
            cache_dir=cache_dir,
            expected_version=expected_version,
            runner=subprocess_runner(binary),
        )
        oracle.verify_version()
        return oracle

    # --- provenance ------------------------------------------------------
    def verify_version(self):
        actual = self.runner(["version"], cwd=self.corpus_root).strip()
        first = actual.split("\n")[0].strip()
        if first != self.expected_version:
            raise GoplsError(
                REASON_VERSION_MISMATCH,
                "gopls version drift: expected %r, found %r. Answer keys are only "
                "valid for the pinned version; refusing to mix."
                % (self.expected_version, first),
            )
        return first

    # --- path handling ---------------------------------------------------
    def _relativize(self, path):
        """Corpus-relative path, plus a flag for anything outside the corpus.

        Absolute paths must never reach a key: a golden carrying this machine's
        checkout path fails on any other checkout. Out-of-corpus hits (stdlib,
        module cache) are marked rather than dropped -- dropping would be the
        silent data loss this module exists to prevent.
        """
        path = path.strip()
        if path.startswith("file://"):
            path = path[len("file://") :]
        path = os.path.normpath(path)
        root = self.corpus_root + os.sep
        if path.startswith(root):
            return path[len(root) :].replace(os.sep, "/"), False
        # Normalize machine-specific module-cache / GOROOT prefixes away.
        for marker in ("/pkg/mod/", "/src/"):
            idx = path.find(marker)
            if idx != -1:
                return "external://" + path[idx + len(marker) :].replace(os.sep, "/"), True
        return "external://" + os.path.basename(path), True

    # --- gopls queries ---------------------------------------------------
    def _definition(self, anchor):
        raw = self.runner(["definition", "-json", anchor.position], cwd=self.corpus_root)
        if not raw.strip():
            raise GoplsError(
                REASON_EMPTY_RESULT,
                "gopls returned no definition for %r while exiting 0" % (anchor,),
            )
        payload = json.loads(raw)
        span = payload["span"]
        path, external = self._relativize(span["uri"])
        return {
            "path": path,
            "external": external,
            "line": span["start"]["line"],
            "column": span["start"]["column"],
            "end_line": span["end"]["line"],
            "end_column": span["end"]["column"],
            "description": payload.get("description", ""),
            "kind": "test" if is_test_path(path) else "production",
        }

    def _parse_locations(self, raw):
        out = []
        for line in raw.strip().split("\n"):
            line = line.strip()
            if not line:
                continue
            m = _LOCATION_RE.match(line)
            if not m:
                # An unparsed line means gopls' output grammar moved. Guessing
                # past it would silently shrink the answer key.
                raise GoplsError(
                    REASON_EXEC_FAILED,
                    "unrecognised gopls location line: %r" % line,
                )
            path, external = self._relativize(m.group("path"))
            out.append(
                {
                    "path": path,
                    "external": external,
                    "line": int(m.group("line")),
                    "column": int(m.group("col")),
                    "end_column": int(m.group("endcol")),
                    "kind": "test" if is_test_path(path) else "production",
                }
            )
        return out

    def _references(self, anchor):
        raw = self.runner(["references", "-d", anchor.position], cwd=self.corpus_root)
        refs = self._parse_locations(raw)
        if not refs:
            # -d includes the declaration itself, so a resolvable symbol always
            # has >=1. Zero means the package silently failed to load.
            raise GoplsError(
                REASON_EMPTY_RESULT,
                "empty reference set for %r, whose definition resolved. gopls "
                "likely failed to load the package while exiting 0; refusing to "
                "emit a key that would grade every answer as a miss." % (anchor,),
            )
        return sorted(refs, key=lambda r: (r["path"], r["line"], r["column"]))

    def _implementations(self, anchor):
        try:
            raw = self.runner(["implementation", anchor.position], cwd=self.corpus_root)
        except GoplsError as exc:
            if _BENIGN_IMPLEMENTATION_RE.search(str(exc)):
                # Not an interface/method: genuinely no implementations.
                return []
            raise
        impls = self._parse_locations(raw)
        return sorted(impls, key=lambda r: (r["path"], r["line"], r["column"]))

    def _call_hierarchy_callers(self, path, line, column):
        try:
            raw = self.runner(
                ["call_hierarchy", "%s:%d:%d" % (path, line, column)],
                cwd=self.corpus_root,
            )
        except GoplsError as exc:
            if "identifier not found" in str(exc):
                return []
            raise
        callers = []
        for text in raw.strip().split("\n"):
            m = _CALLER_RE.match(text.strip())
            if not m:
                continue  # identifier:/callee[N]: lines are not incoming edges
            def_path, def_external = self._relativize(m.group("def_path"))
            site_path, _ = self._relativize(m.group("site_path"))
            callers.append(
                {
                    "name": m.group("name"),
                    "path": def_path,
                    "external": def_external,
                    "line": int(m.group("def_line")),
                    "column": int(m.group("def_col")),
                    "kind": "test" if is_test_path(def_path) else "production",
                    "call_site": {
                        "path": site_path,
                        "line": int(m.group("site_line")),
                        "column": int(m.group("site_col")),
                        "kind": "test" if is_test_path(site_path) else "production",
                    },
                }
            )
        return callers

    def _transitive_callers(self, anchor, max_depth):
        """Breadth-first over incoming edges.

        Go call graphs are cyclic (mutual and self recursion are routine), so
        the visited set is load-bearing, not defensive: without it this walk
        does not terminate.
        """
        root = (anchor.path, anchor.line, anchor.column)
        visited = {root}
        frontier = [root]
        collected = {}
        for depth in range(1, max_depth + 1):
            next_frontier = []
            for path, line, column in frontier:
                for caller in self._call_hierarchy_callers(path, line, column):
                    ident = (caller["path"], caller["line"], caller["column"])
                    if ident in visited:
                        continue
                    visited.add(ident)
                    entry = dict(caller)
                    entry["depth"] = depth
                    collected[ident] = entry
                    if not caller["external"]:
                        next_frontier.append(ident)
            frontier = next_frontier
            if not frontier:
                break
        return sorted(
            collected.values(),
            key=lambda c: (c["depth"], c["path"], c["line"], c["name"]),
        )

    # --- caching ---------------------------------------------------------
    def _version_slug(self):
        return re.sub(r"[^A-Za-z0-9._-]+", "_", self.expected_version)

    def cache_path(self, anchor, max_depth):
        return os.path.join(
            self.cache_dir,
            self.corpus_commit,
            self._version_slug(),
            "%s__d%d.json" % (anchor.slug(), max_depth),
        )

    def _load_cached(self, anchor, max_depth):
        path = self.cache_path(anchor, max_depth)
        if not os.path.exists(path):
            return None
        with open(path) as fh:
            payload = json.load(fh)
        # Path already encodes provenance; re-check the payload so a stale or
        # hand-edited file is refused rather than quietly trusted.
        if (
            payload.get("corpus_commit") != self.corpus_commit
            or payload.get("gopls_version") != self.expected_version
            or payload.get("schema_version") != SCHEMA_VERSION
        ):
            raise GoplsError(
                REASON_CACHE_PROVENANCE,
                "cached key at %s does not match its own provenance "
                "(corpus %s / gopls %s); refusing to serve it."
                % (path, self.corpus_commit, self.expected_version),
            )
        return payload

    def _store(self, anchor, max_depth, key):
        path = self.cache_path(anchor, max_depth)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # Write-then-rename: a crash mid-write must not leave a truncated key
        # that later reads as a legitimate (and wrong) answer.
        tmp = path + ".tmp"
        with open(tmp, "w") as fh:
            fh.write(serialize(key))
        os.replace(tmp, path)

    # --- public API ------------------------------------------------------
    def answer_key(self, anchor, max_depth=2):
        """The ground-truth key for one symbol. Raises rather than return empty.

        Nothing is cached unless the whole key was built successfully, so a
        failed run can never poison later runs with a partial answer.
        """
        cached = self._load_cached(anchor, max_depth)
        if cached is not None:
            return cached

        definition = self._definition(anchor)
        references = self._references(anchor)
        callers = self._transitive_callers(anchor, max_depth)
        implementations = self._implementations(anchor)

        key = {
            "schema_version": SCHEMA_VERSION,
            "corpus_commit": self.corpus_commit,
            "gopls_version": self.expected_version,
            "symbol": {
                "name": anchor.name,
                "path": anchor.path,
                "line": anchor.line,
                "column": anchor.column,
            },
            "max_depth": max_depth,
            "definition": definition,
            "references": references,
            "callers": callers,
            "implementations": implementations,
            "reference_counts": {
                "total": len(references),
                "test": sum(1 for r in references if r["kind"] == "test"),
                "production": sum(1 for r in references if r["kind"] == "production"),
            },
        }
        self._store(anchor, max_depth, key)
        return key


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    key_cmd = sub.add_parser("key", help="emit the answer key for one symbol")
    key_cmd.add_argument("--corpus", required=True)
    key_cmd.add_argument("--symbol", required=True)
    key_cmd.add_argument("--file", required=True)
    key_cmd.add_argument("--line", type=int, required=True)
    key_cmd.add_argument("--column", type=int, required=True)
    key_cmd.add_argument("--depth", type=int, default=2)
    key_cmd.add_argument(
        "--cache-dir",
        default=os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "discovery",
            "oracle",
            "cache",
        ),
    )

    args = parser.parse_args(argv)
    try:
        oracle = GoplsOracle.for_corpus(
            corpus_root=args.corpus, cache_dir=args.cache_dir
        )
        anchor = SymbolAnchor(
            name=args.symbol, path=args.file, line=args.line, column=args.column
        )
        sys.stdout.write(serialize(oracle.answer_key(anchor, max_depth=args.depth)))
    except GoplsError as exc:
        # Loud and non-zero. A benchmark must never mistake a broken oracle for
        # a tool that found nothing.
        sys.stderr.write("gopls-oracle FAILED: %s\n" % exc)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
