#!/usr/bin/env python3
"""Validate stage-3 convention-conformance edit tasks: schema, oracle gates,
dual annotation, prompt-leak, and LIVE exemplar conformance.

An edit task (see edits/schema/edit-task.schema.json) states ONLY a goal and
carries a machine-readable oracle key: a behaviour command with a red->green
discrimination, a mechanical convention rule, >=2 LIVE convention-exemplar
anchors (independently annotated by two annotators), a declared patch scope
(blast radius), and a manifest reference sufficient to translate anchors.

Layers:

  * Schema (jsonschema): rejects a task MISSING the behaviour, convention,
    exemplar, or blast-radius gate, or with fewer than two exemplars.
  * Degeneracy (structural, hermetic): rejects a DEGENERATE gate even when
    present -- a red==green discrimination, a convention pattern that matches
    everything, or a blast radius whose allow-set is a match-everything glob.
  * Dual annotation (structural, hermetic): exactly the two declared annotators,
    each a well-formed record; annotator disagreement (including whether an
    exemplar is a live/conforming location) must be adjudicated with notes; every
    adjudicated exemplar is backed by at least one annotator.
  * Prompt leak (structural, hermetic): the existing exploration leak linter is
    reused verbatim (its matcher, its derived-from-oracle term set) against the
    exemplars-as-evidence, so a prompt that names any target path/symbol fails.
  * Live exemplars (when the forged corpus is present, or with --require-live):
    every exemplar's MUTATED path is a LIVE mutated file of the corpus and NOT a
    decoy (dead/deprecated twin); its line bounds fit; each target identifier
    occurs within the bounded evidence; and the bounded evidence MATCHES the
    convention's conforms_pattern.

ORACLE INDEPENDENCE (bench-harness-oracle-independence rule): the liveness gate
derives the live-path set (path_map values) and the dead-twin set (decoy paths)
DIRECTLY from the manifest and checks set membership itself -- it never calls the
forge's mutation code to decide liveness, so a forge decoy bug surfaces here
instead of being assumed away. The paired discrimination is proven in the test
suite: an exemplar aimed at a decoy path FAILS, and a bounded region that does
not match conforms_pattern FAILS, while the good forms PASS.

The forged corpus is NEVER vendored (see exploration/corpora.json); when it is
absent and --require-live is not set, live checks are reported SKIPPED (never
silently passed) and the structural layers still gate.
"""

import argparse
import os
import re
import sys

import jsonschema

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import validate_exploration_tasks as vet  # noqa: E402  (reused helpers)
import lint_exploration_leaks as leak_linter  # noqa: E402  (reused matcher)

BENCH_ROOT = vet.BENCH_ROOT
EDITS_ROOT = os.path.join(BENCH_ROOT, "edits")
DEFAULT_TASKS_DIR = os.path.join(EDITS_ROOT, "tasks")
DEFAULT_ANNOTATIONS_DIR = os.path.join(EDITS_ROOT, "annotations")
DEFAULT_SCHEMA_PATH = os.path.join(EDITS_ROOT, "schema", "edit-task.schema.json")
DEFAULT_CORPORA_PATH = vet.DEFAULT_CORPORA_PATH
DEFAULT_CORPUS_ROOT = vet.DEFAULT_CORPUS_ROOT

TASK_SCHEMA_CONST = "edit_task_v1"
ANNOTATION_SCHEMA_CONST = "edit_annotation_v1"

REQUIRED_CATEGORIES = {
    "retry-error-handling",
    "logging",
    "module-extraction-layout",
    "api-shape-consistency",
}

Problem = vet.Problem
_load_json = vet._load_json


# ---------------------------------------------------------------------------
# manifest-derived sets (owned here, independent of the forge's mutation code)
# ---------------------------------------------------------------------------


def live_paths(manifest):
    """Real mutated source files (path_map values). Excludes decoys."""
    return set(manifest.get("path_map", {}).values())


def decoy_paths(manifest):
    """Dead/deprecated twin paths the forge injected."""
    return {decoy["path"] for decoy in manifest.get("decoys", [])}


# ---------------------------------------------------------------------------
# annotation loading (edit shape: exemplars, each with a `conforms` verdict)
# ---------------------------------------------------------------------------


