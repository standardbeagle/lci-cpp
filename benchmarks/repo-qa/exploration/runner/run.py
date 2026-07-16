"""Orchestrate one task/arm run: checkout -> agent -> isolation gate -> record.

`run_task` is the single primitive. It is resume-idempotent (skips a key already
recorded with a terminal status), fails loud on a drifted/absent corpus (recorded
as a distinct config_error, agent never invoked), enforces tool isolation on the
adapter's emitted calls, and appends exactly one record capturing every mandated
field. The adapter is injected, so the same primitive serves the fake-agent unit
tests and the live smoke command.
"""

import json
import os
from dataclasses import dataclass
from datetime import datetime, timezone

from runner import corpus, gate, record, toolsets
from task_digest import task_digest


@dataclass(frozen=True)
class BaseConfig:
    """The configuration shared byte-for-byte across both arms."""

    model: str
    system_prompt: str
    timeout_seconds: int


def _now():
    return datetime.now(timezone.utc)


def _safe_name(run_key):
    return run_key.replace("::", "__").replace("/", "_")


def _write_transcript(work_root, run_key, transcript):
    directory = os.path.join(work_root, "transcripts")
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, _safe_name(run_key) + ".json")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(transcript, handle, sort_keys=True)
    os.replace(tmp, path)
    return path


def _base_record(task, arm, base, seed, key):
    ref = task["manifest_ref"]
    return {
        "run_key": key,
        "task_id": task["id"],
        "corpus_id": ref["corpus_id"],
        "source_commit": ref["source_commit"],
        "forge_version": ref["forge_version"],
        "seed": seed,
        "arm": arm,
        "model": base.model,
        "task_digest": task_digest(task),
        "effective_allowlist": list(toolsets.arm_allowlist(arm)),
    }


def run_task(task, arm, adapter, base, *, corpus_root, records_path, work_root):
    """Run one (task, arm). Returns the persisted record dict; on resume-skip
    returns the existing record annotated with skipped=True (adapter untouched)."""
    seed = task["manifest_ref"]["seed"]
    key = record.run_key(task["id"], arm, seed)

    digest = task_digest(task)
    existing = _find_record(records_path, key, digest)
    if existing:
        existing["skipped"] = True
        return existing

    rec = _base_record(task, arm, base, seed, key)

    # Materialise + integrity-check the clean checkout. A drifted or missing
    # corpus is rejected loud, recorded as config_error, and the agent is never
    # invoked against a poisoned tree.
    checkout = os.path.join(work_root, "checkouts", _safe_name(key))
    try:
        manifest, checkout_dir = corpus.prepare_checkout(
            corpus_root, task["manifest_ref"], checkout
        )
    except corpus.CorpusError as error:
        return _finish(
            records_path, rec,
            status=record.STATUS_CONFIG_ERROR,
            manifest_id=None,
            started=None, ended=None,
            final_answer=None, tool_calls=(), token_usage=None,
            transcript_ref=None, error=str(error), violations=[],
        )

    rec["manifest_id"] = manifest.get("tree_hash")

    request = toolsets.build_request(base, arm, checkout_dir, task["prompt"])
    started = _now()
    result = adapter.run(request)
    ended = _now()

    transcript_ref = _write_transcript(work_root, key, result.transcript)
    tool_calls = [{"name": c.name, "arguments": c.arguments} for c in result.tool_calls]
    token_usage = {"input": result.input_tokens, "output": result.output_tokens}

    if result.status_hint == "timeout":
        status, violations, error = record.STATUS_TIMEOUT, [], "timeout"
    elif result.status_hint == "provider_error":
        status, violations, error = record.STATUS_PROVIDER_ERROR, [], "provider_error"
    else:
        violations = gate.enforce(result.tool_calls, request.allowed_tools, checkout_dir)
        if violations:
            status, error = record.STATUS_TOOL_VIOLATION, "tool_isolation_violation"
        elif not result.final_answer:
            status, error = record.STATUS_PROVIDER_ERROR, "empty_answer"
        else:
            status, error = record.STATUS_ANSWERED, None

    return _finish(
        records_path, rec,
        status=status,
        manifest_id=rec["manifest_id"],
        started=started, ended=ended,
        final_answer=result.final_answer,
        tool_calls=tool_calls,
        token_usage=token_usage,
        transcript_ref=transcript_ref,
        error=error,
        violations=violations,
    )


def _finish(records_path, rec, *, status, manifest_id, started, ended, final_answer,
            tool_calls, token_usage, transcript_ref, error, violations):
    rec["manifest_id"] = manifest_id
    rec["status"] = status
    rec["started_at"] = started.isoformat() if started else None
    rec["ended_at"] = ended.isoformat() if ended else None
    rec["duration_seconds"] = (ended - started).total_seconds() if (started and ended) else 0.0
    rec["final_answer"] = final_answer
    rec["tool_calls"] = tool_calls
    rec["token_usage"] = token_usage
    rec["transcript_ref"] = transcript_ref
    rec["error"] = error
    rec["violations"] = violations
    record.append_record(records_path, rec)
    return rec


def _find_record(records_path, key, digest):
    match = None
    for rec in record.load_records(records_path):
        if (
            rec.get("run_key") == key
            and rec.get("task_digest") == digest
            and rec.get("status") in record.COMPLETED_STATUSES
        ):
            match = rec
    return dict(match) if match else None


def run_task_both_arms(task, adapter_factory, base, *, corpus_root, records_path, work_root):
    """Run both arms of one task. `adapter_factory(arm)` yields the adapter for
    that arm, so the caller controls fake vs. real without run_task knowing."""
    return {
        arm: run_task(
            task, arm, adapter_factory(arm), base,
            corpus_root=corpus_root, records_path=records_path, work_root=work_root,
        )
        for arm in (toolsets.TREATMENT, toolsets.BASELINE)
    }
