"""Edit-mode run records: status taxonomy + idempotent resume, atop the S1 log.

Reuses the exploration record log verbatim (append-safe JSONL, torn-tail
tolerance, fail-loud on corrupt interior lines, the ``task_id::arm::seed`` run
key) and only adds the EDIT outcome taxonomy. Every terminal condition maps to
exactly one machine-readable status -- there is no silent omission (criterion 4).

A COMPLETED status is a deterministic property of (task, arm, corpus, toolset,
agent patch): the agent produced a patch and the gates judged it, or it violated
isolation, or it produced no usable patch. Resume never re-runs those. The
transient/infra classes (timeout, agent failure, config error) are retryable.
"""

import edit_paths  # noqa: F401  (sys.path bootstrap)

from runner import record as base_record

# --- edit outcome taxonomy ---
STATUS_PASSED = "edit_passed"                # patch produced; ALL gates green
STATUS_GATE_FAILED = "edit_gate_failed"      # patch produced; a gate rejected it
STATUS_TOOL_VIOLATION = "edit_tool_violation"  # isolation gate rejected a call
STATUS_INVALID_PATCH = "edit_invalid_patch"  # empty / unappliable patch
STATUS_TIMEOUT = "edit_timeout"              # adapter hit the wall-clock deadline
STATUS_AGENT_FAILURE = "edit_agent_failure"  # provider/CLI failure or empty answer
STATUS_CONFIG_ERROR = "edit_config_error"    # corpus missing / drift / bad arm

ALL_STATUSES = frozenset(
    {
        STATUS_PASSED,
        STATUS_GATE_FAILED,
        STATUS_TOOL_VIOLATION,
        STATUS_INVALID_PATCH,
        STATUS_TIMEOUT,
        STATUS_AGENT_FAILURE,
        STATUS_CONFIG_ERROR,
    }
)

# Terminal: re-running cannot change the outcome, so resume skips them.
COMPLETED_STATUSES = frozenset(
    {
        STATUS_PASSED,
        STATUS_GATE_FAILED,
        STATUS_TOOL_VIOLATION,
        STATUS_INVALID_PATCH,
    }
)
RETRYABLE_STATUSES = ALL_STATUSES - COMPLETED_STATUSES

# Reused log primitives (one source of truth with the exploration runner).
run_key = base_record.run_key
append_record = base_record.append_record
load_records = base_record.load_records
completed_keys_of = base_record.completed_keys  # exploration's; not edit-aware
CorruptRecords = base_record.CorruptRecords


def find_completed(records_path, key, digest):
    """The latest COMPLETED edit record for (run_key, task_digest), or None.

    Keying on the task digest as well as the run key means a resumed run that
    changed the task content (different oracle key) is NOT skipped -- it is a
    different cell -- while an unchanged completed cell is never re-run.
    """
    match = None
    for rec in load_records(records_path):
        if (
            rec.get("run_key") == key
            and rec.get("task_digest") == digest
            and rec.get("status") in COMPLETED_STATUSES
        ):
            match = rec
    return dict(match) if match else None
