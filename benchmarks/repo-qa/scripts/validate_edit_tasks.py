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
import fnmatch
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
DEFAULT_PATCHES_DIR = os.path.join(EDITS_ROOT, "patches")
DEFAULT_CORPORA_PATH = vet.DEFAULT_CORPORA_PATH
DEFAULT_CORPUS_ROOT = vet.DEFAULT_CORPUS_ROOT

TASK_SCHEMA_CONST = "edit_task_v1"
ANNOTATION_SCHEMA_CONST = "edit_annotation_v1"
ORACLE_PATCH_FORMAT = "edit_oracle_patch_v1"

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
# oracle patch (answer key) validation
# ---------------------------------------------------------------------------
#
# The scope check below deliberately uses fnmatch, NOT the runtime gate's
# custom glob translator (oracle_gate._glob_to_regex): per the
# bench-harness-oracle-independence rule the bank validator must not share its
# matcher with the gate it cross-checks, so a glob-translation bug in the gate
# cannot be mirrored here. fnmatch is STRICTER for `*` (it crosses `/`), which
# is the safe direction for a validator: it may reject a glob the gate would
# allow, never the reverse.


def _patch_scope_problems(task_id, files, blast):
    problems = []
    allow = blast.get("allow") or []
    max_files = blast.get("max_files")
    for rel in sorted(files):
        if not any(fnmatch.fnmatchcase(rel, glob) for glob in allow):
            problems.append(
                f"{task_id}: oracle patch path {rel!r} matches no "
                f"blast_radius.allow glob"
            )
    if isinstance(max_files, int) and len(files) > max_files:
        problems.append(
            f"{task_id}: oracle patch touches {len(files)} file(s); "
            f"blast_radius.max_files is {max_files}"
        )
    return problems


def load_oracle_patch(patches_dir, ref_path):
    """Load and structurally validate an oracle patch file. Raises Problem."""
    edits_root = os.path.dirname(os.path.abspath(patches_dir))
    abs_path = os.path.normpath(os.path.join(edits_root, ref_path))
    if not abs_path.startswith(os.path.abspath(patches_dir) + os.sep):
        raise Problem(f"oracle_patch path {ref_path!r} escapes the patches dir")
    if not os.path.isfile(abs_path):
        raise Problem(f"oracle patch file does not exist: {ref_path!r}")
    patch = _load_json(abs_path)
    if not isinstance(patch, dict):
        raise Problem(f"oracle patch {ref_path!r} is not a JSON object")
    if patch.get("schema") != ORACLE_PATCH_FORMAT:
        raise Problem(
            f"oracle patch {ref_path!r} schema {patch.get('schema')!r} != "
            f"{ORACLE_PATCH_FORMAT!r}"
        )
    files = patch.get("files")
    if not isinstance(files, dict) or not files:
        raise Problem(f"oracle patch {ref_path!r} has no files mapping")
    for rel, content in files.items():
        if not isinstance(rel, str) or not rel:
            raise Problem(f"oracle patch {ref_path!r} has a non-string path")
        norm = os.path.normpath(rel)
        if norm.startswith("..") or os.path.isabs(rel):
            raise Problem(
                f"oracle patch {ref_path!r} path {rel!r} escapes the tree"
            )
        if content is not None and not isinstance(content, str):
            raise Problem(
                f"oracle patch {ref_path!r} entry {rel!r} is neither a "
                f"string nor null"
            )
    return patch


def _patch_leak_problems(task_id, prompt, patch):
    """The answer key must never leak into the agent-visible prompt.

    Reuses the exploration leak linter's token matcher (normalize_tokens,
    _contiguous, _squash_hit) against two needle classes: every path the patch
    touches, and every substantial content line the patch writes.
    """
    problems = []
    prompt_tokens = leak_linter.normalize_tokens(prompt)

    def _hit(needle_raw):
        needle = leak_linter.normalize_tokens(str(needle_raw))
        return bool(needle) and (
            leak_linter._contiguous(needle, prompt_tokens)
            or leak_linter._squash_hit(needle, prompt_tokens)
        )

    for rel in sorted(patch["files"]):
        # The full path AND each multi-token segment of it: a prompt naming
        # just the basename (`record_crud.go`) or a distinctive directory
        # (`next-app-loader`) hands over the target as surely as the full
        # path does. Single-token segments (`apis`, `sklearn`, `src`) are
        # too common to discriminate and are skipped.
        needles = [rel] + [
            seg for seg in rel.split("/")
            if len(leak_linter.normalize_tokens(seg)) >= 2
        ]
        if any(_hit(needle) for needle in needles):
            problems.append(
                f"{task_id}: oracle patch leak: prompt names patch path "
                f"{leak_linter.redact(rel)}"
            )
    for rel, content in sorted(patch["files"].items()):
        if content is None:
            continue
        for line in content.splitlines():
            stripped = line.strip()
            # Short/common lines (braces, imports) cannot discriminate a leak;
            # only a substantial authored line is answer-key material.
            if len(leak_linter.normalize_tokens(stripped)) < 4:
                continue
            if _hit(stripped):
                problems.append(
                    f"{task_id}: oracle patch leak: prompt contains patch "
                    f"content line from {leak_linter.redact(rel)}"
                )
    return problems


