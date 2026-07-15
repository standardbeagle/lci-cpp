#!/usr/bin/env python3
"""Lint stage-1 exploration prompts for leaked answer-key material.

An exploration task's FINAL adjudicated evidence set (see
validate_exploration_tasks.py) is the oracle: the paths, file names, and target
symbols an agent is supposed to DISCOVER. If any of that material appears in the
task PROMPT, the prompt hands the tool the answer and the exploration measure is
void. This linter fails NONZERO when it does.

Design contract:

  * Forbidden material is DERIVED, per task, from that task's own adjudicated
    evidence set plus its referenced S1 translation manifest -- never a
    hand-written denylist that can drift from the oracle. The only static table
    here is GENERIC_SEGMENTS, an allowlist of architecture words (server,
    client, tools, ...) that suppresses false positives; it never adds a
    forbidden term.
  * Only the PROMPT is scanned. Author-only keys (rubric, adjudication,
    evidence) stay available to downstream scoring untouched.
  * Matching is done on normalized token streams, so direct AND trivially
    reformatted leakage is caught: path-separator changes and common
    snake/kebab/camel/case variants all normalize to the same token list. A
    fully concatenated identifier (loadmanifest) is caught by a squashed-form
    substring check.
  * Structural/anchor validation (validate_exploration_tasks.py) runs FIRST and
    short-circuits, so a malformed or stale oracle can never silently pass the
    leak gate.
  * Matches are reported REDACTED (first/last char + length). The full oracle
    string is never printed, so CI logs never carry the answer key.

The S1 manifest reveals the decoy/original-path translation case: an anchor path
is a MUTATED path, but naming the ORIGINAL pre-mutation path (mapped through the
manifest's path_map, or a decoy's derived_from donor) leaks the same answer. The
manifest lives only in the forged tree, which is never vendored; when it is
absent, translation checks are SKIPPED (never silently passed), exactly as the
validator skips live-anchor checks.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import validate_exploration_tasks as validator

# Architecture/directory words that are broad structure, not the target path.
# Suppresses false positives on single generic segments/stems; NEVER a source of
# forbidden terms (the oracle is; see module docstring).
GENERIC_SEGMENTS = {
    "app", "apps", "client", "common", "config", "core", "handler",
    "handlers", "index", "internal", "lib", "main", "model", "models", "pkg",
    "server", "service", "services", "src", "test", "tests", "tool", "tools",
    "types", "util", "utils",
}

# camelCase / PascalCase / ALLCAPS / digit-run aware word splitter. Separators
# (/ \ . - _ space) are simply not matched, so they act as token boundaries.
_WORD = re.compile(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|[0-9]+")

# Minimum squashed length for the concatenated-form substring check, to avoid
# matching a short generic fragment inside an unrelated word.
_MIN_SQUASH = 6
# Minimum token count for a path-segment subsequence to count as distinctive.
_MIN_SEGMENT_TOKENS = 3


def normalize_tokens(text):
    """Lowercase word-token list; separator/case/camel variants collapse alike."""
    return [match.lower() for match in _WORD.findall(text)]


def redact(raw):
    """Short, answer-key-safe form of a matched term: first/last char + length."""
    if len(raw) <= 4:
        return f"***[{len(raw)}]"
    return f"{raw[0]}…{raw[-1]}[{len(raw)}]"


def _contiguous(needle, hay):
    """True if the needle token list occurs as a contiguous run in hay tokens."""
    size = len(needle)
    if size == 0:
        return False
    return any(hay[i:i + size] == needle for i in range(len(hay) - size + 1))


def _squash_hit(needle_tokens, hay_tokens):
    """Catch a fully concatenated reformat (loadManifest -> "loadmanifest")."""
    squashed = "".join(needle_tokens)
    if len(squashed) < _MIN_SQUASH:
        return False
    return any(squashed in token for token in hay_tokens if len(token) >= len(squashed))


def _leaf(path):
    return re.split(r"[\\/]", path)[-1]


def _iter_terms(task, manifest):
    """Yield (category, token_list, raw) forbidden terms derived from the oracle.

    Nothing is hand-written here; every term comes from the task's evidence or
    the S1 manifest. GENERIC_SEGMENTS only suppresses emission, never adds.
    """
    evidence = task["evidence"]
    oracle_paths = {anchor["path"] for anchor in evidence}

    for anchor in evidence:
        path = anchor["path"]
        yield ("oracle-path", normalize_tokens(path), path)

        leaf = _leaf(path)
        yield ("basename", normalize_tokens(leaf), leaf)
        stem = re.split(r"\.", leaf)[0].lstrip("_")
        stem_tokens = normalize_tokens(stem)
        if len(stem_tokens) >= 2 or (
            len(stem_tokens) == 1 and stem_tokens[0] not in GENERIC_SEGMENTS
        ):
            yield ("basename", stem_tokens, stem)

        segments = [seg for seg in re.split(r"[\\/]", path) if seg]
        # The trailing file extension is generic; a prompt naming
        # "server/load-manifest" omits ".ts". Match on the stem-stripped leaf.
        seg_terms = segments[:]
        if seg_terms:
            seg_terms[-1] = re.split(r"\.", seg_terms[-1])[0].lstrip("_")
        for i in range(len(seg_terms)):
            for j in range(i + 2, len(seg_terms) + 1):
                tokens = normalize_tokens("/".join(seg_terms[i:j]))
                if len(tokens) >= _MIN_SEGMENT_TOKENS:
                    yield ("path-segments", tokens, "/".join(segments[i:j]))

        for identifier in anchor["target_identifiers"]:
            yield ("target-identifier", normalize_tokens(identifier), identifier)

    if not manifest:
        return

    # An oracle anchor path is a MUTATED path; naming the original pre-mutation
    # path (path_map) or a decoy's donor (derived_from) leaks the same answer.
    reverse = {}
    for original, mutated in manifest.get("path_map", {}).items():
        reverse.setdefault(mutated, []).append(original)
    for oracle_path in oracle_paths:
        for original in reverse.get(oracle_path, []):
            if original != oracle_path:
                yield ("translated-path", normalize_tokens(original), original)
    for decoy in manifest.get("decoys", []):
        donor = decoy.get("derived_from")
        if decoy.get("path") in oracle_paths and donor:
            yield ("translated-path", normalize_tokens(donor), donor)


def find_leaks(task, manifest=None):
    """Return sorted [(task_id, category, redacted)] leaks in this task's prompt."""
    prompt_tokens = normalize_tokens(task["prompt"])
    task_id = task["id"]
    leaks = []
    seen = set()
    for category, needle, raw in _iter_terms(task, manifest):
        if not needle:
            continue
        if _contiguous(needle, prompt_tokens) or _squash_hit(needle, prompt_tokens):
            key = (category, redact(raw))
            if key in seen:
                continue
            seen.add(key)
            leaks.append((task_id, category, redact(raw)))
    leaks.sort()
    return leaks


