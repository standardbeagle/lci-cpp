#!/usr/bin/env python3
"""Validate stage-1 exploration tasks: schema, dual annotation, and live anchors.

An exploration task (see exploration/schema/exploration-task.schema.json) carries
a goal-oriented prompt plus an adjudicated evidence set. This validator is the
oracle for the task bank: it returns NONZERO for any malformed task, a missing
corpus/manifest, fewer than two anchors, a stale line bound, a prompt that leaks
its own answer, or an incomplete/unresolved dual annotation.

Two layers:

  * Structural (always, hermetic): JSON Schema conformance, >=2 anchors, exactly
    two independent annotation records whose annotators match `adjudication`,
    resolved disagreement, adjudicated evidence backed by an annotation, and no
    target identifier or evidence path leaking into the prompt.
  * Live anchors (when the forged corpus is present, or forced with
    --require-live): every final anchor's MUTATED path exists in the forged tree
    and is a real mutated/decoy path per the manifest, its line bounds fit the
    file, and each recorded target identifier occurs within the bounded evidence.

The forged corpus is NEVER vendored into git (see corpora.json). It is
regenerated deterministically from (source commit, forge version, seed); the
manifest pins exactly which forge this bank was verified against. When the tree
is absent and --require-live is not set, live checks are reported as SKIPPED
(never silently passed) and the structural layer still gates.
"""

import argparse
import json
import os
import re
import sys

import jsonschema

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exploration_corpus_forge as forge

BENCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPLORATION_ROOT = os.path.join(BENCH_ROOT, "exploration")
DEFAULT_TASKS_DIR = os.path.join(EXPLORATION_ROOT, "tasks")
DEFAULT_ANNOTATIONS_DIR = os.path.join(EXPLORATION_ROOT, "annotations")
DEFAULT_SCHEMA_PATH = os.path.join(
    EXPLORATION_ROOT, "schema", "exploration-task.schema.json"
)
DEFAULT_CORPORA_PATH = os.path.join(EXPLORATION_ROOT, "corpora.json")
# Where the forge lands corpora: <corpus_root>/<corpus_id>/seed-<seed>/{manifest.json,tree/}.
DEFAULT_CORPUS_ROOT = forge.DEFAULT_OUT_ROOT

TASK_SCHEMA_CONST = "exploration_task_v1"
ANNOTATION_SCHEMA_CONST = "exploration_annotation_v1"


class Problem(Exception):
    """A single validation failure, tagged with the task it belongs to."""


def _load_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def _word_present(identifier, text):
    """Whole-identifier match: `Foo` must not match `Foobar` or `p.Foo`-only."""
    return re.search(
        r"(?<![\w.])" + re.escape(identifier) + r"(?![\w])", text
    ) is not None


def _anchor_signature(anchor):
    return (
        anchor["path"],
        tuple(anchor["lines"]),
        tuple(sorted(anchor["target_identifiers"])),
    )


# ---------------------------------------------------------------------------
# annotation loading
# ---------------------------------------------------------------------------


def load_annotations(annotations_dir, task_id):
    """Every annotation record whose task_id matches, keyed by annotator.

    Annotation files live at annotations/<task_id>.<annotator>.json and declare
    both fields internally; the filename is a convenience, the fields are the
    contract.
    """
    found = {}
    if not os.path.isdir(annotations_dir):
        return found
    for name in sorted(os.listdir(annotations_dir)):
        if not name.endswith(".json"):
            continue
        record = _load_json(os.path.join(annotations_dir, name))
        if not isinstance(record, dict) or record.get("task_id") != task_id:
            continue
        annotator = record.get("annotator")
        if not annotator:
            raise Problem(f"{name}: annotation record has no annotator")
        if annotator in found:
            raise Problem(
                f"{task_id}: duplicate annotation from annotator {annotator!r}"
            )
        found[annotator] = record
    return found


