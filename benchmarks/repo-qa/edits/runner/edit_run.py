"""Orchestrate one edit-mode task/arm run:

    verified checkout -> agent edits (isolated worktree) -> isolation gate ->
    capture patch -> behaviour + blast-radius + convention gates -> record.

``run_edit_task`` is the single primitive and mirrors the exploration
``run_task``: resume-idempotent, fails loud on a drifted/absent corpus (recorded
as ``config_error``, agent never invoked), enforces tool isolation on the emitted
calls, and appends exactly one provenance-complete record. The isolated checkout
is a throwaway copy of the pinned tree, cleaned on BOTH success and failure; the
source corpus is never mutated. The behaviour + blast-radius gate (Stage-3
``oracle_gate``) and the convention gate (Stage-3 ``conformance_gate``) are CALLED,
never reimplemented, and always after the agent patch is captured -- and the
agent never sees any oracle material (its prompt is the goal prompt only; its
tree is the pristine corpus).

The adapter is injected, so the same primitive serves the fake-agent unit
tests/smoke and the live CLI.
"""

import os
import shutil
from datetime import datetime, timezone

import edit_paths  # noqa: F401  (sys.path bootstrap)

import conformance_gate
import edit_patch
import edit_record
import edit_toolsets
import oracle_gate
from runner import corpus, gate
from runner.run import BaseConfig  # reused: shared model/system-prompt/timeout
from task_digest import task_digest

__all__ = [
    "BaseConfig",
    "run_edit_task",
    "run_edit_task_both_arms",
    "run_edit_bank",
]

# Stable sub-reason codes for the fail-loud non-gate terminal classes.
REASON_EMPTY_PATCH = "EMPTY_PATCH"
REASON_PATCH_UNAPPLIABLE = "PATCH_UNAPPLIABLE"
REASON_TIMEOUT = "TIMEOUT"
REASON_AGENT_ERROR = "AGENT_ERROR"
REASON_EMPTY_ANSWER = "EMPTY_ANSWER"
REASON_TOOL_ISOLATION = "TOOL_ISOLATION"
REASON_GATE_FAILED = "GATE_FAILED"
REASON_ALL_GREEN = "ALL_GREEN"


def _now():
    return datetime.now(timezone.utc)


def _safe_name(run_key):
    return run_key.replace("::", "__").replace("/", "_")


def _write_transcript(work_root, run_key, transcript):
    directory = os.path.join(work_root, "transcripts")
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, _safe_name(run_key) + ".json")
    tmp = path + ".tmp"
    import json

    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(transcript, handle, sort_keys=True)
    os.replace(tmp, path)
    return path


def _base_record(task, arm, base, seed, key):
    """Provenance skeleton (criterion 1). Outcome fields are filled by _finish."""
    ref = task["manifest_ref"]
    return {
        "mode": "edit",
        "run_key": key,
        "task_id": task["id"],
        "corpus_id": ref["corpus_id"],
        # pinned source commit + (later) tree hash
        "source_commit": ref["source_commit"],
        "forge_version": ref["forge_version"],
        "seed": seed,
        # mutation manifest reference
        "manifest_ref": {
            "corpus_id": ref["corpus_id"],
            "source_commit": ref["source_commit"],
            "seed": seed,
            "forge_version": ref["forge_version"],
        },
        # task + schema + gate versions
        "task_digest": task_digest(task),
        "task_schema": task.get("schema"),
        "gate_versions": {
            "oracle": oracle_gate.AGGREGATE_SCHEMA,
            "conformance": conformance_gate.OUTCOME_SCHEMA,
        },
        # model + arm/tool allowlist
        "model": base.model,
        "arm": arm,
        "effective_allowlist": list(edit_toolsets.arm_allowlist(arm)),
    }


def _finish(records_path, rec, **fields):
    started = fields.pop("started", None)
    ended = fields.pop("ended", None)
    rec["started_at"] = started.isoformat() if started else None
    rec["ended_at"] = ended.isoformat() if ended else None
    rec["duration_seconds"] = (
        (ended - started).total_seconds() if (started and ended) else 0.0
    )
    # Explicit machine-readable outcome fields; absent gates recorded as None,
    # never silently dropped (criterion 4).
    rec.setdefault("manifest_id", None)
    rec.setdefault("agent_visible_files", None)
    rec.setdefault("final_answer", None)
    rec.setdefault("tool_calls", [])
    rec.setdefault("token_usage", None)
    rec.setdefault("transcript_ref", None)
    rec.setdefault("violations", [])
    rec.setdefault("patch_hash", None)
    rec.setdefault("patch_files", None)
    rec.setdefault("gates", None)
    rec.setdefault("reason", None)
    rec.setdefault("error", None)
    rec.update(fields)
    edit_record.append_record(records_path, rec)
    return rec