def load_annotations(annotations_dir, task_id):
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
            f"annotation schema {record.get('schema')!r} != "
            f"{ANNOTATION_SCHEMA_CONST!r}"
        )
    exemplars = record.get("exemplars")
    if not isinstance(exemplars, list) or not exemplars:
        raise Problem("annotation has no exemplars")
    for anchor in exemplars:
        for field in ("path", "lines", "target_identifiers", "conforms"):
            if field not in anchor:
                raise Problem(f"annotation exemplar missing {field!r}")
        if not isinstance(anchor["conforms"], bool):
            raise Problem("annotation exemplar `conforms` must be boolean")


def _annotation_signature(anchor):
    return (
        anchor["path"],
        tuple(anchor["lines"]),
        tuple(sorted(anchor["target_identifiers"])),
        bool(anchor["conforms"]),
    )


def _exemplar_signature(anchor):
    return (
        anchor["path"],
        tuple(anchor["lines"]),
        tuple(sorted(anchor["target_identifiers"])),
    )


# ---------------------------------------------------------------------------
# degeneracy gates
# ---------------------------------------------------------------------------


_WILDCARD_ONLY = re.compile(r"[*/.\s]+$")


def _blast_glob_matches_everything(glob):
    """A glob made only of wildcards/separators bounds nothing."""
    stripped = re.sub(r"[*/.\s]", "", glob)
    return stripped == ""


def compile_convention(pattern):
    """Compile a convention pattern (multi-line). Raise Problem if degenerate."""
    try:
        compiled = re.compile(pattern, re.MULTILINE)
    except re.error as err:
        raise Problem(f"convention conforms_pattern does not compile: {err}")
    # A pattern that matches the empty string gates nothing.
    if compiled.match("") is not None:
        raise Problem(
            "convention conforms_pattern matches the empty string "
            "(degenerate: it accepts everything)"
        )
    return compiled


# ---------------------------------------------------------------------------
# live exemplar verification
# ---------------------------------------------------------------------------


def verify_exemplar_live(anchor, convention_re, manifest, tree_dir):
    """Verify one exemplar against the forged tree. Yields problem strings."""
    path = anchor["path"]
    if path in decoy_paths(manifest):
        yield (
            f"exemplar path {path!r} is a DECOY (dead/deprecated twin), not a "
            f"live convention exemplar"
        )
        return
    if path not in live_paths(manifest):
        yield (
            f"exemplar path {path!r} is not a live mutated file of the forged "
            f"corpus (stale or upstream path?)"
        )
        return
    abs_path = os.path.join(tree_dir, path)
    if not os.path.isfile(abs_path):
        yield f"exemplar path {path!r} does not exist in the forged tree"
        return

    lines = anchor["lines"]
    start = lines[0]
    end = lines[-1]
    if end < start:
        yield f"{path}: line bound {lines} is inverted"
        return
    # Same bound test the RUNTIME gate applies (conformance_gate._evaluate_anchor,
    # ANCHOR_BOUND_STALE). Kept identical on purpose: a bound the gate will reject
    # must never survive bank validation.
    if start < 1:
        yield f"{path}: line bound {lines} starts before line 1"
        return
    total = vet._line_count(abs_path)
    if end > total:
        yield f"{path}: line bound {lines} exceeds file length {total}"
        return

    segment = vet._read_range(abs_path, start, end)
    for identifier in anchor["target_identifiers"]:
        if not vet._word_present(identifier, segment):
            yield (
                f"{path}:{start}-{end}: target identifier {identifier!r} does "
                f"not occur within the bounded evidence"
            )
    if convention_re.search(segment) is None:
        yield (
            f"{path}:{start}-{end}: bounded evidence does not match the "
            f"convention rule (not a conforming exemplar)"
        )


# ---------------------------------------------------------------------------
# per-task validation
# ---------------------------------------------------------------------------