def _check_annotation_record(record):
    if record.get("schema") != ANNOTATION_SCHEMA_CONST:
        raise Problem(
            f"annotation schema {record.get('schema')!r} != {ANNOTATION_SCHEMA_CONST!r}"
        )
    evidence = record.get("evidence")
    if not isinstance(evidence, list) or not evidence:
        raise Problem("annotation has no evidence")
    for anchor in evidence:
        for field in ("path", "lines", "target_identifiers"):
            if field not in anchor:
                raise Problem(f"annotation anchor missing {field!r}")


# ---------------------------------------------------------------------------
# corpus / manifest location
# ---------------------------------------------------------------------------


def locate_manifest(corpus_root, corpus_id, seed):
    """Return (manifest_dict, tree_dir) for a forged corpus, or (None, None)."""
    corpus_dir = os.path.join(corpus_root, corpus_id, f"seed-{seed}")
    manifest_path = os.path.join(corpus_dir, "manifest.json")
    tree_dir = os.path.join(corpus_dir, "tree")
    if not (os.path.isfile(manifest_path) and os.path.isdir(tree_dir)):
        return None, None
    return _load_json(manifest_path), tree_dir


def _mutated_paths(manifest):
    paths = set(manifest.get("path_map", {}).values())
    paths.update(decoy["path"] for decoy in manifest.get("decoys", []))
    return paths


