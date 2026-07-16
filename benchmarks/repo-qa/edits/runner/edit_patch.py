"""Capture an agent-produced patch by diffing the pristine tree vs the checkout.

The agent edits its throwaway checkout in place (via Edit/Write, cwd-rooted
there). The patch is recovered by comparing that mutated checkout against the
pristine forged tree it was copied from, yielding a ``{relpath: new_text or
None}`` map -- the exact shape ``oracle_gate.apply_patch`` consumes, so the
captured patch is re-materialised cleanly by the behaviour gate rather than
scored in place. ``None`` marks a deletion.

No disk mutation happens here; this is pure observation. Symlinks are ignored
(the isolation gate already rejects link escapes) so only regular-file content
is diffed.
"""

import hashlib
import os


def _list_files(root):
    """Relative paths of every regular file under ``root`` (symlinks skipped)."""
    found = set()
    for base, _dirs, files in os.walk(root, followlinks=False):
        for name in files:
            path = os.path.join(base, name)
            if os.path.islink(path):
                continue
            found.add(os.path.relpath(path, root))
    return found


def _read_text(path):
    # surrogateescape round-trips any byte sequence, so a non-UTF-8 source file
    # is captured and re-written faithfully rather than raising mid-capture.
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as handle:
        return handle.read()


def capture_patch(pristine_dir, checkout_dir):
    """Return the agent's patch: changed/added files -> new text, deletions -> None.

    Deterministic (keys inserted in sorted order). Unchanged files are omitted,
    so an agent that touched nothing yields an empty patch (which the runner
    classifies as an invalid/empty patch rather than a silent pass).
    """
    pristine = _list_files(pristine_dir)
    mutated = _list_files(checkout_dir)
    patch = {}
    for rel in sorted(mutated):
        new = _read_text(os.path.join(checkout_dir, rel))
        if rel not in pristine:
            patch[rel] = new
        elif _read_text(os.path.join(pristine_dir, rel)) != new:
            patch[rel] = new
    for rel in sorted(pristine - mutated):
        patch[rel] = None
    return patch


def patch_hash(patch):
    """Stable content hash of a patch map. Order-independent (keys sorted),
    binary-safe (no JSON), and distinguishes a deletion from an empty write."""
    digest = hashlib.sha256()
    for rel in sorted(patch):
        digest.update(rel.encode("utf-8", "surrogateescape"))
        digest.update(b"\x00")
        content = patch[rel]
        if content is None:
            digest.update(b"\x00DELETED\x00")
        else:
            digest.update(content.encode("utf-8", "surrogateescape"))
        digest.update(b"\x00\x00")
    return "sha256:" + digest.hexdigest()


def agent_visible_files(checkout_dir):
    """Sorted relative paths the agent can see in its checkout. Used by the
    oracle-isolation proof: this set must equal the pristine corpus tree's, i.e.
    the harness injected no oracle test/anchor file into the tool-visible tree."""
    return sorted(_list_files(checkout_dir))
