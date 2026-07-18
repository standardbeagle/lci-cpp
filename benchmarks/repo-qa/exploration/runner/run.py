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
import hashlib
from dataclasses import dataclass
from datetime import datetime, timezone

from runner import corpus, gate, record, toolsets
from task_digest import task_digest

EXPLORATION_MODE = "exploration"
CLAIM_VALIDATION_MODE = "claim-validation"
EXPLORATION_SCHEMA = "exploration_answer_v1"
CLAIM_VALIDATION_SCHEMA = "claim_validation_answer_v1"
_VERDICTS = frozenset({"true", "false", "unsupported"})


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


def _claim_digest(task, schema_version):
    payload = {
        "schema": task["schema"], "id": task["id"],
        "claim": task["claim"], "request": task["request"],
        "manifest_ref": task["manifest_ref"], "output_schema": schema_version,
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def _base_record(task, arm, base, seed, key, mode, schema_version):
    ref = task["manifest_ref"]
    rec = {
        "run_key": key,
        "corpus_id": ref["corpus_id"],
        "source_commit": ref["source_commit"],
        "forge_version": ref["forge_version"],
        "seed": seed,
        "arm": arm,
        "model": base.model,
        "effective_allowlist": list(toolsets.arm_allowlist(arm)),
        "mode": mode,
        "schema_version": schema_version,
        "claim_task_digest": _claim_digest(task, schema_version),
        "sealed_metadata": {"task_id": task["id"],
                            "category": task.get("author", {}).get("category")},
    }
    if mode == EXPLORATION_MODE:
        # Preserve the original exploration record contract. Claim validation
        # deliberately keeps author-bearing task identity out of public fields.
        rec["task_id"] = task["id"]
        rec["task_digest"] = task_digest(task)
    return rec


def agent_visible_task(task):
    """The sole task payload exposed to an agent; never include author keys."""
    return {"claim": task["claim"], "request": task["request"]}


def run_task(task, arm, adapter, base, *, corpus_root, records_path, work_root,
             mode=EXPLORATION_MODE, schema_version=None):
    """Run one (task, arm). Returns the persisted record dict; on resume-skip
    returns the existing record annotated with skipped=True (adapter untouched)."""
    seed = task["manifest_ref"]["seed"]
    if mode not in (EXPLORATION_MODE, CLAIM_VALIDATION_MODE):
        raise ValueError(f"unknown runner mode {mode!r}")
    if schema_version is None:
        schema_version = (CLAIM_VALIDATION_SCHEMA if mode == CLAIM_VALIDATION_MODE
                          else EXPLORATION_SCHEMA)
    key = record.run_key(
        task["id"], arm, seed,
        mode if mode != EXPLORATION_MODE else None,
        schema_version if mode != EXPLORATION_MODE else None,
    )

    digest_field = ("claim_task_digest" if mode == CLAIM_VALIDATION_MODE
                    else "task_digest")
    digest = (_claim_digest(task, schema_version) if mode == CLAIM_VALIDATION_MODE
              else task_digest(task))
    existing = _find_record(records_path, key, digest, digest_field)
    if existing:
        existing["skipped"] = True
        return existing

    rec = _base_record(task, arm, base, seed, key, mode, schema_version)

    # Materialise + integrity-check the clean checkout. A drifted or missing
    # corpus is rejected loud, recorded as config_error, and the agent is never
    # invoked against a poisoned tree.
    # Both arms receive the same checkout contract. They execute sequentially,
    # and prepare_checkout refreshes this clean task/mode-specific copy.
    checkout_key = record.run_key(
        task["id"], "paired", seed,
        mode if mode != EXPLORATION_MODE else None,
        schema_version if mode != EXPLORATION_MODE else None,
    )
    checkout = os.path.join(work_root, "checkouts", _safe_name(checkout_key))
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

    public = agent_visible_task(task)
    prompt = f"Claim: {public['claim']}\n\nRequest: {public['request']}"
    if mode == CLAIM_VALIDATION_MODE:
        prompt += (
            "\n\nReturn only one JSON object with this exact shape: "
            '{"verdict":"true|false|unsupported","evidence":'
            '[{"path":"relative/file","line":1}],"rationale":"concise explanation"}. '
            "Cite only files in this checkout."
        )
    shared_instructions = (
        toolsets.CLAIM_VALIDATION_INSTRUCTIONS
        if mode == CLAIM_VALIDATION_MODE else None
    )
    request = toolsets.build_request(
        base, arm, checkout_dir, prompt,
        tool_instructions=shared_instructions,
    )
    started = _now()
    result = adapter.run(request)
    ended = _now()

    transcript_ref = _write_transcript(work_root, key, result.transcript)
    tool_calls = [{"name": c.name, "arguments": c.arguments} for c in result.tool_calls]
    token_usage = {"input": result.input_tokens, "output": result.output_tokens}

    structured_answer = None
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
        elif mode == CLAIM_VALIDATION_MODE:
            structured_answer, output_error = _parse_claim_answer(result.final_answer)
            if output_error:
                status, error = record.STATUS_MALFORMED_OUTPUT, output_error
            else:
                status, error = record.STATUS_ANSWERED, None
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
        structured_answer=structured_answer,
    )


def _finish(records_path, rec, *, status, manifest_id, started, ended, final_answer,
            tool_calls, token_usage, transcript_ref, error, violations,
            structured_answer=None):
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
    rec["structured_answer"] = structured_answer
    record.append_record(records_path, rec)
    return rec


def _find_record(records_path, key, digest, digest_field="task_digest"):
    match = None
    for rec in record.load_records(records_path):
        if (
            rec.get("run_key") == key
            and rec.get(digest_field) == digest
            and rec.get("status") in record.COMPLETED_STATUSES
        ):
            match = rec
    return dict(match) if match else None


def run_task_both_arms(task, adapter_factory, base, *, corpus_root, records_path, work_root,
                       mode=EXPLORATION_MODE, schema_version=None):
    """Run both arms of one task. `adapter_factory(arm)` yields the adapter for
    that arm, so the caller controls fake vs. real without run_task knowing."""
    return {
        arm: run_task(
            task, arm, adapter_factory(arm), base,
            corpus_root=corpus_root, records_path=records_path, work_root=work_root,
            mode=mode, schema_version=schema_version,
        )
        for arm in (toolsets.TREATMENT, toolsets.BASELINE)
    }


def _parse_claim_answer(raw):
    try:
        answer = json.loads(raw)
    except (json.JSONDecodeError, TypeError):
        return None, "malformed_claim_answer: invalid_json"
    if not isinstance(answer, dict) or set(answer) != {"verdict", "evidence", "rationale"}:
        return None, "malformed_claim_answer: fields"
    if answer["verdict"] not in _VERDICTS:
        return None, "malformed_claim_answer: verdict"
    if not isinstance(answer["rationale"], str) or not answer["rationale"].strip():
        return None, "malformed_claim_answer: rationale"
    evidence = answer["evidence"]
    if not isinstance(evidence, list) or not evidence:
        return None, "malformed_claim_answer: evidence"
    for citation in evidence:
        if (not isinstance(citation, dict) or set(citation) != {"path", "line"}
                or not isinstance(citation["path"], str) or not citation["path"]
                or os.path.isabs(citation["path"]) or ".." in citation["path"].split("/")
                or not isinstance(citation["line"], int)
                or isinstance(citation["line"], bool) or citation["line"] < 1
                or gate._is_sealed_path(citation["path"])):
            return None, "malformed_claim_answer: citation"
    return answer, None