def _line_count(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return sum(1 for _ in handle)


def _read_range(path, start, end):
    lines = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for index, line in enumerate(handle, start=1):
            if index < start:
                continue
            if index > end:
                break
            lines.append(line)
    return "".join(lines)


def verify_anchor_live(anchor, manifest, tree_dir):
    """Verify one anchor against the forged tree. Yields problem strings."""
    path = anchor["path"]
    if path not in _mutated_paths(manifest):
        yield (
            f"anchor path {path!r} is not a mutated/decoy path of the forged "
            f"corpus (stale or upstream path?)"
        )
        return
    abs_path = os.path.join(tree_dir, path)
    if not os.path.isfile(abs_path):
        yield f"anchor path {path!r} does not exist in the forged tree"
        return

    lines = anchor["lines"]
    start = lines[0]
    end = lines[-1]
    if end < start:
        yield f"{path}: line bound {lines} is inverted"
        return
    total = _line_count(abs_path)
    if end > total:
        yield f"{path}: line bound {lines} exceeds file length {total}"
        return

    segment = _read_range(abs_path, start, end)
    for identifier in anchor["target_identifiers"]:
        if not _word_present(identifier, segment):
            yield (
                f"{path}:{start}-{end}: target identifier {identifier!r} does "
                f"not occur within the bounded evidence"
            )


# ---------------------------------------------------------------------------
# leakage
# ---------------------------------------------------------------------------


def _basename_stem(path):
    """The distinctive leaf of a target path: basename, extension and leading
    underscores stripped. `sklearn/metrics/cluster/_unsupervised.py` -> the
    `unsupervised` that would name the target file. Ancestor directory names
    (server, client, tools) are broad architecture, not the target path, so they
    are deliberately not treated as leaks."""
    base = re.split(r"[\\/]", path)[-1]
    stem = re.split(r"\.", base)[0].lstrip("_")
    return stem


def check_prompt_leakage(prompt, evidence):
    """The prompt must not name a target identifier or a target file's leaf name.

    Naming either hands the tool the answer, defeating the exploration measure.
    """
    problems = []
    lowered = prompt.lower()
    for anchor in evidence:
        for identifier in anchor["target_identifiers"]:
            if len(identifier) >= 3 and _word_present(identifier.lower(), lowered):
                problems.append(
                    f"prompt leaks target identifier {identifier!r}"
                )
        stem = _basename_stem(anchor["path"])
        if len(stem) >= 3 and _word_present(stem.lower(), lowered):
            problems.append(f"prompt leaks target file name {stem!r}")
    return problems


# ---------------------------------------------------------------------------
# per-task validation
# ---------------------------------------------------------------------------


def validate_task(
    task, schema, corpora, annotations_dir, corpus_root, require_live
):
    """Return a list of problem strings for one task object."""
    problems = []

    validator = jsonschema.Draft202012Validator(schema)
    schema_errors = sorted(
        validator.iter_errors(task), key=lambda err: list(err.path)
    )
    if schema_errors:
        loc = task.get("id", "<no id>")
        for err in schema_errors:
            path = "/".join(str(part) for part in err.path) or "<root>"
            problems.append(f"{loc}: schema violation at {path}: {err.message}")
        # A schema-invalid task cannot be trusted for the deeper checks.
        return problems

    task_id = task["id"]
    corpus_id = task["corpus"]
    ref = task["manifest_ref"]

    spec = corpora.get(corpus_id)
    if spec is None:
        problems.append(f"{task_id}: unknown corpus {corpus_id!r}")
        return problems
    if ref["corpus_id"] != corpus_id:
        problems.append(
            f"{task_id}: manifest_ref.corpus_id {ref['corpus_id']!r} != "
            f"corpus {corpus_id!r}"
        )
    if ref["source_commit"] != spec["pinned_commit"]:
        problems.append(
            f"{task_id}: manifest_ref.source_commit does not match the pinned "
            f"commit for {corpus_id}"
        )
    if ref["forge_version"] != forge.FORGE_VERSION:
        problems.append(
            f"{task_id}: manifest_ref.forge_version {ref['forge_version']!r} != "
            f"{forge.FORGE_VERSION!r}"
        )

    evidence = task["evidence"]

    # ---- dual annotation ------------------------------------------------
    try:
        annotations = load_annotations(annotations_dir, task_id)
    except Problem as err:
        problems.append(f"{task_id}: {err}")
        annotations = {}

    declared = set(task["adjudication"]["annotators"])
    if len(annotations) < 2:
        problems.append(
            f"{task_id}: needs 2 independent annotation records, found "
            f"{len(annotations)}"
        )
    present = set(annotations)
    if annotations and present != declared:
        problems.append(
            f"{task_id}: annotation records {sorted(present)} do not match "
            f"declared annotators {sorted(declared)}"
        )

    annotation_sigs = []
    for annotator, record in sorted(annotations.items()):
        try:
            _check_annotation_record(record)
        except Problem as err:
            problems.append(f"{task_id}[{annotator}]: {err}")
            continue
        sigs = set()
        for anchor in record["evidence"]:
            sigs.add(
                (
                    anchor["path"],
                    tuple(anchor["lines"]),
                    tuple(sorted(anchor["target_identifiers"])),
                )
            )
        annotation_sigs.append(sigs)

    if len(annotation_sigs) >= 2:
        disagreement = any(
            annotation_sigs[0] != other for other in annotation_sigs[1:]
        )
        if disagreement and not task["adjudication"]["resolved"]:
            problems.append(
                f"{task_id}: annotators disagree but adjudication.resolved is "
                f"false (unresolved disagreement)"
            )
        if disagreement and not task["adjudication"].get("notes"):
            problems.append(
                f"{task_id}: adjudicated disagreement needs adjudication.notes"
            )
        # Every adjudicated anchor must be backed by at least one annotator.
        union = set().union(*annotation_sigs)
        for anchor in evidence:
            if _anchor_signature(anchor) not in union:
                problems.append(
                    f"{task_id}: adjudicated anchor {anchor['path']}:"
                    f"{anchor['lines']} is not supported by any annotation record"
                )
    if not task["adjudication"]["resolved"]:
        problems.append(f"{task_id}: adjudication.resolved is false")

    # ---- prompt leakage -------------------------------------------------
    for message in check_prompt_leakage(task["prompt"], evidence):
        problems.append(f"{task_id}: {message}")

    # ---- live anchors ---------------------------------------------------
    manifest, tree_dir = locate_manifest(corpus_root, corpus_id, ref["seed"])
    if manifest is None:
        if require_live:
            problems.append(
                f"{task_id}: forged corpus for {corpus_id} seed {ref['seed']} "
                f"not found under {corpus_root} (required by --require-live)"
            )
        else:
            print(
                f"NOTICE {task_id}: forged corpus absent; live anchor "
                f"verification SKIPPED",
                file=sys.stderr,
            )
    else:
        if manifest.get("source_commit") != ref["source_commit"]:
            problems.append(
                f"{task_id}: forged manifest source_commit does not match "
                f"manifest_ref.source_commit"
            )
        if manifest.get("seed") != ref["seed"]:
            problems.append(
                f"{task_id}: forged manifest seed {manifest.get('seed')} != "
                f"manifest_ref.seed {ref['seed']}"
            )
        for anchor in evidence:
            for message in verify_anchor_live(anchor, manifest, tree_dir):
                problems.append(f"{task_id}: {message}")

    return problems


# ---------------------------------------------------------------------------
# bank validation
# ---------------------------------------------------------------------------


def load_schema(schema_path=DEFAULT_SCHEMA_PATH):
    return _load_json(schema_path)


def load_corpora(corpora_path=DEFAULT_CORPORA_PATH):
    return forge.load_corpora(corpora_path)


def validate_bank(
    tasks_dir=DEFAULT_TASKS_DIR,
    annotations_dir=DEFAULT_ANNOTATIONS_DIR,
    schema_path=DEFAULT_SCHEMA_PATH,
    corpora_path=DEFAULT_CORPORA_PATH,
    corpus_root=DEFAULT_CORPUS_ROOT,
    require_live=False,
):
    """Validate every task file under tasks_dir. Returns (problems, summary)."""
    if not os.path.isdir(tasks_dir):
        return [f"tasks dir does not exist: {tasks_dir}"], {}

    schema = load_schema(schema_path)
    corpora = load_corpora(corpora_path)

    task_files = sorted(
        name for name in os.listdir(tasks_dir) if name.endswith(".json")
    )
    problems = []
    seen_ids = {}
    per_corpus = {}
    for name in task_files:
        task = _load_json(os.path.join(tasks_dir, name))
        task_id = task.get("id")
        if task_id in seen_ids:
            problems.append(
                f"{task_id}: duplicate task id (also in {seen_ids[task_id]})"
            )
        seen_ids[task_id] = name
        problems.extend(
            validate_task(
                task, schema, corpora, annotations_dir, corpus_root, require_live
            )
        )
        per_corpus[task.get("corpus")] = per_corpus.get(task.get("corpus"), 0) + 1

    summary = {"tasks": len(task_files), "per_corpus": per_corpus}
    return problems, summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tasks-dir", default=DEFAULT_TASKS_DIR)
    parser.add_argument("--annotations-dir", default=DEFAULT_ANNOTATIONS_DIR)
    parser.add_argument("--schema", default=DEFAULT_SCHEMA_PATH)
    parser.add_argument("--corpora", default=DEFAULT_CORPORA_PATH)
    parser.add_argument("--corpus-root", default=DEFAULT_CORPUS_ROOT)
    parser.add_argument(
        "--require-live",
        action="store_true",
        help="fail if the forged corpus is absent instead of skipping live checks",
    )
    args = parser.parse_args(argv)

    problems, summary = validate_bank(
        tasks_dir=args.tasks_dir,
        annotations_dir=args.annotations_dir,
        schema_path=args.schema,
        corpora_path=args.corpora,
        corpus_root=args.corpus_root,
        require_live=args.require_live,
    )

    for problem in sorted(problems):
        print(f"INVALID {problem}")
    if problems:
        print(
            f"exploration task validation FAILED: {len(problems)} problem(s) "
            f"across {summary.get('tasks', 0)} task(s)"
        )
        return 1
    print(
        f"exploration task validation passed: {summary['tasks']} task(s) "
        f"{summary['per_corpus']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except forge.ForgeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