# ---------------------------------------------------------------------------
# per-task validation
# ---------------------------------------------------------------------------


def validate_task(
    task, schema, corpora, annotations_dir, corpus_root, require_live,
    patches_dir=DEFAULT_PATCHES_DIR,
    require_answer_key=False,
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
    reference_version = spec.get("reference_forge_version")
    if reference_version is None:
        problems.append(
            f"{task_id}: corpora.json spec for {corpus_id!r} lacks "
            f"reference_forge_version; the bank's pinned forge_version is "
            f"unverifiable"
        )
    elif ref["forge_version"] != reference_version:
        problems.append(
            f"{task_id}: manifest_ref.forge_version {ref['forge_version']!r} != "
            f"reference_forge_version {reference_version!r} recorded for "
            f"{corpus_id} in corpora.json"
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

    # ---- oracle patch (answer key) ---------------------------------------
    # The answer-key fields are OPTIONAL until the per-corpus batch lands
    # (S3.2b iterates the bank without them); --require-answer-key is the
    # enforcement switch M5 flips on. A task that DOES declare them is always
    # fully checked -- optional never means unvalidated.
    if require_answer_key:
        for field in ("oracle_patch", "existing_suite"):
            if field not in task:
                problems.append(
                    f"{task_id}: answer key field {field!r} is missing "
                    f"(required by --require-answer-key)"
                )
    patch = None
    if "oracle_patch" in task:
        try:
            patch = load_oracle_patch(patches_dir, task["oracle_patch"]["path"])
        except Problem as err:
            problems.append(f"{task_id}: {err}")
            patch = None
    if patch is not None:
        if patch.get("task_id") != task_id:
            problems.append(
                f"{task_id}: oracle patch task_id {patch.get('task_id')!r} "
                f"does not match the task id"
            )
        problems.extend(
            _patch_scope_problems(task_id, patch["files"], task["blast_radius"])
        )
        problems.extend(
            _patch_leak_problems(task_id, task["prompt"], patch)
        )
        # The agent-visible task JSON must never name a patch target: a probe
        # argv names only the task id (via {edits_root}); the target path
        # lives in the probe body. Conformance tests (S3.2b) read task JSON,
        # so a relpath here would hand them the answer.
        for token in task["behavior"].get("command") or []:
            for rel in patch["files"]:
                if rel in token:
                    problems.append(
                        f"{task_id}: behavior.command names patch path "
                        f"{leak_linter.redact(rel)} (the target path belongs "
                        f"in the probe body, never in the task JSON)"
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
    patches_dir=DEFAULT_PATCHES_DIR,
    require_answer_key=False,
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
                task, schema, corpora, annotations_dir, corpus_root,
                require_live, patches_dir=patches_dir,
                require_answer_key=require_answer_key,
            )
        )
        per_corpus[task.get("corpus")] = per_corpus.get(task.get("corpus"), 0) + 1
        per_category[task.get("category")] = (
            per_category.get(task.get("category"), 0) + 1
        )

    # Category coverage, mirroring the exploration validator's bank gates: a
    # bank that skips (or invents) a convention family measures only the
    # families it happens to contain.
    if task_files and set(per_category) != REQUIRED_CATEGORIES:
        problems.append(
            "bank must cover every convention category "
            f"{sorted(REQUIRED_CATEGORIES)}; got {sorted(per_category, key=str)}"
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
    parser.add_argument("--patches-dir", default=DEFAULT_PATCHES_DIR)
    parser.add_argument(
        "--require-live",
        action="store_true",
        help="fail if the forged corpus is absent instead of skipping live checks",
    )
    parser.add_argument(
        "--require-answer-key",
        action="store_true",
        help="fail on any task missing the oracle_patch/existing_suite "
        "answer-key fields (they are optional until the per-corpus batch lands)",
    )
    args = parser.parse_args(argv)

    problems, summary = validate_bank(
        tasks_dir=args.tasks_dir,
        annotations_dir=args.annotations_dir,
        schema_path=args.schema,
        corpora_path=args.corpora,
        corpus_root=args.corpus_root,
        require_live=args.require_live,
        patches_dir=args.patches_dir,
        require_answer_key=args.require_answer_key,
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