def run_edit_task(
    task,
    arm,
    adapter,
    base,
    *,
    corpus_root,
    records_path,
    work_root,
    behavior_command=None,
    existing_suite_command=None,
    impacted_symbols=(),
    lci=None,
    gate_timeout=oracle_gate.DEFAULT_TIMEOUT,
    workspace_root=None,
):
    """Run one (task, arm). Returns the persisted record; on resume-skip returns
    the existing completed record annotated ``skipped=True`` (adapter untouched)."""
    seed = task["manifest_ref"]["seed"]
    key = edit_record.run_key(task["id"], arm, seed)
    digest = task_digest(task)

    existing = edit_record.find_completed(records_path, key, digest)
    if existing:
        existing["skipped"] = True
        return existing

    rec = _base_record(task, arm, base, seed, key)

    # Verified, throwaway checkout of the PINNED tree. A drifted/absent corpus is
    # rejected loud, recorded as config_error, and the agent is never invoked.
    try:
        manifest, tree_dir = corpus.locate_forged_corpus(
            corpus_root, task["manifest_ref"]["corpus_id"], seed
        )
        _, checkout = corpus.prepare_checkout(
            corpus_root,
            task["manifest_ref"],
            os.path.join(work_root, "checkouts", _safe_name(key)),
        )
    except corpus.CorpusError as error:
        return _finish(
            records_path, rec,
            status=edit_record.STATUS_CONFIG_ERROR,
            error=str(error),
        )

    rec["manifest_id"] = manifest.get("tree_hash")
    # Oracle-isolation proof surface: exactly the pristine corpus files, nothing
    # the harness injected (criterion 3). Captured before the agent mutates it.
    rec["agent_visible_files"] = edit_patch.agent_visible_files(checkout)

    try:
        request = edit_toolsets.build_request(base, arm, checkout, task["prompt"])
        started = _now()
        result = adapter.run(request)
        ended = _now()

        transcript_ref = _write_transcript(work_root, key, result.transcript)
        tool_calls = [
            {"name": c.name, "arguments": c.arguments} for c in result.tool_calls
        ]
        token_usage = {"input": result.input_tokens, "output": result.output_tokens}
        common = dict(
            started=started, ended=ended,
            transcript_ref=transcript_ref,
            tool_calls=tool_calls, token_usage=token_usage,
            final_answer=result.final_answer,
        )

        if result.status_hint == "timeout":
            return _finish(
                records_path, rec, status=edit_record.STATUS_TIMEOUT,
                reason=REASON_TIMEOUT, error="timeout", **common,
            )
        if result.status_hint == "provider_error":
            return _finish(
                records_path, rec, status=edit_record.STATUS_AGENT_FAILURE,
                reason=REASON_AGENT_ERROR, error="provider_error", **common,
            )

        violations = gate.enforce(result.tool_calls, request.allowed_tools, checkout)
        if violations:
            return _finish(
                records_path, rec, status=edit_record.STATUS_TOOL_VIOLATION,
                reason=REASON_TOOL_ISOLATION, error="tool_isolation_violation",
                violations=violations, **common,
            )

        patch = edit_patch.capture_patch(tree_dir, checkout)
        patch_hash = edit_patch.patch_hash(patch)
        patch_files = sorted(patch)
        if not patch:
            return _finish(
                records_path, rec, status=edit_record.STATUS_INVALID_PATCH,
                reason=REASON_EMPTY_PATCH, error="agent produced no patch",
                patch_hash=patch_hash, patch_files=patch_files, **common,
            )

        # Gates run AFTER the patch is captured. The behaviour + blast-radius
        # gate re-materialises the pristine tree and applies the AGENT patch;
        # the convention gate reads the agent's patched checkout. Neither ever
        # saw oracle material in the prompt or the tree.
        oracle_outcome = oracle_gate.evaluate_oracle(
            task, tree_dir, patch,
            behavior_command=behavior_command,
            existing_suite_command=existing_suite_command,
            impacted_symbols=impacted_symbols,
            lci=lci,
            timeout=gate_timeout,
            workspace_root=workspace_root,
        )
        conformance_outcome = conformance_gate.evaluate_task(task, manifest, checkout)
        gates = {"oracle": oracle_outcome, "conformance": conformance_outcome}

        passed = oracle_outcome["passed"] and conformance_outcome["passed"]
        if passed:
            return _finish(
                records_path, rec, status=edit_record.STATUS_PASSED,
                reason=REASON_ALL_GREEN, patch_hash=patch_hash,
                patch_files=patch_files, gates=gates, **common,
            )
        if not oracle_outcome["passed"]:
            detail = oracle_outcome["diagnostic"]
        else:
            detail = conformance_outcome["diagnostic"]
        return _finish(
            records_path, rec, status=edit_record.STATUS_GATE_FAILED,
            reason=REASON_GATE_FAILED, error=detail, patch_hash=patch_hash,
            patch_files=patch_files, gates=gates, **common,
        )
    finally:
        # Isolated throwaway worktree: removed on success AND failure.
        shutil.rmtree(checkout, ignore_errors=True)


def run_edit_task_both_arms(
    task, adapter_factory, base, *, corpus_root, records_path, work_root, **kwargs
):
    """Run both arms of one edit task. ``adapter_factory(arm)`` yields the arm's
    adapter (fake vs real) without ``run_edit_task`` knowing which."""
    return {
        arm: run_edit_task(
            task, arm, adapter_factory(arm), base,
            corpus_root=corpus_root, records_path=records_path,
            work_root=work_root, **kwargs,
        )
        for arm in (edit_toolsets.TREATMENT, edit_toolsets.BASELINE)
    }


def run_edit_bank(
    tasks, adapter_factory, base, *, corpus_root, records_path, work_root, **kwargs
):
    """Drive a whole task bank, both arms, in a DETERMINISTIC cell order:
    tasks sorted by id, arms (treatment, baseline). Resume-idempotent per cell.

    Yields the persisted record for each cell in order.
    """
    for task in sorted(tasks, key=lambda t: t["id"]):
        for arm in (edit_toolsets.TREATMENT, edit_toolsets.BASELINE):
            yield run_edit_task(
                task, arm, adapter_factory(arm), base,
                corpus_root=corpus_root, records_path=records_path,
                work_root=work_root, **kwargs,
            )
