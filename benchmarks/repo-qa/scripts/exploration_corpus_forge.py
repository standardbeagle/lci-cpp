#!/usr/bin/env python3
"""Forge deterministic, mutated exploration corpora from pinned source snapshots.

An exploration benchmark asks a tool to *find* things. If the tool has memorised
the upstream repo, the benchmark measures recall, not exploration. So we mutate
the tree: rename directories, shuffle module paths, and inject decoy modules,
while keeping the corpus buildable.

Three invariants make the result usable as a benchmark:

  * Deterministic. Same source commit + forge version + seed => byte-identical
    manifest and identical tree hash. A corpus that drifts between runs cannot
    carry an oracle.
  * Translatable. Every mutation is recorded, so an oracle anchor expressed
    against the original tree can be translated to the mutated tree.
  * Validated. Mutation is only legitimate if the corpus still builds/imports.
    The configured validation is the oracle, and a corpus that fails it is
    never marked ready.

The source checkout is read-only: the pinned commit is exported with
`git archive`, so nothing is ever written back into the source repo.

Usage:
    exploration_corpus_forge.py forge --corpus scikit-learn --seed 7
    exploration_corpus_forge.py translate --manifest M --path sklearn/base.py
    exploration_corpus_forge.py check-imports --language python
"""

import argparse
import ast
import hashlib
import io
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

FORGE_VERSION = "1"
MANIFEST_SCHEMA = "exploration_corpus_manifest_v1"

BENCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPORA_PATH = os.path.join(BENCH_ROOT, "exploration", "corpora.json")
# .work/ is gitignored: generated corpora are never vendored into git.
DEFAULT_OUT_ROOT = os.path.join(BENCH_ROOT, ".work", "exploration")

_ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789"
_TS_EXTENSIONS = (".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs", ".json", ".d.ts")


class ForgeError(Exception):
    """The forge refuses to produce a corpus."""


class ValidationFailed(ForgeError):
    """The mutated corpus did not pass its configured validation."""

    def __init__(self, message, manifest):
        super().__init__(message)
        self.manifest = manifest


# ---------------------------------------------------------------------------
# source guards
# ---------------------------------------------------------------------------


def _git(repo, *args):
    proc = subprocess.run(
        ["git", "-C", repo, *args], capture_output=True, text=True
    )
    if proc.returncode != 0:
        raise ForgeError(
            f"git {' '.join(args)} failed in {repo}: {proc.stderr.strip()}"
        )
    return proc.stdout


def verify_source(spec, allow_dirty, allow_head_mismatch):
    """Refuse to forge from an ambiguous source. Returns the overrides used.

    A dirty checkout or a HEAD that has moved off the pin means the operator
    thinks they are forging commit X while the working tree says otherwise.
    That silently poisons every anchor derived from the corpus, so it is a
    hard refusal unless explicitly overridden.
    """
    source = spec["source_path"]
    if not os.path.isdir(os.path.join(source, ".git")):
        raise ForgeError(f"source is not a git checkout: {source}")

    overrides = []

    status = _git(source, "status", "--porcelain").strip()
    if status:
        if not allow_dirty:
            raise ForgeError(
                f"refusing dirty source checkout {source}: "
                f"{len(status.splitlines())} uncommitted/untracked path(s). "
                f"Pass --allow-dirty to override."
            )
        overrides.append("allow_dirty")

    pinned = spec["pinned_commit"]
    head = _git(source, "rev-parse", "HEAD").strip()
    if head != pinned:
        if not allow_head_mismatch:
            raise ForgeError(
                f"source HEAD {head} != pinned commit {pinned} in {source}. "
                f"Pass --allow-head-mismatch to override."
            )
        overrides.append("allow_head_mismatch")

    _git(source, "cat-file", "-e", f"{pinned}^{{commit}}")
    return overrides