def validate_task(
    task, schema, corpora, annotations_dir, corpus_root, require_live
):
    problems = []

    validator = jsonschema.Draft202012Validator(schema)
    schema_errors = sorted(
        validator.iter_errors(task), key=lambda err: list(err.path)
    )
    if schema_errors:
        loc = task.get("id", "<no id>")
        for err in schema_errors:
            where = "/".join(str(part) for part in err.path) or "<root>"
            problems.append(f"{loc}: schema violation at {where}: {err.message}")
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
    if ref["forge_version"] != vet.forge.FORGE_VERSION:
        problems.append(
            f"{task_id}: manifest_ref.forge_version {ref['forge_version']!r} != "
            f"{vet.forge.FORGE_VERSION!r}"
        )

    # ---- degeneracy gates ----------------------------------------------
    discrimination = task["behavior"]["discrimination"]
    if discrimination["red"].strip() == discrimination["green"].strip():
        problems.append(
            f"{task_id}: behaviour discrimination is degenerate (red == green)"
        )

    convention_re = None
    try:
        convention_re = compile_convention(task["convention"]["conforms_pattern"])
    except Problem as err:
        problems.append(f"{task_id}: {err}")

    for glob in task["blast_radius"]["allow"]:
        if _blast_glob_matches_everything(glob):
            problems.append(
                f"{task_id}: blast_radius allow entry {glob!r} matches "
                f"everything (degenerate patch scope)"
            )

    exemplars = task["exemplars"]

    # ---- dual annotation -----------------------------------------------
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
    conforms_by_exemplar = {}
    for annotator, record in sorted(annotations.items()):
        try:
            _check_annotation_record(record)
        except Problem as err:
            problems.append(f"{task_id}[{annotator}]: {err}")
            continue
        sigs = set()
        for anchor in record["exemplars"]:
            sigs.add(_annotation_signature(anchor))
            key = _exemplar_signature(anchor)
            conforms_by_exemplar.setdefault(key, []).append(
                bool(anchor["conforms"])
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
        union = set().union(*annotation_sigs)
        union_exemplars = {sig[:3] for sig in union}
        for anchor in exemplars:
            if _exemplar_signature(anchor) not in union_exemplars:
                problems.append(
                    f"{task_id}: adjudicated exemplar {anchor['path']}:"
                    f"{anchor['lines']} is not supported by any annotation record"
                )

    # Each final exemplar must be attested LIVE/conforming by every annotator
    # that recorded it; a lone dissent that was not adjudicated is surfaced.
    for anchor in exemplars:
        verdicts = conforms_by_exemplar.get(_exemplar_signature(anchor))
        if verdicts and not all(verdicts):
            if not task["adjudication"]["resolved"]:
                problems.append(
                    f"{task_id}: exemplar {anchor['path']}:{anchor['lines']} "
                    f"marked non-conforming by an annotator without adjudication"
                )

    if not task["adjudication"]["resolved"]:
        problems.append(f"{task_id}: adjudication.resolved is false")

    # ---- prompt leak (reuse the exploration linter's matcher) -----------
    manifest, tree_dir = vet.locate_manifest(corpus_root, corpus_id, ref["seed"])
    leak_view = {
        "id": task_id,
        "prompt": task["prompt"],
        "evidence": exemplars,
    }
    for _tid, category, redacted in leak_linter.find_leaks(leak_view, manifest):
        problems.append(f"{task_id}: prompt leak: {category} match {redacted}")

    # ---- live exemplars -------------------------------------------------
    if manifest is None:
        if require_live:
            problems.append(
                f"{task_id}: forged corpus for {corpus_id} seed {ref['seed']} "
                f"not found under {corpus_root} (required by --require-live)"
            )
        else:
            print(
                f"NOTICE {task_id}: forged corpus absent; live exemplar "
                f"verification SKIPPED",
                file=sys.stderr,
            )
    elif convention_re is not None:
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
        for anchor in exemplars:
            for message in verify_exemplar_live(
                anchor, convention_re, manifest, tree_dir
            ):
                problems.append(f"{task_id}: {message}")

    return problems


# ---------------------------------------------------------------------------
# bank validation
# ---------------------------------------------------------------------------


def load_schema(schema_path=DEFAULT_SCHEMA_PATH):
    return _load_json(schema_path)


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
    corpora = vet.load_corpora(corpora_path)

    task_files = sorted(
        name for name in os.listdir(tasks_dir) if name.endswith(".json")
    )
    problems = []
    seen_ids = {}
    per_corpus = {}
    per_category = {}
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
        per_category[task.get("category")] = (
            per_category.get(task.get("category"), 0) + 1
        )

    summary = {
        "tasks": len(task_files),
        "per_corpus": per_corpus,
        "per_category": per_category,
    }
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
            f"edit task validation FAILED: {len(problems)} problem(s) across "
            f"{summary.get('tasks', 0)} task(s)"
        )
        return 1
    print(
        f"edit task validation passed: {summary['tasks']} task(s) "
        f"corpora={summary['per_corpus']} categories={summary['per_category']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except vet.forge.ForgeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
