"""Stable identity for the exact exploration task used by a run."""

import hashlib
import json


def task_digest(task):
    """Return a canonical SHA-256 digest of all task and answer-key content."""
    encoded = json.dumps(
        task, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()