def materialize(spec, out_dir):
    """Export the pinned commit into out_dir/tree. Never touches the source."""
    tree = os.path.join(out_dir, "tree")
    if os.path.exists(tree):
        shutil.rmtree(tree)
    os.makedirs(tree)

    archive = subprocess.run(
        [
            "git",
            "-C",
            spec["source_path"],
            "archive",
            "--format=tar",
            spec["pinned_commit"],
        ],
        capture_output=True,
    )
    if archive.returncode != 0:
        raise ForgeError(
            f"git archive {spec['pinned_commit']} failed: "
            f"{archive.stderr.decode(errors='replace').strip()}"
        )
    with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
        tar.extractall(tree, filter="data")
    return tree


# ---------------------------------------------------------------------------
# tree helpers
# ---------------------------------------------------------------------------


def list_files(tree):
    found = []
    for root, dirs, names in os.walk(tree):
        dirs.sort()
        for name in sorted(names):
            found.append(os.path.relpath(os.path.join(root, name), tree))
    return sorted(found)


def list_dirs(tree):
    found = []
    for root, dirs, _names in os.walk(tree):
        dirs.sort()
        for name in dirs:
            rel = os.path.relpath(os.path.join(root, name), tree)
            if not any(part.startswith(".") for part in rel.split(os.sep)):
                found.append(rel)
    return sorted(found)


def tree_hash(tree):
    """Content hash over (mode, blob, path) for every file, in sorted order."""
    digest = hashlib.sha256()
    for rel in list_files(tree):
        path = os.path.join(tree, rel)
        mode = "100755" if os.access(path, os.X_OK) else "100644"
        with open(path, "rb") as handle:
            blob = hashlib.sha256(handle.read()).hexdigest()
        digest.update(f"{mode} {blob} {rel}\n".encode())
    return digest.hexdigest()