def lint_bank(
    tasks_dir=validator.DEFAULT_TASKS_DIR,
    annotations_dir=validator.DEFAULT_ANNOTATIONS_DIR,
    schema_path=validator.DEFAULT_SCHEMA_PATH,
    corpora_path=validator.DEFAULT_CORPORA_PATH,
    corpus_root=validator.DEFAULT_CORPUS_ROOT,
    require_live=False,
):
    """Validate the bank structurally, THEN leak-scan every prompt.

    Returns (validator_problems, summary, leaks). Validator problems
    short-circuit: a malformed oracle is never leak-scanned, so it cannot
    silently pass.
    """
    problems, summary = validator.validate_bank(
        tasks_dir=tasks_dir,
        annotations_dir=annotations_dir,
        schema_path=schema_path,
        corpora_path=corpora_path,
        corpus_root=corpus_root,
        require_live=require_live,
    )
    if problems:
        return problems, summary, []

    leaks = []
    for name in sorted(os.listdir(tasks_dir)):
        if not name.endswith(".json"):
            continue
        task = validator._load_json(os.path.join(tasks_dir, name))
        ref = task["manifest_ref"]
        manifest, _tree = validator.locate_manifest(
            corpus_root, task["corpus"], ref["seed"]
        )
        leaks.extend(find_leaks(task, manifest))
    leaks.sort()
    return problems, summary, leaks


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tasks-dir", default=validator.DEFAULT_TASKS_DIR)
    parser.add_argument(
        "--annotations-dir", default=validator.DEFAULT_ANNOTATIONS_DIR
    )
    parser.add_argument("--schema", default=validator.DEFAULT_SCHEMA_PATH)
    parser.add_argument("--corpora", default=validator.DEFAULT_CORPORA_PATH)
    parser.add_argument("--corpus-root", default=validator.DEFAULT_CORPUS_ROOT)
    parser.add_argument(
        "--require-live",
        action="store_true",
        help="fail if a forged corpus is absent instead of skipping translation",
    )
    args = parser.parse_args(argv)

    problems, summary, leaks = lint_bank(
        tasks_dir=args.tasks_dir,
        annotations_dir=args.annotations_dir,
        schema_path=args.schema,
        corpora_path=args.corpora,
        corpus_root=args.corpus_root,
        require_live=args.require_live,
    )

    if problems:
        for problem in sorted(problems):
            print(f"INVALID {problem}")
        print(
            f"structural validation FAILED before leak scan: "
            f"{len(problems)} problem(s)"
        )
        return 1

    if leaks:
        for task_id, category, redacted in leaks:
            print(f"LEAK {task_id}: {category} match {redacted}")
        print(
            f"exploration prompt leak check FAILED: {len(leaks)} leak(s) across "
            f"{summary.get('tasks', 0)} task(s)"
        )
        return 1

    print(
        f"exploration prompt leak check passed: {summary.get('tasks', 0)} "
        f"task(s) {summary.get('per_corpus', {})}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except validator.forge.ForgeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
