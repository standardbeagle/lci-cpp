"""Locate a forged corpus, verify its integrity, and materialise a clean checkout.

Every run gets a throwaway copy of the mutated tree so the agent can never
pollute the canonical corpus and repeated runs each start clean. Before copying,
the located tree is hashed and compared against the hash the forge pinned in the
manifest: a mismatch means the corpus drifted from the artefact the task's
answer key was verified against, which would silently poison the result, so it
is a hard, loud rejection (`TreeHashMismatch`) -- never a fallback.

Reuses the forge (`exploration_corpus_forge`) for `tree_hash` and its on-disk
layout convention rather than reimplementing either.
"""

import json
import os
import shutil

import exploration_corpus_forge as forge


class CorpusError(Exception):
    """Base class for corpus-preparation failures (config-class errors)."""


class MissingCorpus(CorpusError):
    """The referenced forged corpus was never generated / is not on disk."""


class TreeHashMismatch(CorpusError):
    """The forged tree's content no longer matches the manifest's pinned hash."""


class EscapingSymlink(CorpusError):
    """A forged tree symlink resolves outside the corpus tree."""


def reject_escaping_symlinks(tree_dir):
    """Reject links that would grant a checkout access to host files."""
    root = os.path.realpath(tree_dir)
    for walk_root, dirs, files in os.walk(tree_dir, followlinks=False):
        for name in dirs + files:
            path = os.path.join(walk_root, name)
            if not os.path.islink(path):
                continue
            if os.path.isabs(os.readlink(path)):
                rel = os.path.relpath(path, tree_dir)
                raise EscapingSymlink(
                    f"forged corpus symlink {rel!r} has an absolute target"
                )
            resolved = os.path.realpath(path)
            if resolved != root and not resolved.startswith(root + os.sep):
                rel = os.path.relpath(path, tree_dir)
                raise EscapingSymlink(
                    f"forged corpus symlink {rel!r} resolves outside its tree"
                )


def locate_forged_corpus(corpus_root, corpus_id, seed):
    """Return (manifest, tree_dir) for a forged corpus. Raise MissingCorpus if
    the forge has not produced it under the standard layout."""
    corpus_dir = os.path.join(corpus_root, corpus_id, f"seed-{seed}")
    manifest_path = os.path.join(corpus_dir, "manifest.json")
    tree_dir = os.path.join(corpus_dir, "tree")
    if not (os.path.isfile(manifest_path) and os.path.isdir(tree_dir)):
        raise MissingCorpus(
            f"no forged corpus for {corpus_id} seed {seed} under {corpus_root}; "
            f"run exploration_corpus_forge.py forge --corpus {corpus_id} "
            f"--seed {seed}"
        )
    with open(manifest_path, encoding="utf-8") as handle:
        return json.load(handle), tree_dir


def verify_tree_hash(tree_dir, manifest):
    """Fail loud if the on-disk tree drifted from the manifest's pinned hash."""
    pinned = manifest.get("tree_hash")
    actual = forge.tree_hash(tree_dir)
    if actual != pinned:
        raise TreeHashMismatch(
            f"tree hash {actual} != manifest tree_hash {pinned} for "
            f"corpus {manifest.get('corpus_id')} seed {manifest.get('seed')}; "
            f"the forged corpus drifted from the pinned artefact"
        )
    return actual


def prepare_checkout(corpus_root, manifest_ref, dest):
    """Locate + integrity-check the forged corpus, then copy its tree to `dest`.

    Returns (manifest, checkout_dir). Raises MissingCorpus / TreeHashMismatch
    (both config-class) before ever copying a drifted or absent tree.
    """
    manifest, tree_dir = locate_forged_corpus(
        corpus_root, manifest_ref["corpus_id"], manifest_ref["seed"]
    )
    if manifest.get("source_commit") != manifest_ref["source_commit"]:
        raise TreeHashMismatch(
            f"forged manifest source_commit {manifest.get('source_commit')} != "
            f"task manifest_ref.source_commit {manifest_ref['source_commit']}"
        )
    verify_tree_hash(tree_dir, manifest)
    reject_escaping_symlinks(tree_dir)
    if os.path.exists(dest):
        shutil.rmtree(dest)
    shutil.copytree(tree_dir, dest, symlinks=True)
    # Re-check the actual checkout: this catches copy races and ensures link
    # interpretation is safe at the destination, not merely at the source.
    try:
        reject_escaping_symlinks(dest)
    except EscapingSymlink:
        shutil.rmtree(dest)
        raise
    return manifest, dest
