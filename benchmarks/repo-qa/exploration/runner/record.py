"""Per-run records: status taxonomy, append-safe persistence, idempotent resume.

Records are a JSONL append log -- one self-contained JSON object per line. A run
is written exactly once, appended, and fsync'd, so an interrupted batch leaves at
most a torn final line (no trailing newline), which `load_records` drops while
still failing loud on a corrupt interior line. Resume keys on
(task_id, arm, seed): a run whose recorded status is terminal is never redone,
while a transient failure (timeout / provider / config) is eligible for retry.
"""

import json
import os

# --- status taxonomy: valid answers vs. the distinct failure classes ---
STATUS_ANSWERED = "answered"          # agent produced an answer; all calls allowed
STATUS_TIMEOUT = "timeout"            # adapter hit the wall-clock deadline
STATUS_PROVIDER_ERROR = "provider_error"  # provider/CLI failure or empty answer
STATUS_TOOL_VIOLATION = "tool_violation"  # isolation gate rejected a tool call
STATUS_CONFIG_ERROR = "config_error"  # corpus missing / tree-hash drift / bad arm

ALL_STATUSES = frozenset(
    {
        STATUS_ANSWERED,
        STATUS_TIMEOUT,
        STATUS_PROVIDER_ERROR,
        STATUS_TOOL_VIOLATION,
        STATUS_CONFIG_ERROR,
    }
)

# Terminal outcomes are a deterministic property of (task, arm, corpus, toolset):
# re-running cannot change them, so resume skips them. Everything else is a
# transient/infra failure worth retrying on the next pass.
COMPLETED_STATUSES = frozenset({STATUS_ANSWERED, STATUS_TOOL_VIOLATION})
RETRYABLE_STATUSES = ALL_STATUSES - COMPLETED_STATUSES


class CorruptRecords(Exception):
    """A non-final record line failed to parse -- real corruption, fail loud."""


def run_key(task_id, arm, seed):
    return f"{task_id}::{arm}::seed-{seed}"


def append_record(records_path, rec):
    """Append one record as a newline-terminated JSON line, durably.

    A single write of a fully-formed line keeps the log append-safe: a crash
    either lands the whole line or leaves a newline-less tail that the reader
    discards. fsync guarantees the completed line survives the interruption.
    """
    parent = os.path.dirname(records_path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    line = json.dumps(rec, sort_keys=True) + "\n"
    with open(records_path, "a", encoding="utf-8") as handle:
        handle.write(line)
        handle.flush()
        os.fsync(handle.fileno())


def load_records(records_path):
    """Parse the JSONL log. Tolerates a torn final line (interrupted write);
    raises CorruptRecords on any malformed interior line."""
    if not os.path.isfile(records_path):
        return []
    with open(records_path, encoding="utf-8") as handle:
        content = handle.read()
    if not content:
        return []
    lines = content.split("\n")
    # split() always yields a trailing element after the last "\n": "" for a
    # complete log, or the newline-less partial for a torn tail. Either way the
    # complete lines are everything before it, so dropping the last element
    # discards an interrupted write without touching any finished record.
    body = lines[:-1]
    records = []
    for index, line in enumerate(body):
        if line == "":
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError as exc:
            raise CorruptRecords(
                f"{records_path}:{index + 1}: malformed record line: {exc}"
            ) from exc
    return records


def completed_keys(records_path):
    """Run keys whose latest recorded status is terminal (skip on resume)."""
    latest = {}
    for rec in load_records(records_path):
        key = rec.get("run_key")
        if key is not None:
            latest[key] = rec.get("status")
    return {key for key, status in latest.items() if status in COMPLETED_STATUSES}