def _read_text(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return handle.read()
    except (UnicodeDecodeError, ValueError):
        return None  # binary: never rewritten


def _write_text(path, body):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(body)


def _rng(spec, seed):
    key = "\0".join([FORGE_VERSION, spec["id"], spec["pinned_commit"], str(seed)])
    return random.Random(int.from_bytes(hashlib.sha256(key.encode()).digest(), "big"))


def _token(rng, size=6):
    return "".join(rng.choice(_ALPHABET) for _ in range(size))


def _strip_ext(rel, spec):
    for ext in spec["source_extensions"]:
        if rel.endswith(ext):
            return rel[: -len(ext)]
    return rel


# ---------------------------------------------------------------------------
# reference rewriting
#
# A path move is only build-preserving if every reference to the old path moves
# with it. References come in four shapes, and we handle all four:
#   slashed  "pkg/mod"          -- import paths, build files, docs
#   dotted   "pkg.mod"          -- python/java-style module paths
#   python relative imports     -- "from ..pkg import x"
#   TS/JS relative specifiers   -- "from './pkg/mod'"
# ---------------------------------------------------------------------------


def _absolute_ref_patterns(spec, old_mod, new_mod):
    patterns = []
    forms = spec["reference_forms"]
    if "slashed" in forms:
        # Import paths embed the repo-relative path as a suffix
        # ("github.com/org/repo/tools/x"), so a leading "/" must still match.
        patterns.append(
            (re.compile(r"(?<![\w\-])" + re.escape(old_mod) + r"(?![\w])"), new_mod)
        )
    if "dotted" in forms:
        old_dotted, new_dotted = old_mod.replace("/", "."), new_mod.replace("/", ".")
        patterns.append(
            (
                re.compile(r"(?<![\w.])" + re.escape(old_dotted) + r"(?![\w])"),
                new_dotted,
            )
        )
    return patterns


def _rewrite_absolute_refs(tree, spec, old_mod, new_mod):
    count = 0
    patterns = _absolute_ref_patterns(spec, old_mod, new_mod)
    for rel in list_files(tree):
        path = os.path.join(tree, rel)
        body = _read_text(path)
        if body is None:
            continue
        updated = body
        for pattern, replacement in patterns:
            updated, hits = pattern.subn(replacement, updated)
            count += hits
        if updated != body:
            _write_text(path, updated)
    return count


_PY_FROM_DOTS = re.compile(
    r"^(?P<head>\s*from\s+)(?P<dots>\.+)(?P<module>[\w.]*)(?P<tail>\s+import\s+)(?P<names>.*)$"
)


def _rewrite_python_relative_imports(tree, old_mod, new_mod):
    """Repoint `from ..pkg import x` style imports at a moved path.

    Files inside the moved subtree already sit at the new path, so their own
    relative imports resolve unchanged and are correctly left alone: the
    resolved target only matches the old path for files *outside* the move.
    """
    count = 0
    for rel in list_files(tree):
        if not rel.endswith(".py"):
            continue
        path = os.path.join(tree, rel)
        body = _read_text(path)
        if body is None:
            continue

        lines = body.split("\n")
        changed = False
        for index, line in enumerate(lines):
            match = _PY_FROM_DOTS.match(line)
            if not match:
                continue

            level = len(match.group("dots"))
            pkg = os.path.dirname(rel).split(os.sep) if os.path.dirname(rel) else []
            if level - 1 > len(pkg):
                continue
            base = pkg[: len(pkg) - (level - 1)]
            module = match.group("module")

            if module:
                target = "/".join(base + module.split("."))
                if target == old_mod or target.startswith(old_mod + "/"):
                    new_target = new_mod + target[len(old_mod) :]
                    new_module = ".".join(new_target.split("/")[len(base) :])
                    lines[index] = (
                        f"{match.group('head')}{match.group('dots')}{new_module}"
                        f"{match.group('tail')}{match.group('names')}"
                    )
                    changed = True
                    count += 1
                continue

            # `from . import a, b` -- the moved name is in the import list.
            names = match.group("names")
            rewritten = names
            for name in re.findall(r"\w+", names.split("#")[0]):
                target = "/".join(base + [name])
                if target != old_mod:
                    continue
                new_name = new_mod.split("/")[-1]
                if "/".join(new_mod.split("/")[:-1]) != "/".join(base):
                    # Moved out of this package: a relative import cannot name
                    # it any more. Leave it; validation will flag the break.
                    continue
                rewritten = re.sub(rf"(?<![\w.]){re.escape(name)}(?![\w])", new_name, rewritten)
            if rewritten != names:
                lines[index] = (
                    f"{match.group('head')}{match.group('dots')}"
                    f"{match.group('tail')}{rewritten}"
                )
                changed = True
                count += 1

        if changed:
            _write_text(path, "\n".join(lines))
    return count


_TS_RELATIVE = re.compile(r"(['\"])(\.{1,2}/[^'\"]*)\1")


def _rewrite_ts_relative_imports(tree, old_mod, new_mod):
    """Repoint `from './pkg/mod'` style specifiers at a moved path."""
    count = 0
    for rel in list_files(tree):
        if not rel.endswith(_TS_EXTENSIONS):
            continue
        path = os.path.join(tree, rel)
        body = _read_text(path)
        if body is None:
            continue
        here = os.path.dirname(rel)

        def repoint(match):
            nonlocal count
            quote, spec_text = match.group(1), match.group(2)
            target = os.path.normpath(os.path.join(here, spec_text)).replace(os.sep, "/")
            if target != old_mod and not target.startswith(old_mod + "/"):
                return match.group(0)
            new_target = new_mod + target[len(old_mod) :]
            new_spec = os.path.relpath(new_target, here or ".").replace(os.sep, "/")
            if not new_spec.startswith("."):
                new_spec = "./" + new_spec
            count += 1
            return f"{quote}{new_spec}{quote}"

        updated = _TS_RELATIVE.sub(repoint, body)
        if updated != body:
            _write_text(path, updated)
    return count


def _apply_move(tree, spec, old_rel, new_rel):
    """Move a path and repoint every reference to it. Returns rewrite count."""
    old_path, new_path = os.path.join(tree, old_rel), os.path.join(tree, new_rel)
    if os.path.exists(new_path):
        raise ForgeError(f"move target already exists: {new_rel}")
    os.makedirs(os.path.dirname(new_path), exist_ok=True)
    os.rename(old_path, new_path)

    old_mod, new_mod = _strip_ext(old_rel, spec), _strip_ext(new_rel, spec)
    count = _rewrite_absolute_refs(tree, spec, old_mod, new_mod)
    if spec["language"] == "python":
        count += _rewrite_python_relative_imports(tree, old_mod, new_mod)
    elif spec["language"] in ("typescript", "javascript"):
        count += _rewrite_ts_relative_imports(tree, old_mod, new_mod)
    return count


# ---------------------------------------------------------------------------
# mutations
# ---------------------------------------------------------------------------


def _under_roots(rel, spec):
    return any(
        rel == root or rel.startswith(root + "/") for root in spec["mutable_roots"]
    )


def _protected(rel, spec):
    return any(
        rel == guard or rel.startswith(guard + "/") for guard in spec.get("protect", [])
    )


def _source_files(tree, spec):
    """Files eligible to be shuffled or cloned into a decoy.

    Test files are excluded via source_exclude_pattern. They look like ordinary
    source but carry compiler-visible naming contracts: a Go `_test.go` file
    declares `package foo_test`, so renaming it -- or cloning it to a
    non-`_test.go` name -- puts two packages in one directory and the corpus
    stops building.
    """
    exclude = spec.get("source_exclude_pattern")
    excluded = re.compile(exclude) if exclude else None
    return [
        rel
        for rel in list_files(tree)
        if rel.endswith(tuple(spec["source_extensions"]))
        and _under_roots(rel, spec)
        and not _protected(rel, spec)
        and not (excluded and excluded.search(rel))
    ]


def _rename_one_dir(tree, spec, rng, ops):
    """Rename one directory that holds source, repointing every reference."""
    holders = {os.path.dirname(rel) for rel in _source_files(tree, spec)}
    candidates = sorted(
        rel
        for rel in list_dirs(tree)
        # Strictly BELOW a root: renaming a root itself would move the tree out
        # from under mutable_roots and strand every later mutation.
        if any(rel.startswith(root + "/") for root in spec["mutable_roots"])
        and not _protected(rel, spec)
        and any(h == rel or h.startswith(rel + "/") for h in holders)
    )
    if not candidates:
        raise ForgeError(f"{spec['id']}: no directory left to rename")

    old_rel = rng.choice(candidates)
    new_rel = f"{old_rel}_{_token(rng)}"
    rewrites = _apply_move(tree, spec, old_rel, new_rel)
    ops.append(
        {
            "index": len(ops),
            "kind": "dir_rename",
            "from": old_rel,
            "to": new_rel,
            "references_rewritten": rewrites,
        }
    )


def _shuffle_one_module(tree, spec, rng, ops):
    """Rename one module file inside its package, changing its module path.

    Kept inside the owning package on purpose: moving a module across packages
    cannot be expressed by the relative imports that reference it, so it would
    reliably break the build. Directory renames already relocate whole subtrees.
    """
    candidates = sorted(
        rel
        for rel in _source_files(tree, spec)
        if not os.path.basename(rel).startswith("__")
    )
    if not candidates:
        raise ForgeError(f"{spec['id']}: no module left to shuffle")

    old_rel = rng.choice(candidates)
    parent, name = os.path.split(old_rel)
    stem = _strip_ext(name, spec)
    ext = name[len(stem) :]
    new_rel = os.path.join(parent, f"{stem}_{_token(rng)}{ext}").replace(os.sep, "/")

    rewrites = _apply_move(tree, spec, old_rel, new_rel)
    ops.append(
        {
            "index": len(ops),
            "kind": "module_shuffle",
            "from": old_rel,
            "to": new_rel,
            "references_rewritten": rewrites,
        }
    )


def _captures(pattern, body):
    found = set()
    for match in re.finditer(pattern, body, re.MULTILINE):
        found.update(group for group in match.groups() if group)
    return found


def _is_safe_donor(body, spec):
    """Reject donors whose twin would not be self-consistent.

    Twinning renames a file's top-level declarations but deliberately leaves
    selector expressions (`x.Foo()`) alone, since a selector may belong to
    another package. That is only sound if every method in the file hangs off a
    type the file itself declares: a Go file with `func (c *Cron) runDue()` where
    `Cron` lives in another file would either duplicate a method on the real
    type, or rename the receiver and strand `c.runDue()` on a type that no
    longer has it. Both stop the corpus building.
    """
    receiver_pattern = spec.get("decoy_receiver_pattern")
    if not receiver_pattern:
        return True
    receivers = _captures(receiver_pattern, body)
    declared = _captures(spec.get("decoy_type_pattern", r"$^"), body)
    return receivers <= declared


def _inject_decoys(tree, spec, rng, ops, to_original):
    """Inject dead symbol twins: plausible modules nothing references.

    A decoy is a copy of a real donor module whose top-level symbols are
    renamed, so it compiles, looks idiomatic, and is entirely dead. Provenance
    records the donor at its ORIGINAL path so a reviewer can always tell a
    decoy apart from real code.
    """
    symbol_pattern = re.compile(spec["symbol_pattern"], re.MULTILINE)
    decoys = []

    for _ in range(spec["mutations"].get("decoys", 0)):
        existing = {op["path"] for op in decoys}
        candidates = []
        for rel in _source_files(tree, spec):
            if rel in existing or os.path.basename(rel).startswith("__"):
                continue
            body = _read_text(os.path.join(tree, rel))
            if body and symbol_pattern.search(body) and _is_safe_donor(body, spec):
                candidates.append(rel)
        if not candidates:
            raise ForgeError(f"{spec['id']}: no donor module for a decoy")

        donor = rng.choice(sorted(candidates))
        body = _read_text(os.path.join(tree, donor))
        names = []
        for match in symbol_pattern.finditer(body):
            name = next(group for group in match.groups() if group)
            if name not in names:
                names.append(name)

        suffix = _token(rng)
        twins = []
        mutated = body
        for name in names:
            twin = f"{name}_{suffix}"
            mutated = re.sub(rf"(?<![\w.]){re.escape(name)}(?![\w])", twin, mutated)
            twins.append({"original": name, "twin": twin})

        parent, filename = os.path.split(donor)
        stem = _strip_ext(filename, spec)
        ext = filename[len(stem) :]
        decoy_rel = os.path.join(parent, f"{stem}_{suffix}{ext}").replace(os.sep, "/")
        if os.path.exists(os.path.join(tree, decoy_rel)):
            raise ForgeError(f"{spec['id']}: decoy would overwrite {decoy_rel}")
        _write_text(os.path.join(tree, decoy_rel), mutated)

        record = {
            "path": decoy_rel,
            "derived_from": to_original[donor],
            "source_commit": spec["pinned_commit"],
            "decoy_kind": "dead_symbol_twin",
            "symbols": twins,
        }
        decoys.append(record)
        ops.append({"index": len(ops), "kind": "decoy_module", **record})

    return decoys


# ---------------------------------------------------------------------------
# validation
# ---------------------------------------------------------------------------


def _expand(token):
    return token.replace("{python}", sys.executable).replace(
        "{forge}", os.path.abspath(__file__)
    )


def run_validation(validation, tree):
    """Run the corpus's configured build/import check inside the mutated tree."""
    argv = [_expand(token) for token in validation["argv"]]
    env = dict(os.environ)
    env.update(validation.get("env", {}))
    expect_exit = validation.get("expect_exit", 0)

    proc = subprocess.run(
        argv,
        cwd=tree,
        capture_output=True,
        text=True,
        env=env,
        timeout=validation.get("timeout_seconds", 1800),
    )
    return {
        "argv": argv,
        "expect_exit": expect_exit,
        "exit_code": proc.returncode,
        "passed": proc.returncode == expect_exit,
        "stdout_tail": proc.stdout[-2000:],
        "stderr_tail": proc.stderr[-2000:],
    }


def check_imports(root, language):
    """Non-mutating import resolvability check. Returns a list of problems.

    This is the oracle for path mutations: if a rename broke a module path,
    an import stops resolving and the corpus is not usable.
    """
    if language == "python":
        return _check_python_imports(root)
    if language in ("typescript", "javascript"):
        return _check_ts_imports(root)
    raise ForgeError(f"check-imports: unsupported language {language!r}")


def _py_exists(root, parts):
    base = os.path.join(root, *parts)
    return os.path.isfile(base + ".py") or os.path.isdir(base)


def _check_python_imports(root):
    problems = []
    first_party = {
        name[:-3] if name.endswith(".py") else name
        for name in os.listdir(root)
        if name.endswith(".py")
        or os.path.isfile(os.path.join(root, name, "__init__.py"))
    }

    for rel in list_files(root):
        if not rel.endswith(".py"):
            continue
        body = _read_text(os.path.join(root, rel))
        if body is None:
            continue
        try:
            tree = ast.parse(body, rel)
        except SyntaxError as err:
            problems.append(f"{rel}: syntax error: {err}")
            continue

        pkg = os.path.dirname(rel).split(os.sep) if os.path.dirname(rel) else []
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                for alias in node.names:
                    parts = alias.name.split(".")
                    if parts[0] in first_party and not _py_exists(root, parts):
                        problems.append(f"{rel}:{node.lineno}: unresolved {alias.name}")
            elif isinstance(node, ast.ImportFrom):
                if node.level == 0:
                    if not node.module:
                        continue
                    parts = node.module.split(".")
                    if parts[0] in first_party and not _py_exists(root, parts):
                        problems.append(
                            f"{rel}:{node.lineno}: unresolved {node.module}"
                        )
                    continue
                if node.level - 1 > len(pkg):
                    problems.append(f"{rel}:{node.lineno}: relative import escapes root")
                    continue
                base = pkg[: len(pkg) - (node.level - 1)]
                parts = base + (node.module.split(".") if node.module else [])
                if parts and not _py_exists(root, parts):
                    problems.append(
                        f"{rel}:{node.lineno}: unresolved relative "
                        f"{'.' * node.level}{node.module or ''}"
                    )
    return problems


def _ts_resolves(root, target):
    base = os.path.join(root, target)
    if os.path.isfile(base):
        return True
    for ext in _TS_EXTENSIONS:
        if os.path.isfile(base + ext):
            return True
    if os.path.isdir(base):
        for ext in _TS_EXTENSIONS:
            if os.path.isfile(os.path.join(base, "index" + ext)):
                return True
    return False


def _check_ts_imports(root):
    problems = []
    for rel in list_files(root):
        if not rel.endswith(_TS_EXTENSIONS) or rel.endswith(".json"):
            continue
        body = _read_text(os.path.join(root, rel))
        if body is None:
            continue
        here = os.path.dirname(rel)
        for match in _TS_RELATIVE.finditer(body):
            target = os.path.normpath(os.path.join(here, match.group(2)))
            if not _ts_resolves(root, target):
                problems.append(f"{rel}: unresolved import {match.group(2)!r}")
    return problems


# ---------------------------------------------------------------------------
# manifest + forge
# ---------------------------------------------------------------------------


def load_corpora(path=CORPORA_PATH):
    with open(path) as handle:
        data = json.load(handle)
    if data.get("forge_version") != FORGE_VERSION:
        raise ForgeError(
            f"{path}: forge_version {data.get('forge_version')!r} "
            f"!= {FORGE_VERSION!r}; mutations are not comparable across versions"
        )
    return {spec["id"]: spec for spec in data["corpora"]}


def translate_path(manifest, original):
    """Translate an original oracle anchor path to the mutated checkout."""
    try:
        return manifest["path_map"][original]
    except KeyError:
        raise ForgeError(
            f"{original!r} is not a file of source commit "
            f"{manifest['source_commit']}"
        ) from None


def _write_json_atomic(path, obj):
    parent = os.path.dirname(path)
    os.makedirs(parent, exist_ok=True)
    handle, tmp = tempfile.mkstemp(dir=parent, suffix=".tmp")
    try:
        with os.fdopen(handle, "w") as out:
            json.dump(obj, out, indent=2, sort_keys=True)
            out.write("\n")
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def forge_corpus(
    spec, seed, out_dir, allow_dirty=False, allow_head_mismatch=False
):
    """Forge one mutated corpus. Raises ValidationFailed if it does not build."""
    overrides = verify_source(spec, allow_dirty, allow_head_mismatch)

    ready_marker = os.path.join(out_dir, "READY")
    if os.path.exists(ready_marker):
        os.unlink(ready_marker)

    tree = materialize(spec, out_dir)
    rng = _rng(spec, seed)

    # original -> current, maintained across every move so anchors stay usable.
    path_map = {rel: rel for rel in list_files(tree)}
    ops = []

    def remap(old_rel, new_rel):
        for original, current in path_map.items():
            if current == old_rel:
                path_map[original] = new_rel
            elif current.startswith(old_rel + "/"):
                path_map[original] = new_rel + current[len(old_rel) :]

    def tracked(mutate):
        mutate(tree, spec, rng, ops)
        op = ops[-1]
        remap(op["from"], op["to"])

    # Renames first, then shuffles, then decoys: decoys are minted at their
    # final path and are never themselves mutated.
    for _ in range(spec["mutations"].get("dir_renames", 0)):
        tracked(_rename_one_dir)
    for _ in range(spec["mutations"].get("module_shuffles", 0)):
        tracked(_shuffle_one_module)

    to_original = {current: original for original, current in path_map.items()}
    decoys = _inject_decoys(tree, spec, rng, ops, to_original)

    hash_before = tree_hash(tree)
    validation = run_validation(spec["validation"], tree)
    if tree_hash(tree) != hash_before:
        raise ForgeError(
            f"{spec['id']}: validation command wrote into the corpus; it must be "
            f"non-mutating: {validation['argv']}"
        )

    manifest = {
        "schema": MANIFEST_SCHEMA,
        "corpus_id": spec["id"],
        "source_path": spec["source_path"],
        "source_commit": spec["pinned_commit"],
        "seed": seed,
        "forge_version": FORGE_VERSION,
        "overrides": overrides,
        "mutations": ops,
        "path_map": dict(sorted(path_map.items())),
        "decoys": decoys,
        "tree_hash": hash_before,
        "validation": validation,
        "status": "ready" if validation["passed"] else "validation_failed",
    }
    _write_json_atomic(os.path.join(out_dir, "manifest.json"), manifest)

    if not validation["passed"]:
        raise ValidationFailed(
            f"{spec['id']}: validation failed with exit {validation['exit_code']}; "
            f"corpus is NOT ready\n{validation['stderr_tail'] or validation['stdout_tail']}",
            manifest,
        )

    with open(ready_marker, "w") as handle:
        handle.write(hash_before + "\n")
    return manifest


# ---------------------------------------------------------------------------
# cli
# ---------------------------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("forge", help="forge a mutated corpus")
    run.add_argument("--corpora", default=CORPORA_PATH)
    run.add_argument("--corpus", required=True)
    run.add_argument("--seed", type=int, required=True)
    run.add_argument("--out", default=None)
    run.add_argument("--allow-dirty", action="store_true")
    run.add_argument("--allow-head-mismatch", action="store_true")

    check = sub.add_parser("check-imports", help="non-mutating import check")
    check.add_argument("--language", required=True)
    check.add_argument("--root", default=".")

    trans = sub.add_parser("translate", help="original -> mutated path")
    trans.add_argument("--manifest", required=True)
    trans.add_argument("--path", required=True)

    args = parser.parse_args(argv)

    if args.command == "check-imports":
        problems = check_imports(os.path.abspath(args.root), args.language)
        for problem in problems:
            print(f"UNRESOLVED {problem}")
        if problems:
            print(f"import check failed: {len(problems)} problem(s)")
            return 1
        print("import check passed")
        return 0

    if args.command == "translate":
        with open(args.manifest) as handle:
            manifest = json.load(handle)
        print(translate_path(manifest, args.path))
        return 0

    specs = load_corpora(args.corpora)
    if args.corpus not in specs:
        raise ForgeError(f"unknown corpus {args.corpus!r}; have {sorted(specs)}")
    out_dir = args.out or os.path.join(
        DEFAULT_OUT_ROOT, args.corpus, f"seed-{args.seed}"
    )

    manifest = forge_corpus(
        specs[args.corpus],
        seed=args.seed,
        out_dir=out_dir,
        allow_dirty=args.allow_dirty,
        allow_head_mismatch=args.allow_head_mismatch,
    )
    print(
        f"{manifest['corpus_id']} seed={manifest['seed']} "
        f"tree={manifest['tree_hash'][:12]} "
        f"mutations={len(manifest['mutations'])} "
        f"decoys={len(manifest['decoys'])} -> {out_dir}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ForgeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
